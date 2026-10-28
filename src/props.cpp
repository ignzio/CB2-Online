#include "props.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cb2prop {
namespace {

enum Field { kSelf = 0, kX = 8, kY = 9, kZ = 10, kVelX = 17, kVelY = 18, kVelZ = 19 };
constexpr uint32_t kObjectType = 3;
constexpr uint32_t kRamLow = 0x80010000, kRamHigh = 0x80200000;
constexpr float kBodyRadius = 110.0f;     // how far from a wall Crash's centre stays
constexpr float kLandMargin = 70.0f;      // how far past the edge of the top Crash still lands on it
constexpr float kNear = 250.0f;           // faces with a corner nearer than this to the camera are not drawn (no clipping)
constexpr float kSquashDepth = 0.14f;     // how much of its height the box gives under a bounce
bool debug() { static const bool d = std::getenv("CB2_PROPLOG") != nullptr; return d; }

struct Placement { uint32_t level; Box box; };

// Where the boxes are. The warp room (level 2; the European language copies count as 2): on the floor ring of the room, a few steps to the right of where Crash starts (between the start and the Snow Go door, in nobody's way).
Props::Placement makePlacement(uint32_t level, float cx, float floorY, float cz, float half) {
    Props::Placement pl;
    pl.level = level;
    pl.box.half[0] = pl.box.half[1] = pl.box.half[2] = half;
    pl.box.centre[0] = cx;
    pl.box.centre[1] = floorY + half;
    pl.box.centre[2] = cz;
    return pl;
}

const std::vector<Props::Placement>& builtIn() {
    static const std::vector<Props::Placement> list = { makePlacement(0x02, 1166.0f, 112.5f, 471.0f, 250.0f) };
    return list;
}

uint8_t shade(int c, float f) { return uint8_t(std::clamp(int(float(c) * f + 0.5f), 0, 255)); }

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

void Props::loadLevel(uint32_t level) {
    boxesLevel = level;
    boxes.clear();
    for (const Placement& pl : custom.empty() ? builtIn() : custom) if (pl.level == level) boxes.push_back(pl.box);
    if (debug()) std::printf("[prop] level %02X: %zu box(es)\n", level, boxes.size());
}

// One box per line: `box <level, hex> <x> <z> <floor y> <half size> [<bounce>]`, in world units; # starts a comment. The level is the number the game uses (2 = the warp room).
int Props::loadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    custom.clear();
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (char* hash = std::strchr(line, '#')) *hash = 0;
        char word[16] = {};
        unsigned level = 0;
        float x = 0, z = 0, floorY = 0, half = 0, bounce = 0;
        const int n = std::sscanf(line, " %15s %x %f %f %f %f %f", word, &level, &x, &z, &floorY, &half, &bounce);
        if (n < 6 || std::strcmp(word, "box") != 0 || half < 40.0f || half > 3000.0f) continue;
        Placement pl = makePlacement(level & 0xFF, x, floorY, z, half);
        if (n >= 7 && bounce > 0) pl.box.bounce = int32_t(std::min(bounce, 3000000.0f));
        custom.push_back(pl);
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

// The player's Crash against the boxes. Crash's feet are the object's position. A box is a wall at its sides (Crash is moved out along the shortest way) and a spring on its
// top: when the feet come down through the top, Crash is put on it and launched upwards, which the game's own jump code then carries on (it integrates the velocity itself).
void Props::interact(uint32_t obj) {
    const uint32_t lv = levelNow();
    if (lv != boxesLevel) loadLevel(lv);
    if (boxes.empty()) return;
    float feet[3] = {float(int32_t(p.peek32(obj + F(kX)))) / 256.0f, float(int32_t(p.peek32(obj + F(kY)))) / 256.0f, float(int32_t(p.peek32(obj + F(kZ)))) / 256.0f};
    const int32_t velY = int32_t(p.peek32(obj + F(kVelY)));
    if (const char* drop = std::getenv("CB2_PROPDROP")) {   // test: put Crash above the first box at this frame (counted from the load)
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
    if (debug() && std::getenv("CB2_PROPTRACE")) std::printf("[prop] f%llu feet %.0f %.0f %.0f vel %d %d %d squash %.2f\n", (unsigned long long)p.frames, feet[0], feet[1], feet[2], int(p.peek32(obj + F(kVelX))), velY, int(p.peek32(obj + F(kVelZ))), boxes[0].squash);
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
            // inside the height of the box: a wall
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

// The faces of a box that face the camera, each as a frame, a panel and a mark, as flat-coloured triangles in the frame's polygon list's coordinates.
void Props::drawBox(const Box& b, std::vector<psx::GpuCmd>& out, float& key, int offX, int offY, const psx::GpuCmd& tmpl) const {
    key = 0;
    const float squashed = 1.0f - kSquashDepth * b.squash;
    const float halfY = b.half[1] * squashed;
    const float base = b.centre[1] - b.half[1];
    const float centreY = base + halfY;
    const float half[3] = {b.half[0], halfY, b.half[2]};
    const float centre[3] = {b.centre[0], centreY, b.centre[2]};
    struct UV { float u, v; };
    auto emit = [&](const float world[4][3], const uint8_t rgb[3]) {
        float s[4][3];
        for (int i = 0; i < 4; ++i) if (!project(world[i], s[i])) return false;
        static const int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& t : tri) {
            psx::GpuCmd c = tmpl;
            c.kind = psx::GpuCmd::Kind::Tri;
            c.textured = false; c.raw = false; c.semi = false; c.alpha = 255; c.world = false; c.nativePoly = 0xFFFFFFFFu;
            c.worldOff[0] = c.worldOff[1] = 0;
            for (int k = 0; k < 3; ++k) {
                c.x[k] = s[t[k]][0] + float(offX);
                c.y[k] = s[t[k]][1] + float(offY);
                c.u[k] = c.v[k] = 0;
                c.w[k] = s[t[k]][2];
                for (int q = 0; q < 3; ++q) c.col[k][q] = rgb[q];
                key = std::max(key, s[t[k]][2]);
            }
            out.push_back(c);
        }
        return true;
    };
    // face f: the axis a it is perpendicular to and the side s; u and v run along the other two axes
    for (int a = 0; a < 3; ++a) for (int side = -1; side <= 1; side += 2) {
        if (a == 1 && side < 0) continue;   // the underside is never seen
        const int b1 = (a + 1) % 3, c1 = (a + 2) % 3;
        float centreFace[3] = {centre[0], centre[1], centre[2]};
        centreFace[a] += float(side) * half[a];
        const float toCam[3] = {cam.pos[0] - centreFace[0], cam.pos[1] - centreFace[1], cam.pos[2] - centreFace[2]};
        if (float(side) * toCam[a] <= 0) continue;   // facing away
        auto at = [&](float u, float v, float out3[3]) {
            out3[0] = centreFace[0]; out3[1] = centreFace[1]; out3[2] = centreFace[2];
            out3[b1] += (2 * u - 1) * half[b1];
            out3[c1] += (2 * v - 1) * half[c1];
        };
        auto quad = [&](const UV q[4], const uint8_t rgb[3]) {
            float w[4][3];
            for (int i = 0; i < 4; ++i) at(q[i].u, q[i].v, w[i]);
            return emit(w, rgb);
        };
        const bool top = a == 1;
        // light from above, the right and the front of the picture
        const float f = top ? 1.0f : (a == 0 ? (side > 0 ? 0.80f : 0.62f) : (side > 0 ? 0.72f : 0.55f));
        const uint8_t frame[3] = {shade(top ? 40 : 96, f), shade(top ? 110 : 52, f), shade(top ? 52 : 16, f)};
        const uint8_t panel[3] = {shade(top ? 96 : b.rgb[0], f), shade(top ? 214 : b.rgb[1], f), shade(top ? 104 : b.rgb[2], f)};
        const uint8_t mark[3] = {shade(top ? 255 : 120, f), shade(top ? 236 : 62, f), shade(top ? 90 : 20, f)};
        const UV all[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        if (!quad(all, frame)) continue;
        const UV inner[4] = {{0.10f, 0.10f}, {0.90f, 0.10f}, {0.90f, 0.90f}, {0.10f, 0.90f}};
        quad(inner, panel);
        if (top) {
            // an arrow pointing up the picture (towards +v... the far side): a bar and a head
            const UV bar[4] = {{0.42f, 0.22f}, {0.58f, 0.22f}, {0.58f, 0.60f}, {0.42f, 0.60f}};
            const UV head[4] = {{0.25f, 0.58f}, {0.75f, 0.58f}, {0.50f, 0.84f}, {0.50f, 0.84f}};
            quad(bar, mark);
            quad(head, mark);
        } else {
            const float w = 0.07f;
            const UV d1[4] = {{0.10f, 0.10f}, {0.10f + w * 2, 0.10f}, {0.90f, 0.90f}, {0.90f - w * 2, 0.90f}};
            const UV d2[4] = {{0.90f - w * 2, 0.10f}, {0.90f, 0.10f}, {0.10f + w * 2, 0.90f}, {0.10f, 0.90f}};
            quad(d1, mark);
            quad(d2, mark);
        }
    }
}

// Called when the game has sent its polygons for a picture to the GPU. The scene's list is the one after which the scenery projection ran; the box's triangles are put into it
// where its depth fits: before the polygons that are nearer than the box and after the ones that are farther (the game's own order is far to near).
void Props::onDisplayList() {
    if (debug() && p.frames % 60 == 0) std::printf("[prop] display list: enabled %d recording %d boxes %zu probe %llu (last %llu) cmds %zu\n", enabled, p.gpu.recording, boxes.size(), (unsigned long long)p.cpu.probeCount, (unsigned long long)lastProbeCount, p.gpu.cmds.size());
    if (!enabled || !p.gpu.recording) return;
    const uint32_t lv = levelNow();
    if (lv != boxesLevel) loadLevel(lv);
    if (boxes.empty() || p.cpu.probeCount == lastProbeCount) return;
    std::vector<psx::GpuCmd>& cmds = p.gpu.cmds;
    size_t start = 0;
    for (size_t i = cmds.size(); i-- > 0;) if (cmds[i].kind == psx::GpuCmd::Kind::Flip) { start = i + 1; break; }
    // a reference polygon of the scene: the box takes its draw area
    const psx::GpuCmd* ref = nullptr;
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
        depth.insert(depth.begin() + std::ptrdiff_t(at), mine.size(), key);   // the next box sees this one
    }
    ++drawnFrames;
    if (debug() && drawnFrames % 30 == 1) std::printf("[prop] frame %llu: box drawn (camera %.0f %.0f %.0f, h %.0f, known depths %zu)\n", (unsigned long long)p.frames, cam.pos[0], cam.pos[1], cam.pos[2], cam.h, known);
}

}  // namespace cb2prop
