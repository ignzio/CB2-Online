#include "model.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace cb2 {
namespace {

uint32_t u32(const Bytes& b, size_t o) { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; }
int32_t i32(const Bytes& b, size_t o) { return int32_t(u32(b, o)); }
int16_t i16(const Bytes& b, size_t o) { int16_t v = 0; if (o + 2 <= b.size()) std::memcpy(&v, &b[o], 2); return v; }

constexpr uint8_t kNullKey = 0x57;

// One entry of the polygon list: either a triangle record or a pair of colour indices shared by following triangles.
struct Poly {
    bool isColor = false;
    int color1 = 0, color2 = 0;              // colour record
    int texture = 0, color = 0, key = 0;     // triangle record
    bool animated = false, duplicate = false;
    int triType = 0, triSubtype = 0;
};

bool isFooter(uint32_t w) { return w == 0xFFFFFFFFu; }

Poly decodePoly(uint32_t w) {
    Poly p;
    if ((w & 0xFFFF0000u) != 0) {
        p.texture = w & 0xFF;
        p.animated = (w >> 8) & 1;
        p.color = (w >> 9) & 0x7F;
        p.key = (w >> 16) & 0xFF;
        p.duplicate = (w >> 26) & 1;
        const int t = int(w >> 28);
        p.triSubtype = t & 3;
        p.triType = (t >> 2) & 3;
    } else {
        p.isColor = true;
        p.color1 = (w >> 2) & 0x7F;
        p.color2 = (w >> 9) & 0x7F;
    }
    return p;
}

}  // namespace

std::optional<Model> parseModel(const Entry& e) {
    if (e.type != 2 || (e.items.size() != 5 && e.items.size() != 6) || e.items[0].size() != 80) return std::nullopt;
    const Bytes& h = e.items[0];
    const uint32_t polyCount = u32(h, 0x2C), colorCount = u32(h, 0x3C), texCount = u32(h, 0x34), animCount = u32(h, 0x48);
    if (e.items[1].size() != polyCount * 4 || e.items[2].size() != colorCount * 4 || e.items[3].size() != texCount * 12 ||
        e.items[4].size() != animCount * 4)
        return std::nullopt;

    Model m;
    m.eid = e.eid;
    for (int i = 0; i < 3; ++i) m.scale[i] = i32(h, 4 * size_t(i));
    m.specialVertices = i32(h, 0x4C);
    m.vertexCount = i32(h, 0x38);
    const uint32_t tpageCount = u32(h, 0x40);
    for (uint32_t i = 0; i < tpageCount && i < 8; ++i) m.tpages.push_back(u32(h, 0xC + 4 * size_t(i)));
    for (uint32_t i = 0; i < colorCount; ++i) {
        const uint8_t* c = &e.items[2][i * 4];
        m.colors.push_back({c[0], c[1], c[2], c[3]});
    }
    for (uint32_t i = 0; i < texCount; ++i) m.textures.push_back(parseTexInfoBytes(&e.items[3][i * 12]));
    for (uint32_t i = 0; i < animCount; ++i) m.animatedTextures.push_back(i32(e.items[4], i * 4));

    m.compressed = e.items.size() == 6;
    if (m.compressed) {
        const int count = m.vertexCount - m.specialVertices;
        if (count < 0 || e.items[5].size() != size_t(count) * 4) return std::nullopt;
        for (int i = 0; i < count; ++i) {
            const int s = i32(e.items[5], size_t(i) * 4);
            m.positions.push_back({uint8_t((s >> 25) & 0x7F), uint8_t(s >> 9), uint8_t(s >> 17), uint8_t((s >> 6) & 7), uint8_t(s & 7),
                                   uint8_t((s >> 3) & 7)});
            // fields: x = bits 25-31, z = bits 17-24, y = bits 9-16; x/z/y bit widths in bits 6-8, 3-5, 0-2
        }
    }

    // Pass 1: resolve vertex numbers. Original records take the next vertex number; duplicates reuse an earlier one.
    std::vector<Poly> polys;
    std::vector<int> vtx;
    std::map<int, int> byKey;
    int v = m.compressed ? -m.specialVertices : 0;
    for (uint32_t pi = 0; pi < polyCount; ++pi) {
        const uint32_t w = u32(e.items[1], size_t(pi) * 4);
        if (isFooter(w)) break;
        Poly p = decodePoly(w);
        if (!p.isColor) {
            if (!p.duplicate) {
                if (p.key != kNullKey) byKey[p.key] = v;
                vtx.push_back(v++);
            } else {
                const auto it = byKey.find(p.key);
                vtx.push_back(it == byKey.end() ? 0 : it->second);
            }
        }
        polys.push_back(p);
    }

    // Pass 2: build triangles from the strip-like polygon records.
    auto colorOf = [&](int i) { return i >= 0 && i < int(polys.size()) && !polys[size_t(i)].isColor ? polys[size_t(i)].color : 0; };
    auto vert = [&](int i) { return i >= 0 && i < int(vtx.size()) ? vtx[size_t(i)] : 0; };
    int lastColor = -1, lastCC = -1, lastAA = -1, lastNonBB = -1, lastValidCC = -3, curV = 0;
    for (int i = 0; i < int(polys.size()); ++i) {
        const Poly& p = polys[size_t(i)];
        if (p.isColor) { lastColor = i; continue; }
        ModelTri t;
        t.texture = p.texture;
        t.type = p.triType;
        t.subtype = p.triSubtype;
        t.animated = p.animated;
        const Poly* lc = lastColor >= 0 ? &polys[size_t(lastColor)] : nullptr;
        bool emit = false;
        switch (p.triType) {
        case 0:
            lastAA = curV;
            lastNonBB = i;
            t.v[0] = vert(curV); t.v[1] = vert(curV - 1); t.v[2] = vert(curV - 2);
            t.c[0] = p.color;
            t.c[1] = lastColor + 1 == i ? (lc ? lc->color1 : 0) : colorOf(i - 1);
            t.c[2] = lastColor + 1 == i ? (lc ? lc->color2 : 0) : (lastColor + 2 == i ? (lc ? lc->color1 : 0) : colorOf(i - 2));
            emit = curV >= 2;
            break;
        case 1: {
            int ci = -1;
            if (lastColor < lastNonBB - 2) ci = colorOf(lastNonBB - 2);
            else if (lastColor == lastNonBB - 2) ci = lc ? lc->color1 : 0;
            else if (lastColor == lastNonBB - 1) ci = lc ? lc->color2 : 0;
            else ci = lc ? lc->color2 : 0;
            t.v[0] = vert(curV); t.v[1] = vert(curV - 1);
            t.v[2] = lastCC > lastAA ? vert(lastCC) : vert(lastAA - 2);
            t.c[0] = p.color;
            t.c[1] = lastColor + 1 == i ? (lc ? lc->color1 : 0) : colorOf(i - 1);
            t.c[2] = ci;
            emit = curV >= 1 && lastAA >= 2;
            break;
        }
        case 2:
            if (i + 2 < int(polys.size()) && lastValidCC + 2 < i) {
                lastValidCC = i;
                t.v[0] = vert(curV); t.v[1] = vert(curV + 1); t.v[2] = vert(curV + 2);
                t.c[0] = p.color; t.c[1] = colorOf(i + 1); t.c[2] = colorOf(i + 2);
                emit = true;
            }
            lastCC = curV;
            lastNonBB = i;
            break;
        default: break;
        }
        if (emit) m.tris.push_back(t);
        ++curV;
    }
    return m;
}

