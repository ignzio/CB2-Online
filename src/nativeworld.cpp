#include "nativeworld.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <iterator>
#include <cstdio>

#include "scenery.hpp"

namespace cb2 {
namespace {

const char* kVert = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec2 aUv;
layout(location=3) in ivec4 aTex;
uniform mat3 uRot;       // rows of the GTE matrix, 4096 = 1.0
uniform vec3 uCam;
uniform float uH;
uniform float uWiden;    // horizontal squeeze (1 = the console's 4:3 view)
uniform vec2 uOfs;       // screen centre in VRAM pixels
uniform float uWorld;    // game units per mesh unit
out vec3 vColor;
out vec2 vUv;
flat out ivec4 vTex;
void main() {
    vec3 c = (uRot * (aPos * uWorld - uCam)) / 4096.0;   // camera space: z is the depth
    float z = c.z;
    // the console's projection (ofs + H * xy / z) written in clip space, so that the hardware divides by z and interpolates perspective-correctly
    float cx = (uOfs.x * z + uH * uWiden * c.x) / 512.0 - z;
    float cy = (uOfs.y * z + uH * c.y) / 256.0 - z;
    const float n = 24.0, f = 60000.0;
    float cz = ((f + n) / (f - n)) * z - 2.0 * f * n / (f - n);
    gl_Position = vec4(cx, cy, cz, z);
    vColor = aColor; vUv = aUv; vTex = aTex;
}
)";

// The game drops a polygon when any of its corners is nearer than its near plane (instead of clipping it), which keeps big polygons that pass the camera from filling the view.
const char* kGeom = R"(#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
in vec3 vColor[]; in vec2 vUv[]; flat in ivec4 vTex[];
out vec3 fColor; out vec2 fUv; flat out ivec4 fTex;
uniform float uMinZ;
uniform int uCull;       // 0 none, 1 drop clockwise triangles on screen, 2 drop counter-clockwise ones
void main() {
    float z0 = gl_in[0].gl_Position.w, z1 = gl_in[1].gl_Position.w, z2 = gl_in[2].gl_Position.w;
    if (min(z0, min(z1, z2)) < uMinZ) return;
    if (uCull != 0) {
        vec2 a = gl_in[0].gl_Position.xy / z0, b = gl_in[1].gl_Position.xy / z1, c = gl_in[2].gl_Position.xy / z2;
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if ((uCull == 1 && area < 0.0) || (uCull == 2 && area > 0.0)) return;
    }
    for (int i = 0; i < 3; ++i) { gl_Position = gl_in[i].gl_Position; fColor = vColor[i]; fUv = vUv[i]; fTex = vTex[i]; EmitVertex(); }
    EndPrimitive();
}
)";

// The page texture holds raw PS1 VRAM bytes: 512 bytes per row, 128 rows per page; indexed modes look their colour up in a CLUT stored in the same page.
const char* kFrag = R"(#version 330 core
in vec3 fColor;
in vec2 fUv;
flat in ivec4 fTex;
uniform usampler2D uPages;
out vec4 oColor;
uint byteAt(int page, int x, int y) { return texelFetch(uPages, ivec2(x, page * 128 + y), 0).r; }
uint halfAt(int page, int x, int y) { return byteAt(page, x * 2, y) | (byteAt(page, x * 2 + 1, y) << 8); }
void main() {
    int page = fTex.x;
    if (page < 0) { oColor = vec4(clamp(fColor * 0.5, 0.0, 1.0), 1.0); return; }
    int mode = fTex.y;
    int u = int(floor(fUv.x + 0.002));
    int v = clamp(int(floor(fUv.y + 0.002)), 0, 127);
    uint texel;
    if (mode == 0) {
        uint b = byteAt(page, clamp(u >> 1, 0, 511), v);
        uint idx = ((u & 1) == 0) ? (b & 15u) : (b >> 4);
        texel = halfAt(page, fTex.z * 16 + int(idx), fTex.w);
    } else if (mode == 1) {
        uint idx = byteAt(page, clamp(u, 0, 511), v);
        texel = halfAt(page, fTex.z * 16 + int(idx), fTex.w);
    } else {
        texel = halfAt(page, clamp(u, 0, 255), v);
    }
    if (texel == 0u) discard;
    vec3 rgb = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u)) / 31.0;
    oColor = vec4(clamp(rgb * fColor, 0.0, 1.0), 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "native world shader: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool NativeWorld::buildProgram() {
    if (program) return true;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVert), gs = compile(GL_GEOMETRY_SHADER, kGeom), fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !gs || !fs) return false;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, gs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(gs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(program); program = 0; return false; }
    locRot = glGetUniformLocation(program, "uRot");
    locCam = glGetUniformLocation(program, "uCam");
    locH = glGetUniformLocation(program, "uH");
    locOfs = glGetUniformLocation(program, "uOfs");
    locWiden = glGetUniformLocation(program, "uWiden");
    locPages = glGetUniformLocation(program, "uPages");
    locDepth = glGetUniformLocation(program, "uWorld");
    locMinZ = glGetUniformLocation(program, "uMinZ");
    locCull = glGetUniformLocation(program, "uCull");
    return true;
}

