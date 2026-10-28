#include "propasset.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <tuple>

#include "model.hpp"
#include "nsf.hpp"
#include "scenery.hpp"

namespace cb2prop {
namespace {

// What a rectangle of a texture page looks like with its palette applied.
struct Rect { int page, mode, clutX, clutY, x0, y0, x1, y1, ax = 0, ay = 0; };

uint16_t texel(const cb2::TexturePage& page, int mode, int clutX, int clutY, int x, int y) {
    const uint8_t* d = page.data.data();
    auto half = [&](int hx, int hy) -> uint16_t {
        hx = std::clamp(hx, 0, 255); hy = std::clamp(hy, 0, 127);
        return uint16_t(d[hy * 512 + hx * 2] | (d[hy * 512 + hx * 2 + 1] << 8));
    };
    y = std::clamp(y, 0, 127);
    if (mode == 0) {
        x = std::clamp(x, 0, 1023);
        const uint8_t b = d[y * 512 + (x >> 1)];
        return half(clutX * 16 + ((x & 1) ? (b >> 4) : (b & 15)), clutY);
    }
    if (mode == 1) {
        x = std::clamp(x, 0, 511);
        return half(clutX * 16 + d[y * 512 + x], clutY);
    }
    return half(x, y);
}

}  // namespace

Asset loadAsset(const cb2::Bytes& levelFile, const std::string& animName) {
    Asset a;
    a.name = animName;
    try {
        const cb2::Nsf nsf = cb2::loadNsfFromBytes(levelFile);
        const cb2::Entry* animEntry = nullptr;
        for (const cb2::Entry& e : nsf.entries) if (e.type == 1 && cb2::eidName(e.eid) == animName) { animEntry = &e; break; }
        if (!animEntry) {
            a.error = "no animation " + animName + " in the level; entries:";
            for (const cb2::Entry& e : nsf.entries) if (cb2::eidName(e.eid).find("10.") != std::string::npos) a.error += " " + cb2::eidName(e.eid) + "/t" + std::to_string(e.type);
            return a;
        }
        const auto anim = cb2::parseAnimation(*animEntry);
        if (!anim || anim->frames.empty()) { a.error = "animation " + animName + " cannot be read"; return a; }
        const cb2::AnimFrame& frame = anim->frames[0];
        std::optional<cb2::Model> model;
        for (const cb2::Entry& e : nsf.entries) if (e.type == 2 && e.eid == frame.modelEid) { model = cb2::parseModel(e); break; }
        if (!model) { a.error = "the model of " + animName + " (" + cb2::eidName(frame.modelEid) + ") is not in the level"; return a; }
        a.model = cb2::eidName(frame.modelEid);
        std::vector<std::array<int, 3>> verts;
        if (!cb2::frameVertices(frame, *model, verts)) { a.error = "the frame does not match the model"; return a; }

        std::map<uint32_t, const cb2::TexturePage*> pages;
        for (const cb2::TexturePage& t : nsf.textures) pages[t.eid] = &t;

        // geometry and texture rectangles (the same reading of the model as cb2::buildModelMesh, which the model viewer uses)
        const float tx = float(frame.xOffset) / 4.0f, ty = float(frame.yOffset) / 4.0f, tz = float(frame.zOffset) / 4.0f;
        const float k = cb2::kWorldScale / (3200.0f * 128.0f);
        const float sx = float(model->scale[0]) * k, sy = float(model->scale[1]) * k, sz = float(model->scale[2]) * k;
        struct Pending { AssetTri t; const cb2::TexInfo* info = nullptr; int rect = -1; float st[3][2] = {}; };
        std::vector<Pending> pending;
        std::vector<Rect> rects;
        std::map<std::tuple<int, int, int, int, int, int, int, int>, int> rectIndex;
        for (const cb2::ModelTri& t : model->tris) {
            const cb2::TexInfo* info = nullptr;
            if (t.texture != 0 || t.animated) {
                info = cb2::resolveModelTexture(*model, t.texture, t.animated, 0);
                if (!info && !(t.animated && t.texture == 0)) continue;
            }
            const bool noCull = t.subtype == 0 || t.subtype == 2;
            const bool flip = ((t.type == 2) != (t.subtype == 3)) && !noCull;
            Pending pd;
            pd.info = info;
            if (info) {
                const int ca = ((t.type != 2 && !flip) || (t.type == 2 && t.subtype == 1)) ? 2 : 0;
                const int cc = 2 - ca;
                pd.st[0][0] = info->x[ca]; pd.st[0][1] = info->y[ca];
                pd.st[1][0] = info->x[1];  pd.st[1][1] = info->y[1];
                pd.st[2][0] = info->x[cc]; pd.st[2][1] = info->y[cc];
            }
            bool inRange = true;
            for (int i = 0; i < 3; ++i) {
                const int vi = t.v[i] + frame.specialVertices;
                if (vi < 0 || vi >= int(verts.size())) inRange = false;
            }
            if (!inRange) continue;
            for (int i = 0; i < 3; ++i) {
                const int n = flip ? 2 - i : i;
                const auto& vv = verts[size_t(t.v[n] + frame.specialVertices)];
                pd.t.p[i][0] = (float(vv[0]) + tx) * sx;
                pd.t.p[i][1] = (float(vv[2]) + ty) * sy;
                pd.t.p[i][2] = (float(vv[1]) + tz) * sz;
                const cb2::Rgb c = t.c[n] < int(model->colors.size()) ? model->colors[size_t(t.c[n])] : cb2::Rgb{128, 128, 128};
                pd.t.col[i][0] = c.r; pd.t.col[i][1] = c.g; pd.t.col[i][2] = c.b;
                if (i == 0 && pending.empty()) { for (int q = 0; q < 3; ++q) a.lo[q] = a.hi[q] = pd.t.p[0][q]; }
                for (int q = 0; q < 3; ++q) { a.lo[q] = std::min(a.lo[q], pd.t.p[i][q]); a.hi[q] = std::max(a.hi[q], pd.t.p[i][q]); }
            }
            if (info) {
                if (info->page >= int(model->tpages.size()) || !pages.count(model->tpages[size_t(info->page)])) continue;
                int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);
                for (int i = 0; i < 3; ++i) {
                    const int px = int(std::floor(pd.st[i][0] + 0.002f)), py = int(std::floor(pd.st[i][1] + 0.002f));
                    x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py);
                }
                const auto key = std::make_tuple(info->page, info->colorMode, info->clutX, info->clutY, x0, y0, x1, y1);
                auto it = rectIndex.find(key);
                if (it == rectIndex.end()) {
                    it = rectIndex.emplace(key, int(rects.size())).first;
                    rects.push_back({info->page, info->colorMode, info->clutX, info->clutY, x0, y0, x1, y1});
                }
                pd.rect = it->second;
                pd.t.textured = true;
            }
            pending.push_back(pd);
        }
        if (pending.empty()) { a.error = "the model has no polygons"; return a; }

