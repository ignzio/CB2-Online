#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nsf.hpp"

namespace cb2 {

struct SceneryVertex {
    int x, y, z;       // 12-bit signed, in 1/16 world-offset units
    int colorIndex;    // index into Scenery::colors, packed in the low bits of x/y/z
    int spare = 0;     // two unexplained bits (bits 2-3 of the packed y word), kept so that editing is lossless
};

struct SceneryTri {
    int a, b, c;
    int texture;  // 1-based index into textures (or animated textures when `animated`), 0 = untextured
    bool animated;
};

struct SceneryQuad {
    int a, b, c, d;
    int texture;
    bool animated;
    int spare = 0;  // bits 4-7 of the second word, not understood; preserved so that editing is lossless
};

// 12-byte polygon texture description: UVs in "mode pixels" plus palette location.
struct TexInfo {
    uint8_t u[4], v[4];
    int clutX;     // CLUT x in 16-color (32-byte) units
    int clutY;     // CLUT row within the page
    int segment;   // horizontal 64-halfword segment of the page
    int blend;
    int colorMode; // 0 = 4-bit, 1 = 8-bit, 2 = 15-bit direct
    int page;      // index into Scenery::tpages
    float x[4], y[4];  // final texel coordinates for corners 1..4
};

struct Rgb { uint8_t r, g, b; uint8_t a = 0; };  // `a` is the fourth byte of the colour table entry (kept for lossless editing)

struct Scenery {
    uint32_t eid = 0;
    int32_t xOffset = 0, yOffset = 0, zOffset = 0;
    bool sky = false;
    std::vector<uint32_t> tpages;  // texture-page EIDs referenced by TexInfo::page
    std::vector<SceneryVertex> vertices;
    std::vector<SceneryTri> triangles;
    std::vector<SceneryQuad> quads;
    std::vector<TexInfo> textures;
    std::vector<Rgb> colors;
    std::vector<int32_t> animatedTextures;  // raw 32-bit flipbook/LOD descriptors
};

// Returns nullopt (never throws) when the entry does not match the Crash 2 layout.
std::optional<Scenery> parseScenery(const Entry& entry);

// Decodes one 12-byte polygon texture description (shared by world and model entries).
TexInfo parseTexInfoBytes(const uint8_t* twelveBytes);

// Interleaved GPU vertex. Texture fields are integers so the shader can decode VRAM itself.
struct GpuVertex {
    float pos[3];
    float color[3];
    float uv[2];
    int32_t tex[4];  // page index (-1 = untextured), color mode, clut x, clut y
    int32_t blend;
};

// Texture pages of one level; `index` maps a texture-chunk EID to its row in the page atlas.
struct PageAtlas {
    std::unordered_map<uint32_t, int> index;
    std::vector<const TexturePage*> pages;
};

PageAtlas buildPageAtlas(const Nsf& nsf);

// Appends triangles for `world`. `frame` selects flipbook animation frames.
// `source`, when given, receives for every emitted vertex the index of the scenery vertex it came from and `polygon` the number of the polygon (triangle or quad) it belongs to.
void buildMesh(const Scenery& world, const PageAtlas& atlas, int frame, std::vector<GpuVertex>& out, std::vector<int>* source = nullptr, std::vector<int>* polygon = nullptr);

constexpr float kWorldScale = 400.0f;  // game units per viewer unit

}  // namespace cb2
