#include "gpu.hpp"
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace psx {
namespace {

inline int sext11(uint32_t v) { return int32_t(v << 21) >> 21; }
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

const int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

inline uint16_t pack555(int r, int g, int b) { return uint16_t((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10); }

}  // namespace

void Gpu::noteFramebuffer(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const std::array<int, 4> r{x, y, w, h};
    for (const auto& e : fbRects) if (e == r) return;
    if (fbRects.size() >= 12) fbRects.erase(fbRects.begin());
    fbRects.push_back(r);
}

bool Gpu::touchesFramebuffer(int x, int y, int w, int h) const {
    for (const auto& e : fbRects)
        if (x < e[0] + e[2] && x + w > e[0] && y < e[1] + e[3] && y + h > e[1]) return true;
    return false;
}

void Gpu::recordUpload(int x, int y, int w, int h) {
    if (!recording || w <= 0 || h <= 0 || x + w > kW || y + h > kH || !touchesFramebuffer(x, y, w, h)) return;
    GpuCmd c;
    c.kind = GpuCmd::Kind::Upload;
    c.rx = int16_t(x); c.ry = int16_t(y); c.rw = int16_t(w); c.rh = int16_t(h);
    c.poolOffset = uint32_t(cmdPool.size());
    for (int j = 0; j < h; ++j) cmdPool.insert(cmdPool.end(), vram.begin() + (y + j) * kW + x, vram.begin() + (y + j) * kW + x + w);
    cmds.push_back(c);
}

void Gpu::recordTri(const Vtx& a, const Vtx& b, const Vtx& c, bool textured, bool semiOn, bool raw, int clutX, int clutY) {
    GpuCmd g;
    const Vtx* v[3] = {&a, &b, &c};
    for (int i = 0; i < 3; ++i) {
        g.x[i] = v[i]->fx < 1e29f ? v[i]->fx : float(v[i]->x);
        g.y[i] = v[i]->fy < 1e29f ? v[i]->fy : float(v[i]->y);
        g.u[i] = float(v[i]->u); g.v[i] = float(v[i]->v);
        g.col[i][0] = uint8_t(v[i]->r); g.col[i][1] = uint8_t(v[i]->g); g.col[i][2] = uint8_t(v[i]->b);
    }
    if (a.fz > 0 && b.fz > 0 && c.fz > 0) { g.w[0] = a.fz; g.w[1] = b.fz; g.w[2] = c.fz; }
    g.textured = textured; g.raw = raw; g.semi = semiOn; g.semiMode = uint8_t(semi); g.depth = uint8_t(depth);
    g.tpx = uint8_t(tpX); g.tpy = uint8_t(tpY);
    g.twMX = uint8_t(twMaskX); g.twMY = uint8_t(twMaskY); g.twOX = uint8_t(twOffX); g.twOY = uint8_t(twOffY);
    g.clutX = uint16_t(clutX); g.clutY = uint16_t(clutY);
    g.area[0] = int16_t(areaX0); g.area[1] = int16_t(areaY0); g.area[2] = int16_t(areaX1); g.area[3] = int16_t(areaY1);
    if (alphaValue < 255 && !alphaKeys.empty() && !semiOn && alphaKeys.count(alphaKey(g.x[0], g.y[0])) && alphaKeys.count(alphaKey(g.x[1], g.y[1])) && alphaKeys.count(alphaKey(g.x[2], g.y[2]))) g.alpha = alphaValue;
    cmds.push_back(g);
    if (drawOffsets.empty() || drawOffsets.back()[0] != offX || drawOffsets.back()[1] != offY) {
        bool known = false;
        for (const auto& o : drawOffsets) known = known || (o[0] == offX && o[1] == offY);
        if (!known) drawOffsets.push_back({offX, offY});
    }
}

size_t Gpu::tagWorld(const std::vector<WorldPoint>& points, const std::function<bool(const uint64_t*, GpuCmd&)>& decide, std::array<int, 2>* offset) {
    static const bool dump = std::getenv("CB2_NWDUMP") != nullptr;
    static int tagCalls = 0;
    ++tagCalls;
    std::vector<std::unordered_map<uint64_t, uint64_t>> keys(drawOffsets.size());
    for (size_t i = 0; i < drawOffsets.size(); ++i)
        for (const auto& p : points) keys[i][alphaKey(p.x + float(drawOffsets[i][0]), p.y + float(drawOffsets[i][1]))] = p.id;
    size_t n = 0;
    for (GpuCmd& c : cmds) {
        if (c.kind != GpuCmd::Kind::Tri) continue;
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto a = keys[i].find(alphaKey(c.x[0], c.y[0])), b = keys[i].find(alphaKey(c.x[1], c.y[1])), d = keys[i].find(alphaKey(c.x[2], c.y[2]));
            if (a == keys[i].end() || b == keys[i].end() || d == keys[i].end()) continue;
            const uint64_t ids[3] = {a->second, b->second, d->second};
            const bool take = decide(ids, c);
            if (dump && tagCalls == 40) std::printf("[nwdump] tagged tri %s center %.0f,%.0f w %.0f textured %d semi %d clut %d,%d\n", take ? "CLAIMED" : "refused", (c.x[0] + c.x[1] + c.x[2]) / 3, (c.y[0] + c.y[1] + c.y[2]) / 3, c.w[0], c.textured, c.semi, c.clutX, c.clutY);
            if (!take) break;
            if (!n && offset) *offset = drawOffsets[i];
            c.world = true;
            c.worldOff[0] = int16_t(drawOffsets[i][0]);
            c.worldOff[1] = int16_t(drawOffsets[i][1]);
            ++n;
            break;
        }
    }
    {
        if (dump && tagCalls == 40) {
            for (const GpuCmd& c : cmds) if (c.kind == GpuCmd::Kind::Tri && !c.world) std::printf("[nwdump] game tri center %.0f,%.0f w %.0f textured %d semi %d\n", (c.x[0] + c.x[1] + c.x[2]) / 3, (c.y[0] + c.y[1] + c.y[2]) / 3, c.w[0], c.textured, c.semi);
        }
    }
    if (offset) drawOffsets.clear();   // the last of a series of calls
    return n;
}

void Gpu::recordRect(int x, int y, int w, int h, int r, int g, int b, bool textured, bool semiOn, bool raw, int u0, int v0, int clutX, int clutY) {
    // Sprite texel coordinates: pixel i samples u0 + i (u0 - i when mirrored), so the corners are offset to make the
    // value at the pixel centre land on that texel.
    const float uL = rectFlipX ? float(u0 + 1) : float(u0), uR = rectFlipX ? float(u0 + 1 - w) : float(u0 + w);
    const float vT = rectFlipY ? float(v0 + 1) : float(v0), vB = rectFlipY ? float(v0 + 1 - h) : float(v0 + h);
    Vtx q[4] = {{x, y, r, g, b, 0, 0}, {x + w, y, r, g, b, 0, 0}, {x, y + h, r, g, b, 0, 0}, {x + w, y + h, r, g, b, 0, 0}};
    const float us[4] = {uL, uR, uL, uR}, vs[4] = {vT, vT, vB, vB};
    const int tri[2][3] = {{0, 1, 2}, {1, 2, 3}};
    for (const auto& t : tri) {
        recordTri(q[t[0]], q[t[1]], q[t[2]], textured, semiOn, raw, clutX, clutY);
        GpuCmd& c = cmds.back();
        for (int i = 0; i < 3; ++i) { c.u[i] = us[t[i]]; c.v[i] = vs[t[i]]; }
    }
}

void Gpu::setTexpage(uint32_t v) {
    tpX = v & 0xF;
    tpY = (v >> 4) & 1;
    semi = (v >> 5) & 3;
    depth = (v >> 7) & 3;
    dither = (v >> 9) & 1;
    drawToDisplay = (v >> 10) & 1;
    texDisable = (v >> 11) & 1;
    rectFlipX = (v >> 12) & 1;
    rectFlipY = (v >> 13) & 1;
}

uint32_t Gpu::stat() {
    uint32_t s = uint32_t(tpX) | uint32_t(tpY) << 4 | uint32_t(semi) << 5 | uint32_t(depth) << 7 | uint32_t(dither) << 9 |
                 uint32_t(drawToDisplay) << 10 | uint32_t(setMask) << 11 | uint32_t(checkMask) << 12 | 1u << 13 | uint32_t(texDisable) << 15;
    int hr = hres == 256 ? 0 : hres == 320 ? 1 : hres == 512 ? 2 : hres == 640 ? 3 : 1;
    if (hres == 368) s |= 1u << 16;
    s |= uint32_t(hr) << 17;
    s |= uint32_t(vres == 480) << 19 | uint32_t(pal) << 20 | uint32_t(color24) << 21 | uint32_t(interlaced) << 22 | uint32_t(!displayEnabled) << 23;
    s |= 1u << 26 | 1u << 28;
    if (xfer == Xfer::FromVram) s |= 1u << 27;
    s |= uint32_t(dir) << 29;
    uint32_t req = 0;
    if (dir == 1) req = 1; else if (dir == 2) req = (s >> 28) & 1; else if (dir == 3) req = (s >> 27) & 1;
    s |= req << 25;
    if (!interlaced || vres != 480) s |= uint32_t(field) << 31; else s |= uint32_t(field) << 31;
    return s;
}

uint32_t Gpu::read() {
    if (xfer == Xfer::FromVram) {
        uint32_t w = 0;
        for (int i = 0; i < 2; ++i) {
            const int x = (xX + xPos % xW) & 1023, y = (xY + xPos / xW) & 511;
            w |= uint32_t(vram[size_t(y) * kW + size_t(x)]) << (16 * i);
            ++xPos;
        }
        if (xPos >= xW * xH) xfer = Xfer::None;
        readLatch = w;
        return w;
    }
    return infoLatch;
}

void Gpu::gp1(uint32_t w) {
    const uint32_t cmd = w >> 24;
    switch (cmd) {
    case 0x00:
        displayEnabled = false; dir = 0; fifo.clear(); need = 0; xfer = Xfer::None;
        setTexpage(0); twMaskX = twMaskY = twOffX = twOffY = 0;
        areaX0 = areaY0 = 0; areaX1 = 1023; areaY1 = 511; offX = offY = 0; setMask = checkMask = false;
        break;
    case 0x01: fifo.clear(); need = 0; xfer = Xfer::None; break;
    case 0x03: displayEnabled = (w & 1) == 0; break;
    case 0x04: dir = int(w & 3); break;
    case 0x05: {
        const int nx = int(w & 0x3FF), ny = int((w >> 10) & 0x1FF);
        if (recording && (nx != displayX || ny != displayY)) {
            // the frame drawn since the last flip is now the one on screen (see FrameInterpolator)
            GpuCmd f;
            f.kind = GpuCmd::Kind::Flip;
            f.rx = int16_t(nx); f.ry = int16_t(ny); f.rw = int16_t(hres); f.rh = int16_t(vres);
            f.color = uint32_t(vblankCount);
            cmds.push_back(f);
            ++flipCount;
        }
        displayX = nx; displayY = ny;
        noteFramebuffer(displayX, displayY, hres, vres);
        break;
    }
    case 0x08: {
        static const int hr[4] = {256, 320, 512, 640};
        hres = (w & 0x40) ? 368 : hr[w & 3];
        vres = (w & 4) ? 480 : 240;
        pal = (w >> 3) & 1;
        color24 = (w >> 4) & 1;
        interlaced = (w >> 5) & 1;
        break;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (w & 0xF) {
        case 2: infoLatch = uint32_t(twMaskX | twMaskY << 5 | twOffX << 10 | twOffY << 15); break;
        case 3: infoLatch = uint32_t(areaX0 | areaY0 << 10); break;
        case 4: infoLatch = uint32_t(areaX1 | areaY1 << 10); break;
        case 5: infoLatch = uint32_t(offX & 0x7FF) | uint32_t(offY & 0x7FF) << 11; break;
        case 7: infoLatch = 2; break;
        default: break;
        }
        break;
    default: break;
    }
}

uint32_t Gpu::commandLength(uint32_t cmd) const {
    if (cmd >= 0x20 && cmd < 0x40) {
        const uint32_t nv = (cmd & 8) ? 4 : 3, tex = (cmd >> 2) & 1, gour = (cmd >> 4) & 1;
        return 1 + (nv - 1) * gour + nv + nv * tex;
    }
    if (cmd >= 0x40 && cmd < 0x60) return (cmd & 0x10) ? 4 : 3;
    if (cmd >= 0x60 && cmd < 0x80) {
        const uint32_t size = (cmd >> 3) & 3, tex = (cmd >> 2) & 1;
        return 2 + tex + (size == 0 ? 1 : 0);
    }
    if (cmd == 0x02) return 3;
    if (cmd >= 0x80 && cmd < 0xA0) return 4;
    if ((cmd >= 0xA0 && cmd < 0xC0) || (cmd >= 0xC0 && cmd < 0xE0)) return 3;
    return 1;
}

void Gpu::gp0(uint32_t w) {
    if (xfer == Xfer::ToVram) {
        for (int i = 0; i < 2; ++i) {
            const int x = (xX + xPos % xW) & 1023, y = (xY + xPos / xW) & 511;
            const uint16_t px = uint16_t(w >> (16 * i));
            uint16_t& dst = vram[size_t(y) * kW + size_t(x)];
            if (!(checkMask && (dst & 0x8000))) dst = uint16_t(px | (setMask ? 0x8000 : 0));
            ++xPos;
            if (xPos >= xW * xH) { xfer = Xfer::None; recordUpload(xX, xY, xW, xH); break; }
        }
        return;
    }
    if (polyLine) {
        const bool gouraud = (fifo[0] >> 24) & 0x10;
        if ((w & 0xF000F000u) == 0x50005000u && fifo.size() >= (gouraud ? 4u : 3u)) {
            execute();
            fifo.clear(); polyLine = false; need = 0;
        } else {
            fifo.push_back(w);
        }
        return;
    }
    if (fifo.empty()) {
        const uint32_t cmd = w >> 24;
        need = commandLength(cmd);
        if (cmd >= 0x40 && cmd < 0x60 && (cmd & 8)) { polyLine = true; fifo.push_back(w); return; }
        if (need == 1 && !(cmd == 0x00 || cmd == 0x01 || cmd == 0x1F || (cmd >= 0xE1 && cmd <= 0xE6))) {
            ++unknownCommands;
            return;
        }
    }
    fifo.push_back(w);
    if (fifo.size() >= need) {
        execute();
        fifo.clear();
        need = 0;
    }
}

void Gpu::putPixel(int x, int y, int r, int g, int b, bool semiOn, bool texSemi) {
    if (x < areaX0 || x > areaX1 || y < areaY0 || y > areaY1) return;
    x &= 1023; y &= 511;
    uint16_t& dst = vram[size_t(y) * kW + size_t(x)];
    if (checkMask && (dst & 0x8000)) return;
    if (semiOn) {
        const int br = (dst & 31) << 3, bg = ((dst >> 5) & 31) << 3, bb = ((dst >> 10) & 31) << 3;
        switch (semi) {
        case 0: r = (br + r) / 2; g = (bg + g) / 2; b = (bb + b) / 2; break;
        case 1: r = br + r; g = bg + g; b = bb + b; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + r / 4; g = bg + g / 4; b = bb + b / 4; break;
        }
        r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255);
    }
    dst = uint16_t(pack555(r, g, b) | ((setMask || texSemi) ? 0x8000 : 0));
}

uint16_t Gpu::texel(int u, int v, int clutX, int clutY, bool& transparent) const {
    u = (u & ~(twMaskX * 8)) | ((twOffX & twMaskX) * 8);
    v = (v & ~(twMaskY * 8)) | ((twOffY & twMaskY) * 8);
    u &= 255; v &= 255;
    const int base = tpX * 64, row = ((tpY * 256 + v) & 511) * kW;
    uint16_t c;
    if (depth == 0) {
        const uint16_t w = vram[size_t(row + ((base + u / 4) & 1023))];
        const int idx = (w >> ((u & 3) * 4)) & 0xF;
        c = vram[size_t(clutY * kW + ((clutX + idx) & 1023))];
    } else if (depth == 1) {
        const uint16_t w = vram[size_t(row + ((base + u / 2) & 1023))];
        const int idx = (w >> ((u & 1) * 8)) & 0xFF;
        c = vram[size_t(clutY * kW + ((clutX + idx) & 1023))];
    } else {
        c = vram[size_t(row + ((base + u) & 1023))];
    }
    transparent = c == 0;
    return c;
}

void Gpu::drawTriangle(const Vtx& a0, const Vtx& b0, const Vtx& c0, bool textured, bool gouraud, bool semiOn, bool raw, int clutX, int clutY) {
    Vtx a = a0, b = b0, c = c0;
    if (std::abs(a.x - b.x) > 1023 || std::abs(b.x - c.x) > 1023 || std::abs(a.x - c.x) > 1023 ||
        std::abs(a.y - b.y) > 511 || std::abs(b.y - c.y) > 511 || std::abs(a.y - c.y) > 511) return;
    int64_t area = int64_t(b.x - a.x) * (c.y - a.y) - int64_t(b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (recording) recordTri(a0, b0, c0, textured, semiOn, raw, clutX, clutY);
    if (!rasterize) { ++primitives; return; }
    if (area < 0) { std::swap(b, c); area = -area; }
    const int minX = std::max(std::min({a.x, b.x, c.x}), areaX0), maxX = std::min(std::max({a.x, b.x, c.x}), areaX1);
    const int minY = std::max(std::min({a.y, b.y, c.y}), areaY0), maxY = std::min(std::max({a.y, b.y, c.y}), areaY1);
    auto edge = [](const Vtx& p, const Vtx& q, int x, int y) { return int64_t(q.x - p.x) * (y - p.y) - int64_t(q.y - p.y) * (x - p.x); };
    auto inclusive = [](const Vtx& p, const Vtx& q) { const int dy = q.y - p.y, dx = q.x - p.x; return dy < 0 || (dy == 0 && dx > 0); };
    const bool i0 = inclusive(b, c), i1 = inclusive(c, a), i2 = inclusive(a, b);
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const int64_t w0 = edge(b, c, x, y), w1 = edge(c, a, x, y), w2 = edge(a, b, x, y);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !i0) || (w1 == 0 && !i1) || (w2 == 0 && !i2)) continue;
            int r, g, bl;
            if (gouraud) {
                r = int((w0 * a.r + w1 * b.r + w2 * c.r) / area);
                g = int((w0 * a.g + w1 * b.g + w2 * c.g) / area);
                bl = int((w0 * a.b + w1 * b.b + w2 * c.b) / area);
            } else { r = a.r; g = a.g; bl = a.b; }
            bool texSemi = false;
            if (textured) {
                const int u = int((w0 * a.u + w1 * b.u + w2 * c.u) / area);
                const int v = int((w0 * a.v + w1 * b.v + w2 * c.v) / area);
                bool transparent;
                const uint16_t t = texel(u, v, clutX, clutY, transparent);
                if (transparent) continue;
                int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
                if (!raw) {
                    tr = std::min(255, tr * r / 128); tg = std::min(255, tg * g / 128); tb = std::min(255, tb * bl / 128);
                    if (dither) {
                        const int d = kDither[y & 3][x & 3];
                        tr = clampi(tr + d, 0, 255); tg = clampi(tg + d, 0, 255); tb = clampi(tb + d, 0, 255);
                    }
                }
                r = tr; g = tg; bl = tb;
                texSemi = (t & 0x8000) != 0;
                putPixel(x, y, r, g, bl, semiOn && texSemi, texSemi);
            } else {
                if (dither && gouraud) {
                    const int d = kDither[y & 3][x & 3];
                    r = clampi(r + d, 0, 255); g = clampi(g + d, 0, 255); bl = clampi(bl + d, 0, 255);
                }
                putPixel(x, y, r, g, bl, semiOn, false);
            }
        }
    }
    ++primitives;
}