std::optional<Animation> parseAnimation(const Entry& e) {
    if (e.type != 1 || e.items.empty()) return std::nullopt;
    Animation a;
    a.eid = e.eid;
    for (const Bytes& d : e.items) {
        if (d.size() < 24) return std::nullopt;
        AnimFrame f;
        f.xOffset = i16(d, 0); f.yOffset = i16(d, 2); f.zOffset = i16(d, 4);
        f.vertexCount = i32(d, 8);
        const int collisionCount = i32(d, 12);
        f.modelEid = u32(d, 16);
        const int headerSize = i32(d, 20);
        const int fake = 24 + collisionCount * 0x28;
        if (f.vertexCount < 0 || f.vertexCount > 0x10000 || collisionCount < 0 || headerSize < fake || size_t(headerSize) > d.size() ||
            (d.size() - size_t(fake)) % 4 != 0)
            return std::nullopt;
        f.specialVertices = (headerSize - fake) / 3;
        f.bitsStart = size_t(fake);
        f.data = d;
        a.frames.push_back(std::move(f));
    }
    return a;
}

bool frameVertices(const AnimFrame& f, const Model& m, std::vector<std::array<int, 3>>& out) {
    out.assign(size_t(f.vertexCount), {0, 0, 0});
    const Bytes& d = f.data;
    const size_t totalBits = (d.size() - f.bitsStart) * 8;
    size_t bit = size_t(f.specialVertices) * 24;
    auto read = [&](int n) {   // bits are taken from the most significant end of each little-endian 32-bit word
        int v = 0;
        for (int i = 0; i < n; ++i, ++bit) {
            const uint32_t w = u32(d, f.bitsStart + (bit / 32) * 4);
            v = (v << 1) | int((w >> (31 - bit % 32)) & 1);
        }
        return v;
    };
    for (int i = 0; i < f.specialVertices && i < f.vertexCount; ++i)
        out[size_t(i)] = {d[f.bitsStart + size_t(i) * 3], d[f.bitsStart + size_t(i) * 3 + 1], d[f.bitsStart + size_t(i) * 3 + 2]};

    if (m.compressed) {
        if (size_t(m.positions.size()) > size_t(f.vertexCount)) return false;
        int ax = 0, ay = 0, az = 0;
        for (size_t i = 0; i < m.positions.size(); ++i) {
            const ModelPos& p = m.positions[i];
            if (bit + 3 + size_t(p.xBits) + p.yBits + p.zBits > totalBits) return false;
            if (p.xBits == 7) ax = 0;
            if (p.yBits == 7) ay = 0;
            if (p.zBits == 7) az = 0;
            auto signedBits = [&](int n) {   // one sign bit, then n magnitude bits, sign-extended
                const int sign = read(1);
                int v = sign ? (-1 << n) : 0;
                if (n) v |= read(n);
                return v;
            };
            const int dx = signedBits(p.xBits), dz = signedBits(p.zBits), dy = signedBits(p.yBits);
            ax = (ax + dx + (p.x << 1)) & 0xFF;
            ay = (ay + dy + p.y) & 0xFF;
            az = (az + dz + p.z) & 0xFF;
            out[i] = {ax, ay, az};
        }
        return true;
    }
    if (totalBits < size_t(f.vertexCount - f.specialVertices) * 24) return false;
    size_t o = f.bitsStart + size_t(f.specialVertices) * 3;
    for (int i = f.specialVertices; i < f.vertexCount; ++i, o += 3) out[size_t(i)] = {d[o], d[o + 1], d[o + 2]};
    return true;
}

