#pragma once
// A prop's look, taken from the game's own data: the model and the first frame of an animation of a level file (the "?" crate of Turtle Woods, for one), with the polygons' positions in game
// units and the textures decoded out of the level's texture pages into one small atlas of 15-bit colours (what the game would have put in video memory, with the palette applied).
#include <cstdint>
#include <string>
#include <vector>

#include "nsf.hpp"

namespace cb2prop {

struct AssetTri {
    float p[3][3];            // corners: x, y (up), z in game units, relative to the model's origin
    float uv[3][2];           // texel coordinates in the atlas (already nudged the way the GPU's floor() needs)
    uint8_t col[3][3];        // vertex colours as the game's GPU takes them (128 = the texture as it is)
    bool textured = false;
};

constexpr int kAtlasSize = 256;

struct Asset {
    std::string name;                     // the animation it came from, e.g. "BA10.V"
    std::string model;                    // the model that animation shows, e.g. "Ba10.G"
    std::vector<AssetTri> tris;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};   // bounds of the corners, same units as p
    std::vector<uint16_t> atlas;          // kAtlasSize x kAtlasSize; 0 = transparent
    bool closed = false;                  // the polygons are a closed solid (their back faces can be left out) with outward winding made consistent
    bool ok = false;
    std::string error;
};

// Builds the asset of the animation `animName` ("BA10.V") from the bytes of a level file; the first frame of the animation is used. On failure `error` says why.
Asset loadAsset(const cb2::Bytes& levelFile, const std::string& animName);

}  // namespace cb2prop