void Gpu::drawRect(int x, int y, int w, int h, int r, int g, int b, bool textured, bool semiOn, bool raw, int u0, int v0, int clutX, int clutY) {
    if (recording && w > 0 && h > 0 && w <= 1024 && h <= 512) recordRect(x, y, w, h, r, g, b, textured, semiOn, raw, u0, v0, clutX, clutY);
    if (!rasterize) { ++primitives; return; }
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const int px = x + i, py = y + j;
            if (px < areaX0 || px > areaX1 || py < areaY0 || py > areaY1) continue;
            if (!textured) { putPixel(px, py, r, g, b, semiOn, false); continue; }
            const int u = rectFlipX ? u0 - i : u0 + i, v = rectFlipY ? v0 - j : v0 + j;
            bool transparent;
            const uint16_t t = texel(u, v, clutX, clutY, transparent);
            if (transparent) continue;
            int tr = (t & 31) << 3, tg = ((t >> 5) & 31) << 3, tb = ((t >> 10) & 31) << 3;
            if (!raw) { tr = std::min(255, tr * r / 128); tg = std::min(255, tg * g / 128); tb = std::min(255, tb * b / 128); }
            const bool texSemi = (t & 0x8000) != 0;
            putPixel(px, py, tr, tg, tb, semiOn && texSemi, texSemi);
        }
    }
    ++primitives;
}

