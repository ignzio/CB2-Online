#include "props.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cb2prop {
namespace {

enum Field { kSelf = 0, kX = 8, kY = 9, kZ = 10, kRotY = 12, kVelX = 17, kVelY = 18, kVelZ = 19 };
constexpr uint32_t kObjectType = 3;
constexpr uint32_t kRamLow = 0x80010000, kRamHigh = 0x80200000;
constexpr float kBodyRadius = 110.0f;     // how far from a wall Crash's centre stays
constexpr float kLandMargin = 70.0f;      // how far past the edge of the top Crash still lands on it
constexpr float kNear = 250.0f;           // polygons with a corner nearer than this to the camera are not drawn (there is no clipping)
constexpr float kSquashDepth = 0.14f;     // how much of its height the prop gives under a bounce
constexpr uint32_t kCrateLevel = 0x1E;    // Turtle Woods: its level file holds the crates
bool debug() { static const bool d = std::getenv("CB2_PROPLOG") != nullptr; return d; }

// Where the props are. The warp room (level 2; the European language copies count as 2): on the floor ring of the room, a few steps to the right of where Crash starts, between the start and the Snow Go door.
const std::vector<Props::Placement>& builtIn() {
    static const std::vector<Props::Placement> list = { {0x02, 1166.0f, 471.0f, 112.5f, 1.0f, 1200000, "Bp10"} };
    return list;
}

}  // namespace

bool Props::isObject(uint32_t a) {
    if (a < kRamLow || a >= kRamHigh - 0x280 || (a & 3)) return false;
    return p.peek32(a) != 0 && p.peek32(a + F(kSelf)) == a && p.peek32(a + 4) == kObjectType;
}

uint32_t Props::findPlayer() {
    const uint32_t cur = p.peek32(cb2game::current().playerGlobal);
    player = cur && isObject(cur) ? cur : 0;
    return player;
}

uint32_t Props::levelNow() { return cb2game::canonicalLevel(cb2game::current().region, p.peek32(cb2game::current().currentLevel)); }

// The model, size and textures come from the game's own files: the level file of Turtle Woods is read from the disc the game runs from (so it is the player's own copy, of either release).
// `model` is the name of a crate's animation in that file without its ".V" ("Bp10" = the "?" crate; the others are in docs/PROPS.md).
int Props::assetFor(const std::string& model) {
    for (size_t i = 0; i < assets.size(); ++i) if (assets[i].name == model + ".V") return assets[i].ok ? int(i) : -1;
    if (assets.size() >= 4) { std::printf("props: more than four different crates in props.txt: %s is left out\n", model.c_str()); return -1; }
    if (!levelFileTried) {
        levelFileTried = true;
        char name[40];
        std::snprintf(name, sizeof name, "S%u\\S%07X.NSF", kCrateLevel >> 4, kCrateLevel);
        if (!p.disc.readFile(name, levelFile)) { levelFile.clear(); std::printf("props: the level file %s is not on the disc\n", name); }
    }
    Asset a;
    a.name = model + ".V";
    if (!levelFile.empty()) {
        a = loadAsset(levelFile, a.name);
        if (!a.ok) std::printf("props: %s is not available: %s\n", model.c_str(), a.error.c_str());
        a.name = model + ".V";
    }
    assets.push_back(std::move(a));
    const Asset& as = assets.back();
    if (!as.ok) return -1;
    // all the crates' textures into the renderer's picture: the asset number picks the 256 x 256 slot
    std::vector<uint16_t>& atlas = p.gpu.propAtlas;
    atlas.resize(1024u * 256u, 0);
    const size_t slot = assets.size() - 1;
    for (int y = 0; y < kAtlasSize; ++y) std::copy_n(as.atlas.begin() + std::ptrdiff_t(y) * kAtlasSize, kAtlasSize, atlas.begin() + std::ptrdiff_t(y) * 1024 + std::ptrdiff_t(slot) * kAtlasSize);
    ++p.gpu.propAtlasVersion;
    std::printf("props: the %s crate (%s, %zu polygons) read from the game's Turtle Woods file\n", as.name.c_str(), as.model.c_str(), as.tris.size());
    if (debug()) std::printf("[prop] model bounds x %.0f..%.0f  y %.0f..%.0f  z %.0f..%.0f (game units), closed %d\n", as.lo[0], as.hi[0], as.lo[1], as.hi[1], as.lo[2], as.hi[2], int(as.closed));
    return int(slot);
}