void NativeWorld::clear() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ebo) glDeleteBuffers(1, &ebo);
    if (runEbo) glDeleteBuffers(1, &runEbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (pageTex) glDeleteTextures(1, &pageTex);
    vbo = vao = ebo = runEbo = pageTex = 0;
    triangleCount = 0;
    extraCount = 0;
    lookup.clear();
    triples.clear();
    polyFirst.clear();
    polyStamp.clear();
    polyPass.clear();
    polyTris.clear();
    polySeeThrough.clear();
    polySection.clear();
    polyClass.clear();
    seeThroughClasses.clear();
    sectionCount = 0;
    levelId = 0xFFFFFFFFu;
}

namespace {
uint64_t worldKey(int x, int y, int z) {
    return (uint64_t(uint32_t(x + 262144) & 0xFFFFF) << 40) | (uint64_t(uint32_t(y + 262144) & 0xFFFFF) << 20) | uint64_t(uint32_t(z + 262144) & 0xFFFFF);
}
// an unordered triple of positions
uint64_t tripleKey(uint64_t a, uint64_t b, uint64_t c) {
    if (a > b) std::swap(a, b);
    if (b > c) std::swap(b, c);
    if (a > b) std::swap(a, b);
    uint64_t h = a * 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 29) ^ b) * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 31) ^ c) * 0x94D049BB133111EBull;
    return h ^ (h >> 32);
}
}  // namespace