void Gpu::drawLine(const Vtx& a, const Vtx& b, bool gouraud, bool semiOn) {
    const int dx = b.x - a.x, dy = b.y - a.y;
    if (std::abs(dx) > 1023 || std::abs(dy) > 511) return;
    if (recording) {
        // a one-pixel wide quad centred on the line
        const float len = std::max(1.0f, std::sqrt(float(dx * dx + dy * dy)));
        const float nx = -float(dy) / len * 0.5f, ny = float(dx) / len * 0.5f;
        const float ax = a.x + 0.5f, ay = a.y + 0.5f, bx = b.x + 0.5f, by = b.y + 0.5f;
        const float qx[4] = {ax - nx, ax + nx, bx - nx, bx + nx}, qy[4] = {ay - ny, ay + ny, by - ny, by + ny};
        const Vtx cv[4] = {a, a, b, b};
        const int tri[2][3] = {{0, 1, 2}, {1, 2, 3}};
        for (const auto& t : tri) {
            recordTri(cv[t[0]], cv[t[1]], cv[t[2]], false, semiOn, false, 0, 0);
            GpuCmd& c = cmds.back();
            for (int i = 0; i < 3; ++i) {
                c.x[i] = qx[t[i]]; c.y[i] = qy[t[i]];
                if (!gouraud) for (int k = 0; k < 3; ++k) c.col[i][k] = uint8_t(k == 0 ? a.r : k == 1 ? a.g : a.b);
            }
        }
    }
    if (!rasterize) { ++primitives; return; }
    const int n = std::max(std::abs(dx), std::abs(dy));
    for (int i = 0; i <= n; ++i) {
        const int x = n ? a.x + (dx * i + (dx >= 0 ? n / 2 : -n / 2)) / n : a.x;
        const int y = n ? a.y + (dy * i + (dy >= 0 ? n / 2 : -n / 2)) / n : a.y;
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud && n) { r = a.r + (b.r - a.r) * i / n; g = a.g + (b.g - a.g) * i / n; bl = a.b + (b.b - a.b) * i / n; }
        putPixel(x, y, r, g, bl, semiOn, false);
    }
    ++primitives;
}

