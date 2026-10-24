#include "scenery.hpp"

#include <algorithm>
#include <cstring>

namespace cb2 {
namespace {

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
int32_t rdS32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

size_t align4(size_t n) { return (n + 3) & ~size_t(3); }

TexInfo parseTexInfo(const uint8_t* d) {
    TexInfo t{};
    t.u[0] = d[0]; t.v[0] = d[1];
    t.u[1] = d[4]; t.v[1] = d[5];
    t.u[2] = d[8]; t.v[2] = d[9];
    t.u[3] = d[10]; t.v[3] = d[11];
    t.clutX = d[2] & 0xF;
    const int clutY1 = (d[2] >> 4) & 0xF;
    const int clutY2 = d[3];
    t.clutY = (clutY2 << 2) | ((clutY1 >> 2) & 0x3);
    t.segment = d[6] & 0xF;
    t.blend = (d[6] >> 5) & 0x3;
    t.colorMode = ((d[6] >> 7) | ((d[7] << 1) & 2)) & 0x3;
    t.page = (d[7] >> 2) & 0x3F;

    // Corners are nudged by one texel so the shader's floor() lands on the correct edge texel.
    const int xoff = (1 << (2 - std::min(t.colorMode, 2))) * 64 * t.segment;
    int tx[4], ty[4];
    for (int i = 0; i < 4; ++i) { tx[i] = t.u[i] + xoff; ty[i] = t.v[i]; }
    // Each comparison sees the corners already nudged before it.
    auto nudge = [](int* a) {
        if (a[0] > a[1] || a[0] > a[2]) ++a[0];
        if (a[1] > a[0] || a[1] > a[2]) ++a[1];
        if (a[2] > a[1] || a[2] > a[0]) ++a[2];
        if (a[3] > a[1] || a[3] > a[2] || a[3] > a[0]) ++a[3];
    };
    nudge(tx);
    nudge(ty);
    for (int i = 0; i < 4; ++i) { t.x[i] = float(tx[i]); t.y[i] = float(ty[i]); }
    return t;
}

}  // namespace

TexInfo parseTexInfoBytes(const uint8_t* d) { return parseTexInfo(d); }

std::optional<Scenery> parseScenery(const Entry& e) {
    if (e.type != kEntryTypeScenery || e.items.size() != 7 || e.items[0].size() != 76) return std::nullopt;
    const Bytes& hdr = e.items[0];
    const uint32_t vc = rd32(&hdr[16]), tc = rd32(&hdr[20]), qc = rd32(&hdr[24]);
    const uint32_t xc = rd32(&hdr[28]), cc = rd32(&hdr[32]), ac = rd32(&hdr[36]);
    if (e.items[1].size() != align4(vc * 6ull) || e.items[2].size() != align4(tc * 6ull) ||
        e.items[3].size() != qc * 8ull || e.items[4].size() != xc * 12ull ||
        e.items[5].size() != cc * 4ull || e.items[6].size() != ac * 4ull) {
        return std::nullopt;
    }

    Scenery s;
    s.eid = e.eid;
    s.xOffset = rdS32(&hdr[0]);
    s.yOffset = rdS32(&hdr[4]);
    s.zOffset = rdS32(&hdr[8]);
    s.sky = rd32(&hdr[12]) != 0;
    const uint32_t tpageCount = std::min<uint32_t>(rd32(&hdr[0x28]), 8);
    for (uint32_t i = 0; i < tpageCount; ++i) s.tpages.push_back(rd32(&hdr[0x2C + 4 * i]));

    // XY words are stored in reverse order, followed by the Z halfwords in forward order.
    const Bytes& vb = e.items[1];
    for (uint32_t i = 0; i < vc; ++i) {
        const uint32_t xy = rd32(&vb[(vc - 1 - i) * 4]);
        const uint16_t rx = uint16_t(xy), ry = uint16_t(xy >> 16), rz = rd16(&vb[vc * 4 + i * 2]);
        SceneryVertex v;
        v.x = int16_t(rx) >> 4;
        v.y = int16_t(ry) >> 4;
        v.z = int16_t(rz) >> 4;
        v.colorIndex = ((ry & 0x3) << 8) | ((rx & 0xF) << 4) | (rz & 0xF);
        v.spare = (ry >> 2) & 0x3;
        s.vertices.push_back(v);
    }

    const Bytes& tb = e.items[2];
    for (uint32_t i = 0; i < tc; ++i) {
        const uint32_t a = rd32(&tb[(tc - 1 - i) * 4]);
        const uint16_t b = rd16(&tb[tc * 4 + i * 2]);
        s.triangles.push_back({int((a >> 8) & 0xFFF), int((a >> 20) & 0xFFF), int((b >> 4) & 0xFFF),
                               int((b & 0xF) | ((a & 0x7F) << 4)), (a & 0x80) != 0});
    }

    const Bytes& qb = e.items[3];
    for (uint32_t i = 0; i < qc; ++i) {
        const uint32_t w1 = rd32(&qb[i * 8]), w2 = rd32(&qb[i * 8 + 4]);
        s.quads.push_back({int((w1 >> 8) & 0xFFF), int((w1 >> 20) & 0xFFF), int((w2 >> 20) & 0xFFF),
                           int(w2 >> 8 & 0xFFF), int((w2 & 0xF) | ((w1 & 0x7F) << 4)), (w1 & 0x80) != 0, int((w2 >> 4) & 0xF)});
    }

    for (uint32_t i = 0; i < xc; ++i) s.textures.push_back(parseTexInfo(&e.items[4][i * 12]));
    for (uint32_t i = 0; i < cc; ++i) {
        const uint8_t* c = &e.items[5][i * 4];
        s.colors.push_back({c[0], c[1], c[2], c[3]});
    }
    for (uint32_t i = 0; i < ac; ++i) s.animatedTextures.push_back(rdS32(&e.items[6][i * 4]));
    return s;
}

PageAtlas buildPageAtlas(const Nsf& nsf) {
    PageAtlas atlas;
    for (const TexturePage& p : nsf.textures) {
        if (atlas.index.emplace(p.eid, int(atlas.pages.size())).second) atlas.pages.push_back(&p);
    }
    return atlas;
}

namespace {

// Resolves a polygon's texture reference. Returns false when the polygon must be skipped;
// `out` is null for untextured polygons.
bool resolveTexture(const Scenery& w, int texId, bool animated, int frame, const TexInfo*& out) {
    out = nullptr;
    if (texId == 0 && !animated) return true;

    int idx = texId - 1;
    if (animated) {
        if (++idx >= int(w.animatedTextures.size())) return false;
        int32_t anim = w.animatedTextures[idx];
        if ((anim & 0x7FF) == 0) return true;  // untextured animated polygon
        idx = (anim & 0x7FF) - 1;
        if (anim < 0) {
            idx += (anim >> 29) & 0x3;  // closest LOD
        } else {
            const int latency = (anim >> 26) & 0x1F, delay = (anim >> 19) & 0x7F, mask = (anim >> 12) & 0x7F;
            idx += (frame / (1 + latency) + delay) & mask;
            if ((anim >> 11) & 1) {
                anim = w.animatedTextures[++idx];
                idx = (anim & 0x7FF) - 1 + (((anim >> 29) & 0x3));
            }
        }
    }
    if (idx < 0 || idx >= int(w.textures.size())) return false;
    out = &w.textures[idx];
    return true;
}

}  // namespace

void buildMesh(const Scenery& w, const PageAtlas& atlas, int frame, std::vector<GpuVertex>& out) {
    // Sky domes are positioned relative to the camera, so they ignore the world offset.
    const float ox = w.sky ? 0.0f : float(w.xOffset), oy = w.sky ? 0.0f : float(w.yOffset),
                oz = w.sky ? 0.0f : float(w.zOffset);
    const int vcount = int(w.vertices.size());

    auto emit = [&](int vi, const TexInfo* t, int corner) {
        const SceneryVertex& v = w.vertices[vi];
        GpuVertex g{};
        g.pos[0] = (float(v.x) * 16.0f + ox) / kWorldScale;
        g.pos[1] = (float(v.y) * 16.0f + oy) / kWorldScale;
        g.pos[2] = (float(v.z) * 16.0f + oz) / kWorldScale;
        const Rgb c = v.colorIndex < int(w.colors.size()) ? w.colors[v.colorIndex] : Rgb{128, 128, 128};
        g.color[0] = c.r / 128.0f; g.color[1] = c.g / 128.0f; g.color[2] = c.b / 128.0f;
        g.tex[0] = -1;
        if (t) {
            int page = -1;
            if (t->page < int(w.tpages.size())) {
                const auto it = atlas.index.find(w.tpages[t->page]);
                if (it != atlas.index.end()) page = it->second;
            }
            g.tex[0] = page;
            g.tex[1] = t->colorMode; g.tex[2] = t->clutX; g.tex[3] = t->clutY;
            g.blend = t->blend;
            g.uv[0] = t->x[corner]; g.uv[1] = t->y[corner];
        }
        out.push_back(g);
    };

    for (const SceneryTri& p : w.triangles) {
        if (p.a >= vcount || p.b >= vcount || p.c >= vcount) continue;
        const TexInfo* t;
        if (!resolveTexture(w, p.texture, p.animated, frame, t)) continue;
        emit(p.a, t, 1); emit(p.b, t, 0); emit(p.c, t, 2);
    }
    for (const SceneryQuad& p : w.quads) {
        if (p.a >= vcount || p.b >= vcount || p.c >= vcount || p.d >= vcount) continue;
        const TexInfo* t;
        if (!resolveTexture(w, p.texture, p.animated, frame, t)) continue;
        emit(p.a, t, 1); emit(p.b, t, 0); emit(p.c, t, 2);
        emit(p.a, t, 1); emit(p.d, t, 3); emit(p.c, t, 2);
    }
}

}  // namespace cb2