bool NativeWorld::load(const Bytes& nsfFile, uint32_t id) {
    clear();
    Nsf nsf;
    try { nsf = loadNsfFromBytes(nsfFile); } catch (const std::exception&) { return false; }
    if (!buildProgram()) return false;
    const PageAtlas atlas = buildPageAtlas(nsf);
    std::vector<GpuVertex> mesh;
    std::vector<int> source, polygon;
    uint32_t base = 0;
    for (const Entry& e : nsf.entries) {
        if (e.type != kEntryTypeScenery) continue;
        const auto w = parseScenery(e);
        if (!w || w->sky || w->vertices.empty()) continue;
        const size_t first = mesh.size();
        buildMesh(*w, atlas, 0, mesh, &source, &polygon);
        if (std::getenv("CB2_NWLOG")) {
            size_t emitted = 0;
            for (size_t i = first; i < mesh.size(); i += 0) { const int n = polygon[i]; size_t e = i; while (e < mesh.size() && polygon[e] == n) ++e; ++emitted; i = e; }
            std::printf("[nw] section %08x: %zu triangles + %zu quads in the file, %zu polygons built\n", w->eid, w->triangles.size(), w->quads.size(), emitted);
        }
        if (mesh.size() == first) continue;
        // the polygons of this section: a triangle is three emitted vertices, a quad six
        for (size_t i = first; i < mesh.size();) {
            const int number = polygon[i];
            size_t end = i;
            while (end < mesh.size() && polygon[end] == number) ++end;
            std::vector<uint64_t> corners;
            for (size_t k = i; k < end; ++k) {
                const SceneryVertex& sv = w->vertices[size_t(source[k])];
                const uint64_t g = worldKey(sv.x * 16 + w->xOffset, sv.y * 16 + w->yOffset, sv.z * 16 + w->zOffset);
                if (std::find(corners.begin(), corners.end(), g) == corners.end()) corners.push_back(g);
            }
            const uint32_t poly = uint32_t(polyFirst.size());
            polyFirst.push_back(uint32_t(i / 3));
            polyTris.push_back(uint8_t((end - i) / 3));
            polySection.push_back(sectionCount);
            {
                const GpuVertex& gv = mesh[i];
                uint32_t h = uint32_t(gv.tex[0] + 2) * 2654435761u;
                for (int k = 1; k < 4; ++k) h = (h ^ uint32_t(gv.tex[k])) * 2246822519u;
                h = (h ^ uint32_t(gv.blend)) * 3266489917u;
                h = (h ^ uint32_t(std::lround(gv.uv[0] * 4)) ^ (uint32_t(std::lround(gv.uv[1] * 4)) << 16)) * 668265263u;
                polyClass.push_back(h);
            }
            for (size_t x = 0; x + 2 < corners.size(); ++x)
                for (size_t y = x + 1; y < corners.size(); ++y)
                    for (size_t z = y + 1; z < corners.size(); ++z) triples.emplace(tripleKey(corners[x], corners[y], corners[z]), poly);
            i = end;
        }
        for (size_t vi = 0; vi < w->vertices.size(); ++vi) {
            const SceneryVertex& v = w->vertices[vi];
            lookup.insert(worldKey(v.x * 16 + w->xOffset, v.y * 16 + w->yOffset, v.z * 16 + w->zOffset));
        }
        base += uint32_t(w->vertices.size());
        ++sectionCount;
    }
    triangleCount = mesh.size() / 3;
    if (!triangleCount) return false;
    polyStamp.assign(polyFirst.size(), 0);
    polyPass.assign(polyFirst.size(), 0);
    polySeeThrough.assign(polyFirst.size(), 0u);
    haveCam = false;

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glGenBuffers(1, &runEbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.size() * sizeof(GpuVertex)), mesh.data(), GL_STATIC_DRAW);
    const GLsizei stride = sizeof(GpuVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, pos)));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, color)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(GpuVertex, uv)));
    glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_INT, stride, reinterpret_cast<void*>(offsetof(GpuVertex, tex)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

    const int pages = std::clamp<int>(int(atlas.pages.size()), 1, 127);
    glGenTextures(1, &pageTex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, pageTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 512, pages * 128, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    for (int i = 0; i < pages && i < int(atlas.pages.size()); ++i)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i * 128, 512, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, atlas.pages[size_t(i)]->data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(0);
    levelId = id;
    return true;
}

bool NativeWorld::solveCamera(const std::vector<std::array<int, 3>>& v, const int guess[3], int radius, int cam[3]) const {
    if (v.size() < 4) return false;
    // the vertices of the level and the offsets of its sections lie on a grid of 16, which fixes the camera position modulo 16 on every axis
    int res[3];
    for (int k = 0; k < 3; ++k) {
        int count[16] = {};
        for (const auto& p : v) ++count[(-p[size_t(k)]) & 15];
        res[k] = int(std::max_element(count, count + 16) - count);
    }
    const size_t n = std::min<size_t>(v.size(), 6);
    std::array<int, 3> probe[6];
    for (size_t i = 0; i < n; ++i) probe[i] = v[i * v.size() / n];
    std::vector<int> axis[3];
    for (int k = 0; k < 3; ++k)
        for (int c = guess[k] - radius; c <= guess[k] + radius; ++c)
            if (((c - res[k]) & 15) == 0) axis[k].push_back(c);
    for (int cx : axis[0])
        for (int cy : axis[1])
            for (int cz : axis[2]) {
                size_t missed = 0;
                for (size_t i = 0; i < n && missed < 2; ++i)
                    if (!lookup.count(worldKey(probe[i][0] + cx, probe[i][1] + cy, probe[i][2] + cz))) ++missed;
                if (missed < 2) { cam[0] = cx; cam[1] = cy; cam[2] = cz; return true; }
            }
    return false;
}

float NativeWorld::observe(const std::vector<std::array<int, 3>>& v, const float camGame[3], float camOut[3], std::vector<uint64_t>& ids) {
    ids.assign(v.size(), kNoVertex);
    if (!ready() || v.empty()) return 0.0f;
    const int ram[3] = {int(std::lround(camGame[0])), int(std::lround(camGame[1])), int(std::lround(camGame[2]))};
    int cam[3];
    bool ok = false;
    if (haveCam) ok = solveCamera(v, lastCam, 112, cam);
    if (!ok) ok = solveCamera(v, ram, 112, cam);
    if (!ok) ok = solveCamera(v, ram, 800, cam);
    if (!ok && haveCam) ok = solveCamera(v, lastCam, 800, cam);
    if (!ok) { haveCam = false; return 0.0f; }
    haveCam = true;
    std::copy(cam, cam + 3, lastCam);
    for (int k = 0; k < 3; ++k) camOut[k] = float(cam[k]);
    size_t hits = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        const uint64_t key = worldKey(v[i][0] + cam[0], v[i][1] + cam[1], v[i][2] + cam[2]);
        if (!lookup.count(key)) continue;
        ids[i] = key;
        ++hits;
    }
    return float(hits) / float(v.size());
}

void NativeWorld::markSeeThrough(const uint64_t keys[3], bool seeThrough, uint32_t frameId) {
    if (!seeThrough) return;
    const auto range = triples.equal_range(tripleKey(keys[0], keys[1], keys[2]));
    for (auto it = range.first; it != range.second; ++it) { polySeeThrough[it->second] = frameId; seeThroughClasses.insert(polyClass[it->second]); }
}