void Gpu::fillRect(uint32_t color, uint32_t xy, uint32_t wh) {
    const int x = int(xy & 0x3F0), y = int((xy >> 16) & 0x1FF);
    const int w = int(((wh & 0x3FF) + 0xF) & ~0xFu), h = int((wh >> 16) & 0x1FF);
    const uint16_t c = pack555(int(color & 0xFF), int((color >> 8) & 0xFF), int((color >> 16) & 0xFF));
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) vram[size_t((y + j) & 511) * kW + size_t((x + i) & 1023)] = c;
    if (recording && w > 0 && h > 0) {
        GpuCmd g;
        g.kind = GpuCmd::Kind::Fill;
        g.rx = int16_t(x); g.ry = int16_t(y); g.rw = int16_t(w); g.rh = int16_t(h);
        g.color = color & 0xFFFFFF;
        cmds.push_back(g);
    }
}

void Gpu::copyRect(uint32_t src, uint32_t dst, uint32_t wh) {
    const int sx = int(src & 0x3FF), sy = int((src >> 16) & 0x1FF), dx = int(dst & 0x3FF), dy = int((dst >> 16) & 0x1FF);
    int w = int(wh & 0x3FF), h = int((wh >> 16) & 0x1FF);
    if (w == 0) w = 1024;
    if (h == 0) h = 512;
    std::vector<uint16_t> tmp(size_t(w) * size_t(h));
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) tmp[size_t(j) * size_t(w) + size_t(i)] = vram[size_t((sy + j) & 511) * kW + size_t((sx + i) & 1023)];
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            uint16_t& d = vram[size_t((dy + j) & 511) * kW + size_t((dx + i) & 1023)];
            if (!(checkMask && (d & 0x8000))) d = uint16_t(tmp[size_t(j) * size_t(w) + size_t(i)] | (setMask ? 0x8000 : 0));
        }
    if (dx + w <= kW && dy + h <= kH) recordUpload(dx, dy, w, h);
}

