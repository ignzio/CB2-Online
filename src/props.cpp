#include "props.hpp"

#include "entity.hpp"

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
constexpr uint32_t kCrateScript = 0x4AC42E4Du;   // the entry id of Boxs.C, the game's crate script (object type 34 in that level)
constexpr uint32_t kCrateType = 34;
constexpr int kMarkerField = 63;          // an unused field of the objects made here: tells them from the game's own
constexpr uint32_t kOldMarkers[2] = {0x50524F50u, 0x32505250u};   // the marks of the crates of older versions (they are taken away when a state has them)
constexpr uint32_t kMarkerTag = 0x43520000u;   // "CR": field 63 of a crate made here is this tag plus its place in Props::crates (+ 1)
constexpr float kLiveRadius = 2600.0f, kDropRadius = 3200.0f;   // a crate is made when Crash is this near, and thrown away (untouched) when he is this far
constexpr int kMaxLive = 20, kMaxPerFrame = 3;
bool debug() { static const bool d = std::getenv("CB2_PROPLOG") != nullptr; return d; }

// Where the props are. The warp room (level 2; the European language copies count as 2): on the floor ring of the room, a few steps to the right of where Crash starts, between the start and the Snow Go door.
const std::vector<Props::Placement>& builtIn() {
    static const std::vector<Props::Placement> list = { {0x02, 1166.0f, 471.0f, 112.5f, 1.0f, 1200000, "Bp10"} };
    return list;
}

// The crates placed by default: a test field in Turtle Woods ahead of where Crash starts (the level has the same shape in both releases), three rows of eight, one of each kind of the game's crates.
// The subtype of the crate script says which crate it is: 2 and 21 25 27-30 35-38 wooden, 10 26 34 "?" (fruit), 0 16 TNT, 18 Nitro, 3 arrow (bounce), 6 slats, 8 Crash (extra life), 9 Aku Aku mask.
std::vector<Props::Crate> builtInCrates() {
    static const int kRows[3][8] = {{2, 3, 0, 18, 6, 2, 16, 2}, {10, 8, 9, 10, 2, 10, 0, 3}, {10, 2, 10, 2, 9, 2, 8, 10}};
    std::vector<Props::Crate> list;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 8; ++col) list.push_back({kCrateLevel, 45024.0f + (float(col) - 3.5f) * 400.0f, -13.0f + float(row) * 400.0f, 172400.0f, kRows[row][col]});
    return list;
}

}  // namespace

Props::Props(psx::Platform& platform) : p(platform), crates(builtInCrates()) {}

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

// One prop per line, in world units; # starts a comment:
//   box <level, hex> <x> <z> <floor y> [<scale> [<bounce> [<crate>]]]      a host prop (a spring crate drawn by the program; the warp room is level 2)
//   crate <level, hex> <x> <floor y> <z> <subtype>                           one crate of the game's own crate script (Turtle Woods, level 1E)
//   wall <level, hex> <x> <floor y> <z> <x|z> <columns> <rows> <subtypes>   a grid of them, 400 apart, along x or z and upwards; the subtypes (a,b,c ...) are used in turn
// The scale is 1 for the size the game's crate has; the crate of a box is the name of one of the Turtle Woods crates (Bp10 = "?", the default).
int Props::loadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return 0;
    custom.clear();
    std::vector<Crate> placed;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (char* hash = std::strchr(line, '#')) *hash = 0;
        char word[16] = {};
        if (std::sscanf(line, " %15s", word) != 1) continue;
        unsigned level = 0;
        if (std::strcmp(word, "crate") == 0) {
            float x = 0, y = 0, z = 0;
            int sub = 0;
            if (std::sscanf(line, " %15s %x %f %f %f %d", word, &level, &x, &y, &z, &sub) == 6 && sub >= 0 && sub < 64) placed.push_back({level & 0xFF, x, y, z, sub});
            continue;
        }
        if (std::strcmp(word, "wall") == 0) {
            float x = 0, y = 0, z = 0;
            char axis[4] = {}, subs[128] = {};
            int cols = 0, rows = 0;
            if (std::sscanf(line, " %15s %x %f %f %f %3s %d %d %127s", word, &level, &x, &y, &z, axis, &cols, &rows, subs) != 9 || cols < 1 || rows < 1 || cols * rows > 4096) continue;
            std::vector<int> list;
            for (const char* c = subs; *c;) { list.push_back(std::atoi(c)); while (*c && *c != ',') ++c; if (*c) ++c; }
            if (list.empty()) continue;
            size_t k = 0;
            for (int row = 0; row < rows; ++row)
                for (int col = 0; col < cols; ++col, ++k)
                    placed.push_back({level & 0xFF, x + (axis[0] == 'x' ? float(col) * 400.0f : 0.0f), y + float(row) * 400.0f, z + (axis[0] == 'x' ? 0.0f : float(col) * 400.0f), list[k % list.size()] & 63});
            continue;
        }
        float x = 0, z = 0, floorY = 0, scale = 1, bounce = 1200000;
        char model[16] = "Bp10";
        const int n = std::sscanf(line, " %15s %x %f %f %f %f %f %15s", word, &level, &x, &z, &floorY, &scale, &bounce, model);
        if (n < 5 || std::strcmp(word, "box") != 0 || scale < 0.1f || scale > 10.0f) continue;
        custom.push_back({level & 0xFF, x, z, floorY, scale, int32_t(std::clamp(bounce, 100000.0f, 3000000.0f)), model});
    }
    std::fclose(f);
    boxesLevel = 0xFFFFFFFFu;
    if (!placed.empty()) { crates = placed; crateLevel = 0xFFFFFFFFu; }
    return int(custom.size() + placed.size());
}