void Props::loadLevel(uint32_t level) {
    boxesLevel = level;
    boxes.clear();
    for (const Placement& pl : custom.empty() ? builtIn() : custom) {
        if (pl.level != level) continue;
        const int ai = assetFor(pl.model);
        if (ai < 0) continue;
        const Asset& asset = assets[size_t(ai)];
        Box b;
        b.asset = ai;
        b.scale = pl.scale;
        b.bounce = pl.bounce;
        b.origin[0] = pl.x;
        b.origin[1] = pl.floorY - asset.lo[1] * pl.scale;   // the bottom of the model stands on the floor
        b.origin[2] = pl.z;
        for (int a = 0; a < 3; ++a) {
            b.centre[a] = b.origin[a] + (asset.lo[a] + asset.hi[a]) * 0.5f * pl.scale;
            b.half[a] = (asset.hi[a] - asset.lo[a]) * 0.5f * pl.scale;
        }
        boxes.push_back(b);
    }
    if (debug()) std::printf("[prop] level %02X: %zu prop(s)\n", level, boxes.size());
}

// One prop per line: `box <level, hex> <x> <z> <floor y> [<scale> [<bounce> [<crate>]]]`, in world units; # starts a comment. The level is the number the game uses (2 = the warp room); the scale is 1 for the size
// the game's crate has; the crate is the name of one of the Turtle Woods crates (Bp10 = "?", the default).
int Props::loadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    custom.clear();
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (char* hash = std::strchr(line, '#')) *hash = 0;
        char word[16] = {};
        unsigned level = 0;
        float x = 0, z = 0, floorY = 0, scale = 1, bounce = 1200000;
        char model[16] = "Bp10";
        const int n = std::sscanf(line, " %15s %x %f %f %f %f %f %15s", word, &level, &x, &z, &floorY, &scale, &bounce, model);
        if (n < 5 || std::strcmp(word, "box") != 0 || scale < 0.1f || scale > 10.0f) continue;
        custom.push_back({level & 0xFF, x, z, floorY, scale, int32_t(std::clamp(bounce, 100000.0f, 3000000.0f)), model});
    }
    std::fclose(f);
    boxesLevel = 0xFFFFFFFFu;
    return int(custom.size());
}

void Props::install() {
    p.addHook(cb2game::current().objectStep, [this](psx::Platform& pl) { onStepEnter(pl.cpu.r[4]); });
    p.addHook(cb2game::current().objectStepReturn, [this](psx::Platform&) { onStepReturn(); });
    const auto prev = p.onDisplayList;
    p.onDisplayList = [this, prev] { if (prev) prev(); onDisplayList(); };
    p.cpu.probe = &probe;
    p.cpu.probePcs = cb2game::current().sceneryProjection;
}

void Props::onStepEnter(uint32_t obj) { stepping = obj; }

void Props::onStepReturn() {
    const uint32_t obj = stepping;
    stepping = 0;
    if (!enabled || !obj || obj != findPlayer()) return;
    interact(obj);
}