void Gpu::execute() {
    const uint32_t cmd = fifo[0] >> 24;
    ++drawCalls;
    if (cmd >= 0x20 && cmd < 0x40) {
        const bool quad = cmd & 8, tex = cmd & 4, gour = cmd & 0x10, semiOn = cmd & 2, raw = cmd & 1;
        const int nv = quad ? 4 : 3;
        Vtx v[4] = {};
        size_t p = 0;
        int clutX = 0, clutY = 0;
        const uint32_t c0 = fifo[0];
        for (int i = 0; i < nv; ++i) {
            const uint32_t col = (i == 0 || !gour) ? c0 : fifo[p];
            if (i == 0) p = 1; else if (gour) ++p;
            v[i].r = int(col & 0xFF); v[i].g = int((col >> 8) & 0xFF); v[i].b = int((col >> 16) & 0xFF);
            const uint32_t xy = fifo[p++];
            v[i].x = sext11(xy & 0x7FF) + offX; v[i].y = sext11((xy >> 16) & 0x7FF) + offY;
            if (pgxp && recording) {
                auto it = pgxp->find(xy);
                if (it != pgxp->end()) { v[i].fx = it->second[0] + float(offX); v[i].fy = it->second[1] + float(offY); v[i].fz = it->second[2]; }
            }
            if (tex) {
                const uint32_t uv = fifo[p++];
                v[i].u = int(uv & 0xFF); v[i].v = int((uv >> 8) & 0xFF);
                if (i == 0) { clutX = int(uv >> 16 & 0x3F) * 16; clutY = int((uv >> 22) & 0x1FF); }
                if (i == 1) setTexpage(uv >> 16);
            }
        }
        drawTriangle(v[0], v[1], v[2], tex, gour, semiOn, raw, clutX, clutY);
        if (quad) drawTriangle(v[1], v[2], v[3], tex, gour, semiOn, raw, clutX, clutY);
        return;
    }
    if (cmd >= 0x40 && cmd < 0x60) {
        const bool gour = cmd & 0x10, semiOn = cmd & 2;
        std::vector<Vtx> pts;
        size_t p = 0;
        uint32_t col = fifo[0];
        p = 1;
        while (p < fifo.size()) {
            Vtx v{};
            v.r = int(col & 0xFF); v.g = int((col >> 8) & 0xFF); v.b = int((col >> 16) & 0xFF);
            const uint32_t xy = fifo[p++];
            v.x = sext11(xy & 0x7FF) + offX; v.y = sext11((xy >> 16) & 0x7FF) + offY;
            pts.push_back(v);
            if (gour && p < fifo.size()) col = fifo[p++];
        }
        for (size_t i = 0; i + 1 < pts.size(); ++i) drawLine(pts[i], pts[i + 1], gour, semiOn);
        return;
    }
    if (cmd >= 0x60 && cmd < 0x80) {
        const bool tex = cmd & 4, semiOn = cmd & 2, raw = cmd & 1;
        const uint32_t size = (cmd >> 3) & 3;
        size_t p = 1;
        const uint32_t xy = fifo[p++];
        int u = 0, v = 0, clutX = 0, clutY = 0;
        if (tex) { const uint32_t uv = fifo[p++]; u = int(uv & 0xFF); v = int((uv >> 8) & 0xFF); clutX = int(uv >> 16 & 0x3F) * 16; clutY = int((uv >> 22) & 0x1FF); }
        int w = size == 1 ? 1 : size == 2 ? 8 : 16, h = w;
        if (size == 0) { const uint32_t wh = fifo[p++]; w = int(wh & 0x3FF); h = int((wh >> 16) & 0x1FF); }
        const uint32_t col = fifo[0];
        drawRect(sext11(xy & 0x7FF) + offX, sext11((xy >> 16) & 0x7FF) + offY, w, h, int(col & 0xFF), int((col >> 8) & 0xFF), int((col >> 16) & 0xFF), tex, semiOn, raw, u, v, clutX, clutY);
        return;
    }
    switch (cmd) {
    case 0x02: fillRect(fifo[0], fifo[1], fifo[2]); break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        copyRect(fifo[1], fifo[2], fifo[3]);
        break;
    case 0xA0: case 0xC0: {
        xX = int(fifo[1] & 0x3FF); xY = int((fifo[1] >> 16) & 0x1FF);
        xW = int(fifo[2] & 0x3FF); xH = int((fifo[2] >> 16) & 0x1FF);
        if (xW == 0) xW = 1024;
        if (xH == 0) xH = 512;
        xPos = 0;
        xfer = cmd == 0xA0 ? Xfer::ToVram : Xfer::FromVram;
        break;
    }
    case 0xE1: setTexpage(fifo[0]); break;
    case 0xE2:
        twMaskX = int(fifo[0] & 31); twMaskY = int((fifo[0] >> 5) & 31); twOffX = int((fifo[0] >> 10) & 31); twOffY = int((fifo[0] >> 15) & 31);
        break;
    case 0xE3: areaX0 = int(fifo[0] & 0x3FF); areaY0 = int((fifo[0] >> 10) & 0x1FF); break;
    case 0xE4: areaX1 = int(fifo[0] & 0x3FF); areaY1 = int((fifo[0] >> 10) & 0x1FF); noteFramebuffer(areaX0, areaY0, areaX1 - areaX0 + 1, areaY1 - areaY0 + 1); break;
    case 0xE5: offX = sext11(fifo[0] & 0x7FF); offY = sext11((fifo[0] >> 11) & 0x7FF); break;
    case 0xE6: setMask = fifo[0] & 1; checkMask = (fifo[0] >> 1) & 1; break;
    default: break;
    }
}

}  // namespace psx
