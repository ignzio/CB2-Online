#include "scenery_edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>

namespace cb2 {
namespace {

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }

void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

size_t align4(size_t n) { return (n + 3) & ~size_t(3); }

void checkVertex(int v, const char* axis) {
    if (v < -2048 || v > 2047) fail(std::string("vertex ") + axis + " coordinate out of the 12-bit range (-2048..2047)");
}

}  // namespace

Entry packScenery(const Scenery& s, const Entry& original) {
    if (original.items.size() != 7 || original.items[0].size() != 76) fail("not a scenery entry");
    const size_t vc = s.vertices.size(), tc = s.triangles.size(), qc = s.quads.size(), cc = s.colors.size();
    if (vc > 4096) fail("too many vertices (the format has 12-bit vertex indices: 4096 maximum)");
    if (cc > 1024) fail("too many colours (10-bit colour index: 1024 maximum)");

    Entry out = original;
    Bytes& hdr = out.items[0];
    put32(&hdr[16], uint32_t(vc));
    put32(&hdr[20], uint32_t(tc));
    put32(&hdr[24], uint32_t(qc));
    put32(&hdr[32], uint32_t(cc));

    Bytes vb(align4(vc * 6), 0);
    for (size_t i = 0; i < vc; ++i) {
        const SceneryVertex& v = s.vertices[i];
        checkVertex(v.x, "x"); checkVertex(v.y, "y"); checkVertex(v.z, "z");
        if (v.colorIndex < 0 || v.colorIndex >= int(std::max<size_t>(cc, 1))) fail("vertex colour index out of range");
        const uint16_t rx = uint16_t(((v.x << 4) & 0xFFF0) | ((v.colorIndex >> 4) & 0xF));
        const uint16_t ry = uint16_t(((v.y << 4) & 0xFFF0) | ((v.spare & 3) << 2) | ((v.colorIndex >> 8) & 3));
        const uint16_t rz = uint16_t(((v.z << 4) & 0xFFF0) | (v.colorIndex & 0xF));
        put32(&vb[(vc - 1 - i) * 4], uint32_t(rx) | uint32_t(ry) << 16);
        put16(&vb[vc * 4 + i * 2], rz);
    }
    out.items[1] = std::move(vb);

    Bytes tb(align4(tc * 6), 0);
    for (size_t i = 0; i < tc; ++i) {
        const SceneryTri& t = s.triangles[i];
        if (t.a < 0 || t.b < 0 || t.c < 0 || t.a > 0xFFF || t.b > 0xFFF || t.c > 0xFFF) fail("triangle vertex index out of range");
        if (t.texture < 0 || t.texture > 0x7FF) fail("triangle texture index out of range");
        const uint32_t a = uint32_t((t.texture >> 4) & 0x7F) | (t.animated ? 0x80u : 0u) | uint32_t(t.a) << 8 | uint32_t(t.b) << 20;
        put32(&tb[(tc - 1 - i) * 4], a);
        put16(&tb[tc * 4 + i * 2], uint16_t((t.texture & 0xF) | (t.c << 4)));
    }
    out.items[2] = std::move(tb);

    Bytes qb(qc * 8, 0);
    for (size_t i = 0; i < qc; ++i) {
        const SceneryQuad& q = s.quads[i];
        for (int idx : {q.a, q.b, q.c, q.d}) if (idx < 0 || idx > 0xFFF) fail("quad vertex index out of range");
        if (q.texture < 0 || q.texture > 0x7FF) fail("quad texture index out of range");
        put32(&qb[i * 8], uint32_t((q.texture >> 4) & 0x7F) | (q.animated ? 0x80u : 0u) | uint32_t(q.a) << 8 | uint32_t(q.b) << 20);
        put32(&qb[i * 8 + 4], uint32_t(q.texture & 0xF) | uint32_t(q.spare & 0xF) << 4 | uint32_t(q.d) << 8 | uint32_t(q.c) << 20);
    }
    out.items[3] = std::move(qb);

    Bytes cb(cc * 4, 0);
    for (size_t i = 0; i < cc; ++i) { cb[i * 4] = s.colors[i].r; cb[i * 4 + 1] = s.colors[i].g; cb[i * 4 + 2] = s.colors[i].b; cb[i * 4 + 3] = s.colors[i].a; }
    out.items[5] = std::move(cb);
    return out;
}