// The player's Crash against the props. Crash's feet are the object's position. A prop is a wall at its sides (Crash is moved out along the shortest way) and a spring on its top: when the feet come
// down through the top, Crash is put on it and launched upwards, which the game's own jump code then carries on (it integrates the velocity itself).
void Props::interact(uint32_t obj) {
    const uint32_t lv = levelNow();
    if (lv != boxesLevel) loadLevel(lv);
    if (boxes.empty()) return;
    float feet[3] = {float(int32_t(p.peek32(obj + F(kX)))) / 256.0f, float(int32_t(p.peek32(obj + F(kY)))) / 256.0f, float(int32_t(p.peek32(obj + F(kZ)))) / 256.0f};
    const int32_t velY = int32_t(p.peek32(obj + F(kVelY)));
    if (debug() && std::getenv("CB2_PROPTRACE")) std::printf("[prop] f%llu feet %.0f %.0f %.0f vel %d %d %d squash %.2f\n", (unsigned long long)p.frames, feet[0], feet[1], feet[2], int(p.peek32(obj + F(kVelX))), velY, int(p.peek32(obj + F(kVelZ))), boxes[0].squash);
    if (const char* drop = std::getenv("CB2_PROPDROP")) {   // test: put Crash above the first prop this many frames after the start
        static uint64_t first = 0;
        if (!first) first = p.frames;
        static bool done = false;
        if (!done && p.frames >= first + std::strtoull(drop, nullptr, 10)) {
            done = true;
            const Box& b = boxes[0];
            p.poke32(obj + F(kX), uint32_t(int32_t(b.centre[0] * 256.0f)));
            p.poke32(obj + F(kZ), uint32_t(int32_t(b.centre[2] * 256.0f)));
            p.poke32(obj + F(kY), uint32_t(int32_t((b.centre[1] + b.half[1] + 350.0f) * 256.0f)));
            p.poke32(obj + F(kVelY), 0);
            p.poke32(obj + F(kVelX), 0);
            p.poke32(obj + F(kVelZ), 0);
            p.poke32(obj + F(26), 0x00020800u);
            return;
        }
    }
    for (Box& b : boxes) {
        b.squash = std::max(0.0f, b.squash - 0.12f);
        const float top = b.centre[1] + b.half[1], bottom = b.centre[1] - b.half[1];
        const float dx = feet[0] - b.centre[0], dz = feet[2] - b.centre[2];
        const bool overTop = std::fabs(dx) < b.half[0] + kLandMargin && std::fabs(dz) < b.half[2] + kLandMargin;
        const bool crossed = b.haveLast && b.lastFeetY >= top - 25.0f && feet[1] <= top + 25.0f && feet[1] >= bottom;
        if (overTop && crossed && velY <= 0) {
            feet[1] = top;
            p.poke32(obj + F(kY), uint32_t(int32_t(std::lround(top * 256.0f))));
            p.poke32(obj + F(kVelY), uint32_t(b.bounce));
            b.squash = 1.0f;
            ++bounces;
            if (debug()) std::printf("[prop] frame %llu: bounce at %.0f %.0f %.0f (fell at %d)\n", (unsigned long long)p.frames, feet[0], feet[1], feet[2], velY);
        } else if (feet[1] < top - 25.0f && feet[1] > bottom - 600.0f) {
            // inside the height of the prop: a wall
            const float penX = b.half[0] + kBodyRadius - std::fabs(dx), penZ = b.half[2] + kBodyRadius - std::fabs(dz);
            if (penX > 0 && penZ > 0) {
                if (penX < penZ) {
                    feet[0] = b.centre[0] + (dx < 0 ? -1.0f : 1.0f) * (b.half[0] + kBodyRadius);
                    p.poke32(obj + F(kX), uint32_t(int32_t(std::lround(feet[0] * 256.0f))));
                    p.poke32(obj + F(kVelX), 0);
                } else {
                    feet[2] = b.centre[2] + (dz < 0 ? -1.0f : 1.0f) * (b.half[2] + kBodyRadius);
                    p.poke32(obj + F(kZ), uint32_t(int32_t(std::lround(feet[2] * 256.0f))));
                    p.poke32(obj + F(kVelZ), 0);
                }
            }
        }
        b.lastFeetY = feet[1];
        b.haveLast = true;
    }
}

bool Props::readCamera() {
    if (p.cpu.probeCount == lastProbeCount) return false;   // no scenery was projected since the last picture was drawn into
    const uint32_t* rt = probe.rt;
    const int16_t m[9] = {int16_t(rt[0]), int16_t(rt[0] >> 16), int16_t(rt[1]), int16_t(rt[1] >> 16), int16_t(rt[2]), int16_t(rt[2] >> 16), int16_t(rt[3]), int16_t(rt[3] >> 16), int16_t(rt[4])};
    for (int i = 0; i < 9; ++i) cam.rot[i] = float(m[i]);
    for (int i = 0; i < 3; ++i) cam.pos[i] = float(int32_t(p.peek32(cb2game::current().cameraPos + 4u * uint32_t(i))));
    cam.h = float(probe.h);
    cam.ofx = float(probe.ofx) / 65536.0f;
    cam.ofy = float(probe.ofy) / 65536.0f;
    cam.valid = probe.h != 0;
    return cam.valid;
}

bool Props::project(const float w[3], float out[3]) const {
    const float d[3] = {w[0] - cam.pos[0], w[1] - cam.pos[1], w[2] - cam.pos[2]};
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = (cam.rot[3 * i] * d[0] + cam.rot[3 * i + 1] * d[1] + cam.rot[3 * i + 2] * d[2]) / 4096.0f;
    if (c[2] < kNear) return false;
    out[0] = cam.ofx + c[0] * cam.h / c[2];
    out[1] = cam.ofy + c[1] * cam.h / c[2];
    out[2] = c[2];
    return true;
}