void Props::install() {
    p.addHook(cb2game::current().objectStep, [this](psx::Platform& pl) { onStepEnter(pl.cpu.r[4]); });
    p.addHook(cb2game::current().objectStepReturn, [this](psx::Platform&) { onStepReturn(); });
    const auto prev = p.onDisplayList;
    p.onDisplayList = [this, prev] { if (prev) prev(); onDisplayList(); };
    p.cpu.probe = &probe;
    p.cpu.probePcs = cb2game::current().sceneryProjection;
}

void Props::onStepEnter(uint32_t obj) {
    stepping = obj;
    if (enabled && p.frames != crateCheckedFrame) { crateCheckedFrame = p.frames; streamCrates(); }
}

bool Props::isCrateObject(uint32_t a) {
    if (!isObject(a)) return false;
    const uint32_t e = p.peek32(a + 0xC);
    return e >= kRamLow && e < kRamHigh - 16 && p.peek32(e + 4) == kCrateScript;
}

bool Props::transact(const char* what, const std::function<void()>& f) {
    struct Busy { bool& b; explicit Busy(bool& x) : b(x) { b = true; } ~Busy() { b = false; } } busy(building);   // the engine's routines run object steps, which come back here
    const uint64_t oldBudget = p.callBudget;
    p.callBudget = 600'000;
    undoRam.assign(p.ram.begin(), p.ram.end());
    undoScratch.assign(p.cpu.scratch, p.cpu.scratch + sizeof p.cpu.scratch);
    bool ok = true;
    try {
        f();
    } catch (const std::exception& e) {
        std::memcpy(p.ram.data(), undoRam.data(), undoRam.size());
        std::memcpy(p.cpu.scratch, undoScratch.data(), undoScratch.size());
        std::printf("props: %s failed (%s); the game's memory was put back\n", what, e.what());
        ok = false;
    }
    p.callBudget = oldBudget;
    return ok;
}

// A state (or a level that has just been entered) may hold crates made here: they are taken back into the list. Crates of older versions, or ones that are not in the list, are taken away.
// Also finds the parent the engine gives the level's crates (the one of a crate of the level itself when there is one).
void Props::adoptCrates(uint32_t level) {
    crateAdopted = true;
    uint32_t tmpl = 0;
    std::vector<uint32_t> orphans;
    for (uint32_t a = kRamLow; a < kRamHigh - 0x280; a += 4) {
        if (!isObject(a)) continue;
        const uint32_t mk = p.peek32(a + F(kMarkerField));
        if ((mk & 0xFFFF0000u) == kMarkerTag) {
            const size_t idx = size_t(mk & 0xFFFFu) - 1;
            if (idx < crates.size() && crates[idx].level == level && crates[idx].obj == 0 && int(p.peek32(a + F(29))) == crates[idx].sub && isCrateObject(a)) {
                crates[idx].obj = a;
                crates[idx].madeX = float(int32_t(p.peek32(a + F(kX)))) / 256.0f; crates[idx].madeY = float(int32_t(p.peek32(a + F(kY)))) / 256.0f; crates[idx].madeZ = float(int32_t(p.peek32(a + F(kZ)))) / 256.0f;
            } else orphans.push_back(a);
            continue;
        }
        if (mk == kOldMarkers[0] || mk == kOldMarkers[1]) { orphans.push_back(a); continue; }
        if (!tmpl && isCrateObject(a)) tmpl = a;
    }
    crateParent = tmpl ? p.peek32(tmpl + 0x44) : cb2game::current().puppetParent + 0x18;
    if (!orphans.empty()) {
        transact("taking away old crates", [&] { for (const uint32_t o : orphans) if (isObject(o)) p.callFunction(cb2game::current().destroyObject, {o, 0}); });
        std::printf("props: %zu crates of an older version taken away\n", orphans.size());
    }
}