std::string sceneryToObj(const Scenery& s) {
    std::string out;
    char buf[256];
    std::snprintf(buf, sizeof buf, "# Crash Bandicoot 2 scenery %s (see scenery_edit.hpp for the conventions)\n# offset %d %d %d sky %d\no %s\n",
                  eidName(s.eid).c_str(), s.xOffset, s.yOffset, s.zOffset, s.sky ? 1 : 0, eidName(s.eid).c_str());
    out += buf;
    const double ox = s.sky ? 0 : s.xOffset, oy = s.sky ? 0 : s.yOffset, oz = s.sky ? 0 : s.zOffset;
    for (const SceneryVertex& v : s.vertices) {
        const Rgb c = v.colorIndex < int(s.colors.size()) ? s.colors[size_t(v.colorIndex)] : Rgb{128, 128, 128};
        std::snprintf(buf, sizeof buf, "v %.6f %.6f %.6f %.6f %.6f %.6f\n#c %d %d\n", (v.x * 16.0 + ox) / kWorldScale,
                      (v.y * 16.0 + oy) / kWorldScale, (v.z * 16.0 + oz) / kWorldScale, c.r / 128.0, c.g / 128.0, c.b / 128.0, v.colorIndex, v.spare);
        out += buf;
    }
    int lastTex = -1;
    bool lastAnim = false;
    auto material = [&](int tex, bool anim) {
        if (tex == lastTex && anim == lastAnim) return;
        lastTex = tex; lastAnim = anim;
        std::snprintf(buf, sizeof buf, "usemtl %c%d\n", anim ? 'a' : 't', tex);
        out += buf;
    };
    const int vcount = int(s.vertices.size());
    // Polygons that point past the vertex table exist in the shipped data (the engine ignores them). They are kept as
    // `#f` comment lines so that 3D packages do not choke on them and nothing is lost on re-import.
    auto dangling = [&](std::initializer_list<int> ids) { for (int i : ids) if (i >= vcount) return true; return false; };
    for (const SceneryTri& t : s.triangles) {
        material(t.texture, t.animated);
        std::snprintf(buf, sizeof buf, "%sf %d %d %d\n", dangling({t.a, t.b, t.c}) ? "#" : "", t.a + 1, t.b + 1, t.c + 1);
        out += buf;
    }
    for (const SceneryQuad& q : s.quads) {
        material(q.texture, q.animated);
        std::snprintf(buf, sizeof buf, "%sf %d %d %d %d\n", dangling({q.a, q.b, q.c, q.d}) ? "#" : "", q.a + 1, q.b + 1, q.c + 1, q.d + 1);
        out += buf;
        if (q.spare) { std::snprintf(buf, sizeof buf, "#qs %d\n", q.spare); out += buf; }
    }
    return out;
}