// The model's polygons as the game's GPU would get them: back faces left out (when the model is a closed solid), the rest sorted far to near.
void Props::drawBox(const Box& b, std::vector<psx::GpuCmd>& out, float& key, int offX, int offY, const psx::GpuCmd& tmpl) const {
    key = 0;
    const Asset& asset = assets[size_t(b.asset)];
    const float squashed = 1.0f - kSquashDepth * b.squash;
    struct Item { float depth; psx::GpuCmd cmd; };
    std::vector<Item> items;
    for (const AssetTri& t : asset.tris) {
        float w[3][3], s[3][3];
        bool visible = true;
        for (int i = 0; i < 3; ++i) {
            w[i][0] = b.origin[0] + t.p[i][0] * b.scale;
            w[i][1] = b.origin[1] + (asset.lo[1] + (t.p[i][1] - asset.lo[1]) * squashed) * b.scale;   // squashed towards the bottom
            w[i][2] = b.origin[2] + t.p[i][2] * b.scale;
            if (!project(w[i], s[i])) visible = false;
        }
        if (!visible) continue;
        if (asset.closed) {
            const float u[3] = {w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2]}, v[3] = {w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2]};
            const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
            if (n[0] * (cam.pos[0] - w[0][0]) + n[1] * (cam.pos[1] - w[0][1]) + n[2] * (cam.pos[2] - w[0][2]) <= 0) continue;
        }
        psx::GpuCmd c = tmpl;
        c.kind = psx::GpuCmd::Kind::Tri;
        c.textured = t.textured; c.prop = t.textured; c.raw = false; c.semi = false; c.alpha = 255; c.world = false; c.nativePoly = 0xFFFFFFFFu;
        c.worldOff[0] = c.worldOff[1] = 0;
        float depth = 0;
        for (int k = 0; k < 3; ++k) {
            c.x[k] = s[k][0] + float(offX);
            c.y[k] = s[k][1] + float(offY);
            c.u[k] = t.uv[k][0] + float(b.asset * kAtlasSize); c.v[k] = t.uv[k][1];
            c.w[k] = s[k][2];
            for (int q = 0; q < 3; ++q) c.col[k][q] = t.col[k][q];
            depth += s[k][2] / 3.0f;
            key = std::max(key, s[k][2]);
        }
        items.push_back({depth, c});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b2) { return a.depth > b2.depth; });
    for (const Item& it : items) out.push_back(it.cmd);
}

// Called when the game has sent its polygons for a picture to the GPU. The scene's list is the one after which the scenery projection ran; the prop's polygons are put into it where its depth fits:
// before the polygons that are nearer than the prop and after the ones that are farther (the game's own order is far to near).
void Props::onDisplayList() {
    if (!enabled || !p.gpu.recording) return;
    const uint32_t lv = levelNow();
    if (lv != boxesLevel) loadLevel(lv);
    if (boxes.empty() || p.cpu.probeCount == lastProbeCount) return;
    std::vector<psx::GpuCmd>& cmds = p.gpu.cmds;
    size_t start = 0;
    for (size_t i = cmds.size(); i-- > 0;) if (cmds[i].kind == psx::GpuCmd::Kind::Flip) { start = i + 1; break; }
    const psx::GpuCmd* ref = nullptr;   // a polygon of the scene: the prop takes its draw area
    std::vector<float> depth(cmds.size() - start, -1.0f);
    size_t known = 0, tris = 0;
    for (size_t i = start; i < cmds.size(); ++i) {
        const psx::GpuCmd& c = cmds[i];
        if (c.kind != psx::GpuCmd::Kind::Tri) continue;
        if (!ref) ref = &c;
        ++tris;
        if (c.w[0] > 1.0f && c.w[1] > 1.0f && c.w[2] > 1.0f) { depth[i - start] = (c.w[0] + c.w[1] + c.w[2]) / 3.0f; ++known; }
    }
    if (!ref || tris < 100 || !readCamera()) return;   // a short list is not the 3D scene (the game sends a picture in more than one list)
    lastProbeCount = p.cpu.probeCount;
    const psx::GpuCmd tmpl = *ref;
    const int offX = p.gpu.drawOffsetX(), offY = p.gpu.drawOffsetY();
    for (const Box& b : boxes) {
        std::vector<psx::GpuCmd> mine;
        float key = 0;
        drawBox(b, mine, key, offX, offY, tmpl);
        if (mine.empty()) continue;
        size_t at = cmds.size() - start;
        if (known >= 50) {
            // the place with the fewest polygons on the wrong side of it
            size_t farAfter = 0;
            for (float d : depth) if (d > key) ++farAfter;
            size_t nearBefore = 0, best = ~size_t(0), bestAt = depth.size();
            for (size_t i = 0; i <= depth.size(); ++i) {
                const size_t cost = nearBefore + farAfter;
                if (cost < best) { best = cost; bestAt = i; }
                if (i == depth.size()) break;
                if (depth[i] >= 0 && depth[i] < key) ++nearBefore;
                if (depth[i] > key) --farAfter;
            }
            at = bestAt;
        }
        cmds.insert(cmds.begin() + std::ptrdiff_t(start + at), mine.begin(), mine.end());
        depth.insert(depth.begin() + std::ptrdiff_t(at), mine.size(), key);   // the next prop sees this one
    }
    ++drawnFrames;
    if (debug() && drawnFrames % 30 == 1) std::printf("[prop] frame %llu: drawn (camera %.0f %.0f %.0f, h %.0f, known depths %zu)\n", (unsigned long long)p.frames, cam.pos[0], cam.pos[1], cam.pos[2], cam.h, known);
}

}  // namespace cb2prop