uint32_t NativeWorld::claim(const uint64_t keys[3], bool seeThrough, uint32_t frameId, uint32_t passId) {
    const auto range = triples.equal_range(tripleKey(keys[0], keys[1], keys[2]));
    static const bool dbg = std::getenv("CB2_NWLOG") != nullptr;
    if (dbg) { ++statClaims; if (range.first == range.second) ++statMiss; else if (std::distance(range.first, range.second) > 1) ++statMulti; if (seeThrough) ++statSemi; }
    if (range.first == range.second || seeThrough) return kNoPolygon;
    for (auto it = range.first; it != range.second; ++it) if (polySeeThrough[it->second] && polySeeThrough[it->second] + 1 >= frameId) return kNoPolygon;
    // polygons that share their three corners (a decal over a wall) are separate triangles for the game: each triangle takes the next one that has not been taken in this pass
    uint32_t pick = range.first->second;
    for (auto it = range.first; it != range.second; ++it) if (polyPass[it->second] != passId) { pick = it->second; break; }
    for (auto it = range.first; it != range.second; ++it) polyStamp[it->second] = frameId;
    polyPass[pick] = passId;
    return pick;
}

void NativeWorld::commit(uint32_t frameId, bool extend) {
    extraCount = 0;
    if (!ready() || !extend) return;
    // The polygons of the sections the game drew from that it did not draw itself (it culled them at the edge of its 4:3 view): drawn first, under the picture.
    std::vector<uint32_t> index;
    std::vector<uint8_t> sectionUsed(sectionCount, 0);
    auto drawnRecently = [&](size_t p) { const uint32_t s = polyStamp[p]; return s && s + 1 >= frameId && s <= frameId; };
    for (size_t p = 0; p < polyFirst.size(); ++p) if (drawnRecently(p)) sectionUsed[polySection[p]] = 1;
    for (size_t p = 0; p < polyFirst.size(); ++p) {
        if (drawnRecently(p) || !sectionUsed[polySection[p]] || polySeeThrough[p] || seeThroughClasses.count(polyClass[p])) continue;
        for (uint32_t k = 0; k < polyTris[p]; ++k)
            for (uint32_t c = 0; c < 3; ++c) index.push_back((polyFirst[p] + k) * 3 + c);
    }
    extraCount = GLsizei(index.size());
    static const bool dbg = std::getenv("CB2_NWLOG") != nullptr;
    if (dbg) { std::printf("[nw] commit frame %u: %zu game triangles looked up (%zu not in the level, %zu with several polygons, %zu see-through), %zu extra triangles\n", frameId, statClaims, statMiss, statMulti, statSemi, index.size() / 3); statClaims = statMiss = statMulti = statSemi = 0; }
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(index.size() * sizeof(uint32_t)), index.data(), GL_DYNAMIC_DRAW);
    glBindVertexArray(0);
}

void NativeWorld::setup(const NativeView& v, float widen, GLuint element) {
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(program);
    glUniformMatrix3fv(locRot, 1, GL_TRUE, v.rot);
    glUniform3fv(locCam, 1, v.cam);
    glUniform1f(locH, v.h);
    glUniform1f(locWiden, widen);
    glUniform2f(locOfs, v.ofx, v.ofy);
    glUniform1f(locDepth, kWorldScale);
    static const float minZ = std::getenv("CB2_NWMINZ") ? float(std::atof(std::getenv("CB2_NWMINZ"))) : 24.0f;
    glUniform1f(locMinZ, minZ);
    glUniform1i(locCull, 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, pageTex);
    glUniform1i(locPages, 3);
    glBindVertexArray(vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element);
}

void NativeWorld::drawExtras(const NativeView& v, float widen) {
    if (!ready() || !v.valid || !extraCount) return;
    // the extra polygons are not in the game's order: they get a depth buffer among themselves
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    setup(v, widen, ebo);
    glDrawElements(GL_TRIANGLES, extraCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_DEPTH_TEST);
}

void NativeWorld::drawRun(const NativeView& v, const std::vector<uint32_t>& polygons, float widen) {
    if (!ready() || !v.valid || polygons.empty()) return;
    // The scenery polygons in the order the game drew them (its painter's algorithm: no depth buffer, a nearer polygon can be drawn first and be covered by a farther one).
    runIndex.clear();
    for (const uint32_t p : polygons)
        for (uint32_t k = 0; k < polyTris[p]; ++k)
            for (uint32_t c = 0; c < 3; ++c) runIndex.push_back((polyFirst[p] + k) * 3 + c);
    glDisable(GL_DEPTH_TEST);
    setup(v, widen, runEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(runIndex.size() * sizeof(uint32_t)), runIndex.data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, GLsizei(runIndex.size()), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
}

}  // namespace cb2
