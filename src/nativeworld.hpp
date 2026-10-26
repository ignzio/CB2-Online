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
    float farZ = 1e9f;          // the farthest depth worth drawing (the game's own draw distance)
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
    // A triangle the game sent to the GPU, made of the level's vertices `keys`. Returns the level polygon that replaces it (the renderer draws that polygon where the game's
    // triangle would have been, and leaves the game's out), or kNoPolygon when the polygon is not in the level file or is drawn see-through (the game keeps it). `passId` is
    // different for every pass, `frameId` for every picture.
    static constexpr uint32_t kNoPolygon = 0xFFFFFFFFu;
    uint32_t claim(const uint64_t keys[3], bool seeThrough, uint32_t frameId, uint32_t passId);
    // After the claims of a pass. With `extend` (a widened picture) the polygons of the sections the game drew from that it did not draw are prepared, so that the margins the
    // game's 4:3 view culled are not empty. Needs the GL context.
    void commit(uint32_t frameId, bool extend, const float cam[3], float farZ);
    // Under the picture: the polygons the game did not draw (see commit), with a depth buffer of their own.
    void drawExtras(const NativeView& view, float widen = 1.0f);
    // The scenery polygons in the order the game drew them, with no depth test: the game's painter's algorithm lets a polygon that is nearer be covered by one that is farther.
    void drawRun(const NativeView& view, const std::vector<uint32_t>& polygons, float widen = 1.0f);

private:
    uint32_t levelId = 0xFFFFFFFFu;
    GLuint program = 0, vao = 0, vbo = 0, ebo = 0, runEbo = 0, pageTex = 0;
    GLint locRot = -1, locCam = -1, locH = -1, locOfs = -1, locPages = -1, locDepth = -1, locWiden = -1, locMinZ = -1, locMaxZ = -1, locCull = -1;
    size_t triangleCount = 0;
    std::unordered_set<uint64_t> lookup;                   // the world positions (worldKey) of the level's vertices
    std::unordered_multimap<uint64_t, uint32_t> triples;   // three vertex numbers (sorted) of a polygon -> the polygon
    std::vector<uint32_t> runIndex;
    GLsizei extraCount = 0;
    std::vector<uint32_t> polyPass;                        // the pass that last took the polygon
    std::vector<uint32_t> polyFirst, polyStamp;            // per polygon: its first triangle (a quad has two), the last frame it was claimed in
    std::vector<uint8_t> polyTris;
    std::vector<uint8_t> polySemi;                          // per polygon: drawn see-through by the game (textured with a blend mode)
    std::vector<uint32_t> polySection, polyClass;           // per polygon: its section, and its texture (polygons of a see-through texture are the game's to draw)
    uint32_t sectionCount = 0;
    std::vector<std::array<float, 6>> sectionBox;           // per section: min x y z, max x y z in game units
    float farLimit = 1e9f;
    size_t statClaims = 0, statMiss = 0, statMulti = 0, statSemi = 0;   // debugging counters (CB2_NWLOG)
    int lastCam[3] = {0, 0, 0};
    bool haveCam = false;
    bool solveCamera(const std::vector<std::array<int, 3>>& v, const int guess[3], int radius, int cam[3]) const;
    void setup(const NativeView& v, float widen, GLuint element);
    bool buildProgram();
};

}  // namespace cb2
