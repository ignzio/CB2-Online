#pragma once
// Crash 2 object models (entry type 2) and animations (entry type 1). Layout from CrashEdit's Model/ModelEntry*.cs and
// Animation/Frame.cs. A model holds polygons, colours and texture descriptions; an animation is a list of frames whose
// vertex positions are bit-packed deltas decoded against the model's per-vertex bit widths.
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "nsf.hpp"
#include "scenery.hpp"

namespace cb2 {

struct ModelTri {
    int v[3];
    int c[3];
    int texture = 0;
    int type = 0, subtype = 0;
    bool animated = false;
};

struct ModelPos {          // compression description of one vertex
    uint8_t x, y, z;       // base values (x is stored halved)
    uint8_t xBits, yBits, zBits;
};

struct Model {
    uint32_t eid = 0;
    int scale[3] = {0, 0, 0};
    int specialVertices = 0;               // uncompressed attachment vertices at the start of each frame
    int vertexCount = 0;
    std::vector<uint32_t> tpages;
    std::vector<ModelTri> tris;
    std::vector<Rgb> colors;
    std::vector<TexInfo> textures;
    std::vector<int32_t> animatedTextures;
    std::vector<ModelPos> positions;       // empty for uncompressed models
    bool compressed = false;
};

struct AnimFrame {
    int xOffset = 0, yOffset = 0, zOffset = 0;
    int vertexCount = 0, specialVertices = 0;
    uint32_t modelEid = 0;
    Bytes data;                            // the whole frame item
    size_t bitsStart = 0;                  // byte offset of the vertex data
};

struct Animation {
    uint32_t eid = 0;
    std::vector<AnimFrame> frames;
};

std::optional<Model> parseModel(const Entry& e);
std::optional<Animation> parseAnimation(const Entry& e);

// Decodes a frame's vertices (x, y, z bytes before scaling); false if the frame does not match the model.
bool frameVertices(const AnimFrame& f, const Model& m, std::vector<std::array<int, 3>>& out);

// Appends the model posed by `f` as triangles. Positions are game units / kWorldScale, with the model's axes kept as is.
void buildModelMesh(const Model& m, const AnimFrame& f, const PageAtlas& atlas, int texFrame, std::vector<GpuVertex>& out);

}  // namespace cb2