Scenery sceneryFromObj(const std::string& obj, const Scenery& base) {
    Scenery s = base;
    s.vertices.clear();
    s.triangles.clear();
    s.quads.clear();
    const double ox = s.sky ? 0 : s.xOffset, oy = s.sky ? 0 : s.yOffset, oz = s.sky ? 0 : s.zOffset;
    std::map<uint32_t, int> colorLookup;
    auto key = [](const Rgb& c) { return uint32_t(c.r) << 16 | uint32_t(c.g) << 8 | c.b; };
    for (size_t i = 0; i < s.colors.size(); ++i) colorLookup.emplace(key(s.colors[i]), int(i));
    const std::vector<Rgb> baseColors = base.colors;

    int hintColor = -1, hintSpare = 0;
    bool havePending = false;
    int tex = 0;
    bool anim = false;
    size_t lineNo = 0;
    std::istringstream in(obj);
    for (std::string line; std::getline(in, line);) {
        ++lineNo;
        auto err = [&](const std::string& m) { fail("line " + std::to_string(lineNo) + ": " + m); };
        if (line.compare(0, 3, "#c ") == 0) {
            if (std::sscanf(line.c_str() + 3, "%d %d", &hintColor, &hintSpare) != 2 || s.vertices.empty()) err("bad #c line");
            // attach the original colour index to the vertex just read, if its colour is unchanged
            SceneryVertex& v = s.vertices.back();
            if (hintColor >= 0 && hintColor < int(baseColors.size())) {
                const Rgb& b = baseColors[size_t(hintColor)];
                const Rgb& cur = s.colors[size_t(v.colorIndex)];
                if (b.r == cur.r && b.g == cur.g && b.b == cur.b) v.colorIndex = hintColor;
            }
            v.spare = hintSpare & 3;
            havePending = false;
            continue;
        }
        if (line.compare(0, 4, "#qs ") == 0) {
            if (s.quads.empty()) fail("line " + std::to_string(lineNo) + ": #qs without a quad");
            s.quads.back().spare = std::atoi(line.c_str() + 4) & 0xF;
            continue;
        }
        if (line.compare(0, 3, "#f ") == 0) line.erase(0, 1);  // dangling polygon kept by the exporter
        else if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "v") {
            double p[3], c[3] = {1, 1, 1};
            if (!(ls >> p[0] >> p[1] >> p[2])) err("expected three coordinates");
            const bool hasColor = bool(ls >> c[0] >> c[1] >> c[2]);
            SceneryVertex v{};
            v.x = int(std::lround((p[0] * kWorldScale - ox) / 16.0));
            v.y = int(std::lround((p[1] * kWorldScale - oy) / 16.0));
            v.z = int(std::lround((p[2] * kWorldScale - oz) / 16.0));
            Rgb col{128, 128, 128};
            if (hasColor) {
                auto q = [](double x) { return uint8_t(std::clamp(std::lround(x * 128.0), 0l, 255l)); };
                col = {q(c[0]), q(c[1]), q(c[2])};
            }
            auto it = colorLookup.find(key(col));
            if (it == colorLookup.end()) {
                if (s.colors.size() >= 1024) err("more than 1024 distinct colours");
                s.colors.push_back(col);
                it = colorLookup.emplace(key(col), int(s.colors.size()) - 1).first;
            }
            v.colorIndex = it->second;
            s.vertices.push_back(v);
            havePending = true;
        } else if (tag == "usemtl") {
            std::string m;
            ls >> m;
            if (m.size() < 2 || (m[0] != 't' && m[0] != 'a') || !std::isdigit(static_cast<unsigned char>(m[1]))) err("material names must look like t12 or a3");
            anim = m[0] == 'a';
            tex = std::atoi(m.c_str() + 1);
        } else if (tag == "f") {
            std::vector<int> idx;
            for (std::string t; ls >> t;) {
                int v = std::atoi(t.c_str());
                v = v < 0 ? int(s.vertices.size()) + v : v - 1;
                idx.push_back(v);
            }
            if (idx.size() < 3) err("a face needs at least three vertices");
            if (idx.size() == 3) s.triangles.push_back({idx[0], idx[1], idx[2], tex, anim});
            else if (idx.size() == 4) s.quads.push_back({idx[0], idx[1], idx[2], idx[3], tex, anim});
            else for (size_t i = 1; i + 1 < idx.size(); ++i) s.triangles.push_back({idx[0], idx[i], idx[i + 1], tex, anim});
        }
    }
    (void)havePending;
    return s;
}

}  // namespace cb2