// Crates near Crash are made, crates far from him are thrown away. A crate the game has removed (broken, exploded, collected) stays removed until the level is entered again or Crash is made anew.
void Props::streamCrates() {
    if (building) return;
    const uint32_t lv = levelNow();
    if (lv != crateLevel) {
        crateLevel = lv;
        for (Crate& c : crates) { c.obj = 0; c.gone = false; }
        crateAdopted = false; crateQuietSince = 0; lastPlayer = 0; liveCount = 0;
    }
    bool any = false;
    for (const Crate& c : crates) if (c.level == lv) { any = true; break; }
    if (!any || lv != kCrateLevel || !findPlayer()) return;
    if (!crateAdopted) {   // the engine must have finished loading the level first (it is not reading the disc any more)
        if (p.cd.sectorsRead != crateLastSectors) { crateLastSectors = p.cd.sectorsRead; crateQuietSince = p.frames; return; }
        if (p.frames < crateQuietSince + 45) return;
        adoptCrates(lv);
        lastPlayer = player;
        return;
    }
    if (player != lastPlayer) {   // a new Crash (he fell, a checkpoint): the world was made again
        lastPlayer = player;
        for (Crate& c : crates) { if (c.level == lv && c.obj && isCrateObject(c.obj)) c.obj = 0; c.gone = false; }
        crateAdopted = false;
        return;
    }
    if (p.frames % 3) return;
    const float px = float(int32_t(p.peek32(player + F(kX)))) / 256.0f, py = float(int32_t(p.peek32(player + F(kY)))) / 256.0f, pz = float(int32_t(p.peek32(player + F(kZ)))) / 256.0f;
    std::vector<std::pair<float, size_t>> wanted;
    std::vector<size_t> drop;
    int live = 0;
    for (size_t i = 0; i < crates.size(); ++i) {
        Crate& c = crates[i];
        if (c.level != lv) continue;
        const float d = std::hypot(c.x - px, c.z - pz);
        if (c.obj) {
            if (!isCrateObject(c.obj) || p.peek32(c.obj + F(kMarkerField)) != (kMarkerTag | uint32_t(i + 1))) {
                c.obj = 0; c.gone = true;
                if (debug()) std::printf("[crate] %zu (subtype %d) is gone\n", i, c.sub);
                continue;
            }
            ++live;
            const float mx = float(int32_t(p.peek32(c.obj + F(kX)))) / 256.0f, my = float(int32_t(p.peek32(c.obj + F(kY)))) / 256.0f, mz = float(int32_t(p.peek32(c.obj + F(kZ)))) / 256.0f;
            const bool untouched = std::fabs(mx - c.madeX) < 60 && std::fabs(my - c.madeY) < 100 && std::fabs(mz - c.madeZ) < 60;
            if (d > kDropRadius && untouched) drop.push_back(i);
        } else if (!c.gone && d < kLiveRadius && std::fabs(c.y - py) < 2500.0f) {
            wanted.push_back({d, i});
        }
    }
    liveCount = live;
    if (!drop.empty()) {
        transact("throwing crates away", [&] { for (const size_t i : drop) p.callFunction(cb2game::current().destroyObject, {crates[i].obj, 0}); });
        for (const size_t i : drop) { crates[i].obj = 0; --liveCount; }
    }
    std::sort(wanted.begin(), wanted.end());
    if (wanted.size() > size_t(std::max(0, kMaxLive - liveCount))) wanted.resize(size_t(std::max(0, kMaxLive - liveCount)));
    if (wanted.size() > size_t(kMaxPerFrame)) wanted.resize(size_t(kMaxPerFrame));
    if (wanted.empty()) return;
    const bool ok = transact("making crates", [&] {
        for (const auto& w : wanted) {
            Crate& c = crates[w.second];
            const uint32_t o = p.callFunction(cb2game::current().createObject, {crateParent, kCrateType, uint32_t(c.sub), 0}, {0, 0});
            if (!isObject(o)) { c.gone = true; continue; }
            p.poke32(o + F(kX), uint32_t(int32_t(c.x * 256.0f)));
            p.poke32(o + F(kY), uint32_t(int32_t(c.y * 256.0f)));
            p.poke32(o + F(kZ), uint32_t(int32_t(c.z * 256.0f)));
            p.poke32(o + F(kMarkerField), kMarkerTag | uint32_t(w.second + 1));
            c.obj = o; c.madeX = c.x; c.madeY = c.y; c.madeZ = c.z;
            ++liveCount;
        }
    });
    if (!ok) for (const auto& w : wanted) { crates[w.second].obj = 0; crates[w.second].gone = true; }
    if (debug()) std::printf("[crate] frame %llu: made %zu (ok %d), %d live of %zu placed\n", (unsigned long long)p.frames, wanted.size(), int(ok), liveCount, crates.size());
}

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