namespace {

const TexInfo* resolveModelTexture(const Model& m, int texId, bool animated, int frame) {
    if (texId == 0 && !animated) return nullptr;
    int idx = texId - 1;
    if (animated) {
        if (++idx >= int(m.animatedTextures.size())) return nullptr;
        int32_t anim = m.animatedTextures[size_t(idx)];
        if ((anim & 0x7FF) == 0) return nullptr;
        idx = (anim & 0x7FF) - 1;
        if (anim < 0) {
            idx += (anim >> 29) & 0x3;
        } else {
            const int latency = (anim >> 26) & 0x1F, delay = (anim >> 19) & 0x7F, mask = (anim >> 12) & 0x7F;
            idx += (frame / (1 + latency) + delay) & mask;
            if ((anim >> 11) & 1) {
                if (idx + 1 >= int(m.animatedTextures.size())) return nullptr;
                anim = m.animatedTextures[size_t(++idx)];
                idx = (anim & 0x7FF) - 1 + ((anim >> 29) & 0x3);
            }
        }
    }
    return idx >= 0 && idx < int(m.textures.size()) ? &m.textures[size_t(idx)] : nullptr;
}

}  // namespace

void buildModelMesh(const Model& m, const AnimFrame& f, const PageAtlas& atlas, int texFrame, std::vector<GpuVertex>& out) {
    std::vector<std::array<int, 3>> verts;
    if (!frameVertices(f, m, verts)) return;
    const float tx = float(f.xOffset) / 4.0f, ty = float(f.yOffset) / 4.0f, tz = float(f.zOffset) / 4.0f;
    const float sx = float(m.scale[0]) / (3200.0f * 128.0f), sy = float(m.scale[1]) / (3200.0f * 128.0f), sz = float(m.scale[2]) / (3200.0f * 128.0f);

    for (const ModelTri& t : m.tris) {
        const TexInfo* info = nullptr;
        if ((t.texture != 0 || t.animated)) {
            info = resolveModelTexture(m, t.texture, t.animated, texFrame);
            if (!info && !(t.animated && t.texture == 0)) continue;   // missing description: skip the polygon
        }
        const bool noCull = t.subtype == 0 || t.subtype == 2;
        const bool flip = ((t.type == 2) != (t.subtype == 3)) && !noCull;
        float st[3][2] = {};
        int page = -1;
        if (info) {
            if (info->page < int(m.tpages.size())) {
                const auto it = atlas.index.find(m.tpages[size_t(info->page)]);
                if (it != atlas.index.end()) page = it->second;
            }
            const int a = ((t.type != 2 && !flip) || (t.type == 2 && t.subtype == 1)) ? 2 : 0;   // corner used by slot 0
            const int c = 2 - a;
            st[0][0] = info->x[a]; st[0][1] = info->y[a];
            st[1][0] = info->x[1]; st[1][1] = info->y[1];
            st[2][0] = info->x[c]; st[2][1] = info->y[c];
        }
        bool inRange = true;
        for (int i = 0; i < 3; ++i) {
            const int vi = t.v[i] + f.specialVertices;
            if (vi < 0 || vi >= int(verts.size())) inRange = false;
        }
        if (!inRange) continue;
        for (int i = 0; i < 3; ++i) {
            const int n = flip ? 2 - i : i;
            const auto& vv = verts[size_t(t.v[n] + f.specialVertices)];
            GpuVertex g{};
            // stream order is x, z, y: the second stored component is the game's up axis
            g.pos[0] = (float(vv[0]) + tx) * sx;
            g.pos[1] = (float(vv[2]) + ty) * sy;
            g.pos[2] = (float(vv[1]) + tz) * sz;
            const Rgb c = t.c[n] < int(m.colors.size()) ? m.colors[size_t(t.c[n])] : Rgb{128, 128, 128};
            g.color[0] = c.r / 128.0f; g.color[1] = c.g / 128.0f; g.color[2] = c.b / 128.0f;
            g.tex[0] = page;
            if (info) { g.tex[1] = info->colorMode; g.tex[2] = info->clutX; g.tex[3] = info->clutY; g.blend = info->blend; g.uv[0] = st[i][0]; g.uv[1] = st[i][1]; }
            out.push_back(g);
        }
    }
}

}  // namespace cb2
