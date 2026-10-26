#pragma once
// Native world renderer: the level scenery drawn by OpenGL from the level file itself instead of from the polygons the game's GTE code projected.
// The game places every scenery section in the world with a fixed offset and projects it with the camera's matrix alone, so a section's vertex is
// (16 * vertex + section offset - camera position), rotated by the camera matrix, then divided by its depth. Doing that here, with a real depth buffer
// and perspective-correct textures, gives the same picture as the console's but is independent of the game's 4:3 view, so it can be widened, drawn at any
// camera position in between two frames and, later, replaced by a custom level.
// Which polygons the game draws (it walks the level's visibility data) is learned from the game itself: the vertices it projected are looked up in the level's vertex
// table (their world position is the projected vertex plus the camera position), the triangles it then sent to the GPU are matched to the level's polygons through their
// corners, and exactly those polygons are drawn natively in the last two frames. Polygons the game draws see-through are left to the game.
#include <array>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glad/gl.h>

#include "nsf.hpp"

namespace cb2 {

struct NativeView {
    float cam[3] = {0, 0, 0};   // camera position in game units
    float rot[9] = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};   // the camera matrix as the GTE holds it (row-major, 4096 = 1.0)
    float h = 288;              // GTE projection plane distance
    float ofx = 256, ofy = 120; // screen centre in VRAM pixels (the GTE's offset plus the GPU draw offset)
    bool valid = false;
};

class NativeWorld {
public:
    ~NativeWorld() { clear(); }
    // Builds the meshes and the texture pages of one level from the bytes of its .NSF file. False when the file is not a level.
    bool load(const Bytes& nsfFile, uint32_t levelId);
    void clear();
    bool ready() const { return program != 0 && triangleCount > 0; }
    uint32_t level() const { return levelId; }
    // The scenery vertices one frame's camera matrix projected (the GTE's input vertices) and the camera position the game keeps in memory, which is only near the one
    // that frame used. Finds the exact camera position (the vertices plus the camera are positions of vertices of the level: it is searched around the last one found and around
    // the game's value), puts it in `camOut`, gives the level position key of every input vertex in `ids` (kNoVertex when no vertex of the level is there) and returns how many
    // were found (1.0 = all, 0 = no camera was found). A position can hold several vertices of the level (a hard edge has one per colour): polygons are matched by position.
    static constexpr uint64_t kNoVertex = ~0ull;
    float observe(const std::vector<std::array<int, 3>>& vertices, const float camGame[3], float camOut[3], std::vector<uint64_t>& ids);
    // A triangle the game sent to the GPU, made of the level's vertices `ids`. True when the native renderer draws that polygon (in frame `frameId`) and the game's triangle
    // is to be left out; false when the polygon is not in the level file or is drawn see-through (the game keeps it).
    bool claim(const uint64_t keys[3], bool seeThrough, uint32_t frameId);
    // After the claims of a pass: the polygons claimed in frame `frameId` or the one before are the ones to draw. With `extend` (a widened picture) the whole sections they
    // belong to are drawn, so that the margins the game's 4:3 view culled are not empty. Needs the GL context.
    void commit(uint32_t frameId, bool extend);
    // Draws into the bound framebuffer, which has the layout of VRAM at `scale` times its size (so a pixel of the console's picture is `scale` pixels). The scissor and the
    // depth buffer are the caller's: the depth buffer is cleared here.
    void draw(const NativeView& view, int scale, float widen = 1.0f);

private:
    uint32_t levelId = 0xFFFFFFFFu;
    GLuint program = 0, vao = 0, vbo = 0, ebo = 0, pageTex = 0;
    GLint locRot = -1, locCam = -1, locH = -1, locOfs = -1, locPages = -1, locDepth = -1, locWiden = -1, locMinZ = -1, locCull = -1;
    size_t triangleCount = 0;
    std::unordered_set<uint64_t> lookup;                   // the world positions (worldKey) of the level's vertices
    std::unordered_multimap<uint64_t, uint32_t> triples;   // three vertex numbers (sorted) of a polygon -> the polygon
    std::vector<uint32_t> polyFirst, polyStamp;            // per polygon: its first triangle (a quad has two), the last frame it was claimed in
    std::vector<uint8_t> polyTris, polySeeThrough;
    std::vector<uint32_t> polySection, polyClass;           // per polygon: its section, and its texture (polygons of a see-through texture are the game's to draw)
    std::unordered_set<uint32_t> seeThroughClasses;
    uint32_t sectionCount = 0;
    GLsizei drawCount = 0;
    size_t statClaims = 0, statMiss = 0, statMulti = 0, statSemi = 0;   // debugging counters (CB2_NWLOG)
    int lastCam[3] = {0, 0, 0};
    bool haveCam = false;
    bool solveCamera(const std::vector<std::array<int, 3>>& v, const int guess[3], int radius, int cam[3]) const;
    bool buildProgram();
};

}  // namespace cb2