        // pack the rectangles into the atlas, shelf by shelf (a texel of margin on every side so that the filter cannot reach the next picture)
        a.atlas.assign(size_t(kAtlasSize) * kAtlasSize, 0);
        int shelfX = 0, shelfY = 0, shelfH = 0;
        for (Rect& r : rects) {
            const int w = r.x1 - r.x0 + 2, h = r.y1 - r.y0 + 2;
            if (w > kAtlasSize || h > kAtlasSize) { a.error = "a texture is too large for the atlas"; return a; }
            if (shelfX + w > kAtlasSize) { shelfX = 0; shelfY += shelfH; shelfH = 0; }
            if (shelfY + h > kAtlasSize) { a.error = "the textures do not fit in the atlas"; return a; }
            r.ax = shelfX; r.ay = shelfY;
            shelfX += w; shelfH = std::max(shelfH, h);
            const cb2::TexturePage& page = *pages[model->tpages[size_t(r.page)]];
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    a.atlas[size_t(r.ay + y) * kAtlasSize + size_t(r.ax + x)] = texel(page, r.mode, r.clutX, r.clutY, r.x0 + x, r.y0 + y);
        }
        for (Pending& pd : pending) {
            if (pd.rect >= 0) {
                const Rect& r = rects[size_t(pd.rect)];
                for (int i = 0; i < 3; ++i) { pd.t.uv[i][0] = pd.st[i][0] - float(r.x0) + float(r.ax); pd.t.uv[i][1] = pd.st[i][1] - float(r.y0) + float(r.ay); }
            }
            a.tris.push_back(pd.t);
        }

        // closed solid? every edge shared by exactly two polygons: then the winding is made consistent (outwards) so that back faces can be skipped
        std::map<std::tuple<int, int, int, int, int, int>, int> edges;
        auto vkey = [](const float* v) { return std::make_tuple(int(std::lround(v[0] * 4)), int(std::lround(v[1] * 4)), int(std::lround(v[2] * 4))); };
        bool closed = true;
        std::map<std::tuple<int, int, int, int, int, int>, int> edgeCount;
        for (const AssetTri& t : a.tris)
            for (int i = 0; i < 3; ++i) {
                auto p = vkey(t.p[i]), q = vkey(t.p[(i + 1) % 3]);
                if (q < p) std::swap(p, q);
                ++edgeCount[std::tuple_cat(p, q)];
            }
        for (const auto& e : edgeCount) if (e.second != 2) closed = false;
        if (closed) {
            float mid[3] = {(a.lo[0] + a.hi[0]) / 2, (a.lo[1] + a.hi[1]) / 2, (a.lo[2] + a.hi[2]) / 2};
            double volume = 0;
            for (const AssetTri& t : a.tris) {
                float u[3], v[3], n[3];
                for (int q = 0; q < 3; ++q) { u[q] = t.p[1][q] - t.p[0][q]; v[q] = t.p[2][q] - t.p[0][q]; }
                n[0] = u[1] * v[2] - u[2] * v[1]; n[1] = u[2] * v[0] - u[0] * v[2]; n[2] = u[0] * v[1] - u[1] * v[0];
                float c[3];
                for (int q = 0; q < 3; ++q) c[q] = (t.p[0][q] + t.p[1][q] + t.p[2][q]) / 3 - mid[q];
                volume += double(n[0] * c[0] + n[1] * c[1] + n[2] * c[2]);
            }
            if (volume < 0)   // inward: turn every polygon around
                for (AssetTri& t : a.tris) { std::swap(t.p[1], t.p[2]); std::swap(t.uv[1], t.uv[2]); std::swap(t.col[1], t.col[2]); }
        }
        a.closed = closed;
        a.ok = true;
    } catch (const std::exception& e) {
        a.error = e.what();
    }
    return a;
}

}  // namespace cb2prop
