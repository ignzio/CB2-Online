#include "multiplayer.hpp"

#include "roster.hpp"
#include "runtracker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

namespace cb2mp {
namespace {

// Field numbers of the game's process object (object + 0x40 + 4 * n; names in gool_fields.inc)
enum Field { kSelf = 0, kPlayer = 5, kX = 8, kY = 9, kZ = 10, kRotX = 11, kRotY = 12, kRotZ = 13, kScaleX = 14, kScaleY = 15, kScaleZ = 16,
             kStatusA = 26, kFrameGroup = 42, kFrameNum = 43 };

constexpr uint32_t kObjectType = 3;       // the value the engine stores at object + 4 for a live process
constexpr uint32_t kEntryPtr = 0xC;       // pointer to the object's GOOL script entry
constexpr uint32_t kTransBlock = 0x40 + 4 * 34, kEventBlock = 0x40 + 4 * 35, kHeadBlock = 0x40 + 4 * 36;   // tpc, epc, hpc: script block pointers (fields 34-36)
constexpr uint32_t kRamLow = 0x80010000, kRamHigh = 0x80200000;
constexpr int kMarkerField = 63;          // an unused field of the extra object: holds kMarker so we can tell our objects from reused memory
constexpr uint32_t kMarker = 0x4D504C59;

bool debug() { static const bool d = std::getenv("CB2_MPDEBUG") != nullptr; return d; }

}  // namespace

double Multiplayer::now() const {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

bool Multiplayer::start(const Options& options, std::string& error) {
    opt = options;
    playerName = opt.name;
    if (opt.fake) { if (playerName.empty()) playerName = "GHOST"; }
    if (playerName.empty()) { const char* u = std::getenv("USERNAME"); playerName = u && *u ? u : "CRASH"; }
    playerName = playerName.substr(0, cb2net::kNameLength - 1);
    if (opt.fake) return true;
    bool ok = false;
    if (opt.online) ok = net.start({opt.room, playerName, opt.rendezvous, opt.directory, opt.seeds, opt.joinViaDirectory, opt.password}, error);
    else error = "online play is not switched on";
    if (ok) announce(0xFF);
    return ok;
}

// What others see of us while we are not in a level, or when we have just entered one: our name, no Crash yet.
void Multiplayer::applyRuns(cb2net::PlayerState& s) const {
    if (!runs) return;
    s.bestMs = runs->bestMsFor(int(s.level));
    s.lastLevel = uint8_t(runs->lastRun().level);
    s.lastMs = runs->lastRun().ms;
    s.lastFinished = runs->lastRun().finished ? 1 : 0;
    s.runCount = runCounter;
}

void Multiplayer::announce(uint32_t lvl) {
    cb2net::PlayerState s;
    s.level = uint8_t(lvl);
    applyRuns(s);
    std::strncpy(s.name, playerName.c_str(), cb2net::kNameLength - 1);
    s.fgrp = 0;
    net.setLocal(s);
}

void Multiplayer::install() {
    p.addHook(cb2game::current().objectStep, [this](psx::Platform& pl) { onStepEnter(pl.cpu.r[4]); });
    p.addHook(cb2game::current().objectStepReturn, [this](psx::Platform&) { onStepReturn(); });
    // Right after the object update has worked out which model the current frame uses and before it draws it: if the lookup failed for one
    // of our objects (a pose whose model is not loaded) the object is left undrawn instead of crashing the draw code.
    p.addHook(cb2game::current().afterAnimate, [this](psx::Platform& pl) {
        const uint32_t obj = pl.cpu.r[17];
        if (!steppingPuppet || obj != steppingPuppet->obj) return;
        Puppet& pu = *steppingPuppet;
        const uint32_t fg = pl.peek32(obj + F(kFrameGroup));
        if (recordSound(obj)) {
            if (fg == pu.pose) { pu.lastDrawable = fg; pu.lastDrawableFnum = int32_t(pl.peek32(obj + F(kFrameNum))); }
            return;
        }
        // not drawable: do not let the game draw it this time, and do not offer this pose again for a while
        if (fg == pu.pose) { pu.badPose = fg; pu.badUntil = pl.frames + 120; }
        if (poseType(obj) == 1 || !recordValid(obj)) pl.poke32(obj + F(kFrameGroup), 0);
    });
    p.onDisplayList = [this] { buildTags(); };
    // The extra Crash's own script must not run: left alone it eventually executes a "return" and the engine deletes the object. Every
    // entry into the script interpreter for one of our objects is answered here with the usual "carry on" result.
    p.addHook(cb2game::current().runScript, [this](psx::Platform& pl) {
        const uint32_t o = pl.cpu.r[4];
        if (puppets.empty() || !isOurs(o)) return;
        pl.cpu.r[2] = 0xFFFFFF01u;
        pl.cpu.pc = pl.cpu.r[31];
        pl.cpu.nextPc = pl.cpu.pc + 4;
    });
    p.addHook(cb2game::current().openLevel, [this](psx::Platform& pl) {
        if (debug()) std::printf("[mp] frame %llu: open-level call a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x\n", (unsigned long long)pl.frames, pl.cpu.r[4], pl.cpu.r[5], pl.cpu.r[6], pl.cpu.r[7], pl.cpu.r[31]);
        if (pl.cpu.r[4] == 0) onLevel(pl.cpu.r[5]);
    });
}

void Multiplayer::stateLoaded(uint32_t levelNow) {
    level = cb2game::canonicalLevel(cb2game::current().region, levelNow);
    levelStartFrame = p.frames;
    player = playerSeen = 0;
    playerSteps = lastPlayerStep = 0;
    managedFrame = ~0ull;
    puppets.clear();
    zombies.clear();
    steppingPuppet = nullptr;
    stepping = 0;
    samples.clear();
    p.cpu.gteSamples = nullptr;
    if (net.active()) announce(level);
}

std::string Multiplayer::status() const {
    if (!active()) return "offline";
    std::string s = opt.fake ? std::string("test ghost") : net.status();
    if (level == 0xFF) s += " (waiting for a level)";
    else if (!player) s += " (no Crash here)";
    return s;
}

bool Multiplayer::isObject(uint32_t a) {
    if (a < kRamLow || a >= kRamHigh - 0x280 || (a & 3)) return false;
    // the engine's kill routine unlinks an object and zeroes its first word but leaves the rest of the memory alone
    return p.peek32(a) != 0 && p.peek32(a + F(kSelf)) == a && p.peek32(a + 4) == kObjectType;
}

bool Multiplayer::recordValid(uint32_t obj) {
    const uint32_t record = p.peek32(obj + 8);
    return record >= kRamLow && record < kRamHigh - 0x40;
}

// The type byte of the frame group an object currently shows: 1 animated model (needs a model pointer in the record), 2 and 5 other
// drawable kinds, 4 not drawn, 0 none.
uint32_t Multiplayer::poseType(uint32_t obj) {
    const uint32_t fg = p.peek32(obj + F(kFrameGroup));
    if (fg < kRamLow || fg >= kRamHigh - 0x40) return 0;
    return p.peek32(fg & ~3u) >> ((fg & 3u) * 8) & 0xFF;
}

// Whether the game can draw this object right now without following a bad pointer: the model pointer the update computed must be a
// real address for animated models (type 1); the other drawable kinds do not use it.
bool Multiplayer::recordSound(uint32_t obj) {
    if (!recordValid(obj)) return false;
    const uint32_t type = poseType(obj);
    if (type == 2 || type == 5) return true;
    const uint32_t model = p.peek32(p.peek32(obj + 8) + 0x28);
    return model >= kRamLow && model < kRamHigh - 0x40;
}

bool Multiplayer::isOurs(uint32_t a) {
    return isObject(a) && p.peek32(a + F(kMarkerField)) == kMarker && p.peek32(a + 0x44) == cb2game::current().puppetParent;
}

// The game's current player object. Normally its "player" field points at itself, but after a death / respawn or a teleport the engine can
// leave that field pointing at another object while the object is still the live Crash, so the global that names the current player counts too.
bool Multiplayer::isPlayer(uint32_t a) {
    return a && isObject(a) && (p.peek32(a + F(kPlayer)) == a || a == p.peek32(cb2game::current().playerGlobal));
}

bool Multiplayer::isPuppet(uint32_t a) {
    return a && player && a != player && isObject(a) && p.peek32(a + F(kMarkerField)) == kMarker && p.peek32(a + F(29)) == kPuppetSubtype &&
           p.peek32(a + kEntryPtr) == p.peek32(player + kEntryPtr) && p.peek32(a + 0x44) == cb2game::current().puppetParent;
}

uint32_t Multiplayer::findPlayer() {
    // The game keeps its current player object in a global. The memory of an earlier Crash (a cutscene's) stays intact after the engine drops
    // it, so a scan alone would keep finding that stale copy.
    const uint32_t current = p.peek32(cb2game::current().playerGlobal);
    if (isPlayer(current)) { player = current; return player; }
    player = 0;
    return 0;
}

void Multiplayer::onLevel(uint32_t id) {
    if (debug()) std::printf("[mp] frame %llu: level %02X opened\n", (unsigned long long)p.frames, id & 0xFF);
    level = cb2game::canonicalLevel(cb2game::current().region, id);   // the language versions of the PAL warp room are one level for the other players
    levelStartFrame = p.frames;
    announce(level);   // until the new level's Crash exists others must not keep drawing the old one
    player = 0;
    playerSteps = 0;
    puppets.clear();       // the engine throws every object away when a level is loaded
    zombies.clear();
    steppingPuppet = nullptr;
    stepping = 0;
    p.cpu.gteSamples = nullptr;
}

void Multiplayer::onStepEnter(uint32_t obj) {
    if (obj == player && player) {
        if (lastPlayerStep != p.frames) { lastPlayerStep = p.frames; ++playerSteps; }
    }
    stepping = obj;
    steppingPuppet = nullptr;
    if (p.frames != managedFrame) {
        managedFrame = p.frames;
        try { manage(); }
        catch (const std::exception& e) {   // nothing in the online code may end the game
            ++prof.failures;
            disabledUntil = p.frames + 600;
            std::printf("[mp] frame %llu: online update failed (%s); paused for ten seconds\n", (unsigned long long)p.frames, e.what());
            std::fflush(stdout);
        }
    }
    bool ours = false;
    for (auto& [id, pu] : puppets) {
        if (pu.obj != obj || !isPuppet(obj)) continue;
        ours = true;
        // The record at +8 must be a real address or the update would read garbage. Everything else is repaired by the update itself, which
        // recomputes the model pointer from the pose written here, so the pose is written before every update (also the first one).
        if (!recordValid(obj)) {
            ++pu.badRecordSteps;
            p.cpu.r[2] = 0;                 // "keep the object"
            p.cpu.pc = p.cpu.r[31];         // return straight away
            p.cpu.nextPc = p.cpu.pc + 4;
            stepping = 0;
            return;
        }
        steppingPuppet = &pu;
        pu.lastStep = p.frames;
        for (const cb2net::PlayerState& s : others())
            if (s.id == id) { drive(pu, s); break; }
        break;
    }
    // One of our objects that we have removed (or could not remove yet) but the engine's loop still reaches: leave it alone.
    if (!ours && p.peek32(obj + F(kMarkerField)) == kMarker && p.peek32(obj + 0x44) == cb2game::current().puppetParent && p.peek32(obj + 4) == kObjectType) {
        stepping = 0;
        p.cpu.r[2] = 0;
        p.cpu.pc = p.cpu.r[31];
        p.cpu.nextPc = p.cpu.pc + 4;
    }
}

void Multiplayer::onStepReturn() {
    const uint32_t obj = stepping;
    stepping = 0;
    if (steppingPuppet) {
        Puppet* pu = steppingPuppet;
        steppingPuppet = nullptr;
        // The update has just built the model matrix for this puppet; its column lengths are the puppet's signature among everything drawn.
        const uint32_t* c = p.cpu.cop2c;
        const int16_t m[3][3] = {{int16_t(c[0]), int16_t(c[0] >> 16), int16_t(c[1])},
                                 {int16_t(c[1] >> 16), int16_t(c[2]), int16_t(c[2] >> 16)},
                                 {int16_t(c[3]), int16_t(c[3] >> 16), int16_t(c[4])}};
        for (int k = 0; k < 3; ++k)
            pu->column[k] = std::sqrt(double(m[0][k]) * m[0][k] + double(m[1][k]) * m[1][k] + double(m[2][k]) * m[2][k]);
        pu->haveColumns = true;
        return;
    }
    if (obj && obj == player) publish();
}

void Multiplayer::buildTags() {
    struct Timer { double* sink; double t0; ~Timer() {} } timer{&prof.tagsMs, 0};
    const auto began = std::chrono::steady_clock::now();
    struct Done { Multiplayer* m; std::chrono::steady_clock::time_point b; ~Done() { m->prof.tagsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b).count(); } } done{this, began};
    if (puppets.empty() || samples.empty()) { samples.clear(); return; }
    using Mat = std::array<double, 9>;   // row-major, in the GTE's 1.3.12 fixed point units (4096 = 1.0)
    struct Group { Mat rt; size_t count = 0; float top = 1e9f, centre = 0, middle = 0; size_t begin = 0, end = 0; };
    std::vector<Group> groups;
    // consecutive vertices projected with the same transform belong together (one model part, or the level)
    size_t start = 0;
    auto same = [&](const r3000::GteSample& a, const r3000::GteSample& b) { return std::memcmp(a.rt, b.rt, sizeof a.rt) == 0 && std::memcmp(a.tr, b.tr, sizeof a.tr) == 0; };
    for (size_t n = 1; n <= samples.size(); ++n) {
        if (n < samples.size() && same(samples[n], samples[start])) continue;
        if (n - start >= 8) {
            const auto& s = samples[start];
            Group g;
            g.rt = {double(int16_t(s.rt[0])), double(int16_t(s.rt[0] >> 16)), double(int16_t(s.rt[1])),
                    double(int16_t(s.rt[1] >> 16)), double(int16_t(s.rt[2])), double(int16_t(s.rt[2] >> 16)),
                    double(int16_t(s.rt[3])), double(int16_t(s.rt[3] >> 16)), double(int16_t(s.rt[4]))};
            g.count = n - start;
            g.begin = start;
            g.end = n;
            std::vector<float> xs, ys;
            for (size_t q = start; q < n; ++q) {
                const auto& v = samples[q];
                if (v.sz <= 0 || v.sx < -1000 || v.sx > 1000 || v.sy < -1000 || v.sy > 1000) continue;
                xs.push_back(v.sx);
                ys.push_back(v.sy);
            }
            if (xs.size() >= 8) {
                // robust extent: a few stray vertices must not drag the tag around
                std::sort(xs.begin(), xs.end());
                std::sort(ys.begin(), ys.end());
                g.top = ys[ys.size() / 50];
                g.centre = xs[xs.size() / 2];
                g.middle = ys[ys.size() / 2];
            }
            groups.push_back(g);
        }
        start = n;
    }
    // The level is drawn with the camera's matrix alone and has by far the most vertices; every object is that matrix times the object's own.
    const Group* camera = nullptr;
    for (const Group& g : groups) if (g.count >= 1000 && (!camera || g.count > camera->count)) camera = &g;
    if (!camera) { samples.clear(); return; }
    const Mat& c = camera->rt;
    const double det = c[0] * (c[4] * c[8] - c[5] * c[7]) - c[1] * (c[3] * c[8] - c[5] * c[6]) + c[2] * (c[3] * c[7] - c[4] * c[6]);
    if (std::abs(det) < 1e3) { samples.clear(); return; }
    Mat inv = {(c[4] * c[8] - c[5] * c[7]) / det, (c[2] * c[7] - c[1] * c[8]) / det, (c[1] * c[5] - c[2] * c[4]) / det,
               (c[5] * c[6] - c[3] * c[8]) / det, (c[0] * c[8] - c[2] * c[6]) / det, (c[2] * c[3] - c[0] * c[5]) / det,
               (c[3] * c[7] - c[4] * c[6]) / det, (c[1] * c[6] - c[0] * c[7]) / det, (c[0] * c[4] - c[1] * c[3]) / det};
    for (double& v : inv) v *= 4096.0;   // so that inverse * matrix stays in the same units as the matrix

    const bool seeThrough = opacity < 100;
    p.gpu.alphaValue = seeThrough ? uint8_t(std::max(10, opacity) * 255 / 100) : uint8_t(255);
    int index = 0;
    for (auto& [id, pu] : puppets) {
        ++index;
        if (!pu.haveColumns || pu.name.empty()) continue;
        // The object's own matrix has columns as long as the object's scale on each axis, whatever the camera does; the puppet's slot makes
        // those lengths unique among the Crashes on screen.
        const Group* best = nullptr;
        double bestErr = 0.0012;
        for (const Group& g : groups) {
            if (&g == camera || g.count < 200 || g.top > 1e8f) continue;
            double err = 0;
            for (int col = 0; col < 3; ++col) {
                double len = 0;
                for (int row = 0; row < 3; ++row) {
                    double v = 0;
                    for (int k = 0; k < 3; ++k) v += inv[row * 3 + k] * g.rt[k * 3 + col];
                    len += v * v;
                }
                err = std::max(err, std::abs(std::sqrt(len) - pu.column[col]) / std::max(1.0, pu.column[col]));
            }
            if (err < bestErr) { bestErr = err; best = &g; }
            // every part with this puppet's matrix signature is the puppet: mark its vertices so the renderer can draw them see-through
            if (seeThrough && err < 0.0012)
                for (size_t q = g.begin; q < g.end; ++q) p.gpu.markAlpha(samples[q].sx, samples[q].sy);
        }
        static unsigned calls = 0;
        if (debug() && (++calls % 120) == 0) std::printf("[mp] frame %llu: \"%s\" %s\n", (unsigned long long)p.frames, pu.name.c_str(), best ? "is on screen" : "is not on screen");
        // only while the puppet is really in view: its middle must lie on the picture (a tag at the screen edge for somebody far away is wrong)
        if (!best || best->centre < 8 || best->centre > float(p.gpu.hres) - 8 || best->middle < 8 || best->middle > float(p.gpu.vres) - 8) continue;
        psx::Gpu::TextTag tag;
        tag.text = pu.name;
        tag.x = best->centre;
        tag.y = std::max(best->top - 3.0f, 14.0f);
        {
            const uint32_t c = cb2roster::playerRgb(id);   // 0xRRGGBB -> the GPU's BGR
            tag.rgb = ((c & 0xFF) << 16) | (c & 0xFF00) | (c >> 16);
        }
        if (showTags) p.gpu.queueTag(tag);
    }
    samples.clear();
}

void Multiplayer::publish() {
    if (!isPlayer(player) || level == 0xFF || !recordValid(player)) { if (debug() && p.frames % 30 == 0) std::printf("[mp] frame %llu: publish skipped (player/level/record)\n", (unsigned long long)p.frames); return; }
    const uint32_t pose = poseType(player);
    if (pose != 1 && pose != 2 && pose != 4 && pose != 5) { if (debug() && p.frames % 30 == 0) std::printf("[mp] frame %llu: publish skipped (pose type %u)\n", (unsigned long long)p.frames, pose); return; }   // not a pose the game can draw (cutscenes)
    cb2net::PlayerState s;
    s.level = uint8_t(level);
    std::strncpy(s.name, playerName.c_str(), cb2net::kNameLength - 1);
    for (int i = 0; i < 3; ++i) {
        s.pos[i] = int32_t(p.peek32(player + F(kX + i)));
        s.rot[i] = int32_t(p.peek32(player + F(kRotX + i)));
        s.scale[i] = int32_t(p.peek32(player + F(kScaleX + i)));
    }
    s.fgrp = int32_t(p.peek32(player + F(kFrameGroup)) - p.peek32(player + kEntryPtr));
    s.fnum = int32_t(p.peek32(player + F(kFrameNum)));
    s.flags = p.peek32(player + F(kStatusA));
    if (!haveLocal && debug()) std::printf("[mp] frame %llu: player object %08x, level %02X\n", (unsigned long long)p.frames, player, level);
    applyRuns(s);
    localState = s;
    haveLocal = true;
    net.setLocal(s);
}

void Multiplayer::tick() {
    if (!net.active()) return;
    const double t0 = now();
    net.update(t0);
    prof.tickMs += (now() - t0) * 1000.0;
    ++prof.tickCalls;
}

std::vector<cb2net::PlayerState> Multiplayer::others() {
    if (!opt.fake) {
        std::vector<cb2net::PlayerState> v = net.others();
        // players of a version that sends the raw number of their language's warp room are in the warp room all the same
        for (cb2net::PlayerState& s : v) s.level = uint8_t(cb2game::canonicalLevel(cb2game::current().region, s.level));
        return v;
    }
    std::vector<cb2net::PlayerState> v;
    if (!haveLocal || level == 0xFF) return v;
    if (opt.fake >= 2 && (p.frames / 360) % 2 == 1) return v;   // away for six seconds out of twelve
    cb2net::PlayerState g = localState;
    g.id = 1;
    std::strncpy(g.name, playerName.c_str(), cb2net::kNameLength - 1);
    g.pos[0] += 0x40000;    // 2.5 units to the side and a little further from the camera
    g.pos[2] -= 0x30000;
    v.push_back(g);
    return v;
}

bool Multiplayer::engineBusy() {
    if (p.cd.sectorsRead != lastCdSectors) { lastCdSectors = p.cd.sectorsRead; lastDiscFrame = p.frames; }
    return p.frames < lastDiscFrame + 30;
}

template <class Fn> bool Multiplayer::guarded(const char* what, Fn&& fn) {
    // A function called into the game gets a budget of a few hundred thousand instructions (creating an object takes a few thousand).
    static const uint64_t budget = std::getenv("CB2_MPBUDGET") ? std::strtoull(std::getenv("CB2_MPBUDGET"), nullptr, 10) : 400'000;
    const uint64_t old = p.callBudget;
    p.callBudget = budget;
    // Transaction: a call that fails halfway (a fault, or it ran out of budget) has already changed the engine's memory; put all of it back, so that
    // the engine carries on exactly as if the call had never been made.
    undoRam.assign(p.ram.begin(), p.ram.end());
    undoScratch.assign(p.cpu.scratch, p.cpu.scratch + sizeof p.cpu.scratch);
    bool ok = true;
    try { fn(); }
    catch (const std::exception& e) {
        ok = false;
        ++prof.failures;
        std::memcpy(p.ram.data(), undoRam.data(), undoRam.size());
        std::memcpy(p.cpu.scratch, undoScratch.data(), undoScratch.size());
        std::printf("[mp] frame %llu: %s failed inside the game (%s); online play carries on\n", (unsigned long long)p.frames, what, e.what());
        std::fflush(stdout);
    }
    p.callBudget = old;
    return ok;
}

uint32_t Multiplayer::spawnPuppet() {
    ++prof.spawns;
    uint32_t obj = 0;
    static const bool forceFault = std::getenv("CB2_MPFAULT") != nullptr;   // tests: the first spawns call the engine with a bad parent
    const uint32_t parent = (forceFault && prof.spawns <= 2) ? 0xFFFFFFF0u : cb2game::current().puppetParent;
    const bool ok = guarded("creating a player's Crash", [&] { obj = p.callFunction(cb2game::current().createObject, {parent, 0, kPuppetSubtype, 0}, {0, 0}); });
    if (!ok) {
        // do not hammer the engine: wait, and give up on this level after a few failures in a row
        retryAt = p.frames + 120 * uint64_t(1 + spawnFailures);
        if (++spawnFailures >= 4) disabledUntil = p.frames + 1800;
        return 0;
    }
    spawnFailures = 0;
    if (std::getenv("CB2_MPDEBUG")) std::printf("[mp] frame %llu: creating a Crash took %llu guest instructions\n", (unsigned long long)p.frames, (unsigned long long)p.lastCallInstructions);
    return isObject(obj) && obj != player ? obj : 0;
}

void Multiplayer::killObject(uint32_t obj) {
    ++prof.kills;
    guarded("removing a player's Crash", [&] { p.callFunction(cb2game::current().destroyObject, {obj, 0}); });
    if (isObject(obj) && std::find(zombies.begin(), zombies.end(), obj) == zombies.end()) zombies.push_back(obj);
}

void Multiplayer::destroyPuppet(Puppet& pu) {
    if (isObject(pu.obj) && p.peek32(pu.obj + F(kMarkerField)) == kMarker && pu.obj != stepping) killObject(pu.obj);
    pu.obj = 0;
}

// Once per frame, at the first object update: make the set of puppets match the players who are here.
void Multiplayer::manage() {
    const auto began = std::chrono::steady_clock::now();
    struct Done { Multiplayer* m; std::chrono::steady_clock::time_point b; ~Done() { m->prof.manageMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b).count(); } } done{this, began};
    if (!findPlayer() || level == 0xFF || level == 0x3C) {
        // With no Crash to work from, still forget puppets the engine has thrown away (a level restart frees every object).
        for (auto it = puppets.begin(); it != puppets.end();) {
            if (!isOurs(it->second.obj)) it = puppets.erase(it); else ++it;
        }
        if (debug() && p.frames % 60 == 0) {
            const uint32_t g = p.peek32(cb2game::current().playerGlobal);
            auto dump = [&](const char* tag, uint32_t a) {
                std::printf("[mp]   %s %08x: first %08x type %08x record %08x entry %08x parent %08x pos %d,%d,%d fgrp %08x(type %u) f5 %08x f29 %08x f63 %08x\n", tag, a, p.peek32(a), p.peek32(a + 4), p.peek32(a + 8), p.peek32(a + 0xC),
                            p.peek32(a + 0x44), int(p.peek32(a + F(kX))), int(p.peek32(a + F(kY))), int(p.peek32(a + F(kZ))), p.peek32(a + F(kFrameGroup)), poseType(a), p.peek32(a + F(5)), p.peek32(a + F(29)), p.peek32(a + F(63)));
            };
            if (g && p.frames % 60 == 0) { dump("global", g); dump("its field5", p.peek32(g + F(kPlayer))); }
            std::printf("[mp] frame %llu: no player yet (level %02X): global %08x isObject %d first %08x self %08x type %08x field5 %08x\n", (unsigned long long)p.frames, level, g, int(isObject(g)),
                        p.peek32(g), p.peek32(g + F(kSelf)), p.peek32(g + 4), p.peek32(g + F(kPlayer)));
        }
        return;
    }
    for (size_t i = 0; i < zombies.size();) {
        const uint32_t z = zombies[i];
        if (isObject(z) && p.frames % 30 == 0) killObject(z);
        if (!isObject(z)) zombies.erase(zombies.begin() + long(i)); else ++i;
    }
    if (player != playerSeen) { playerSeen = player; playerSteps = 0; }
    // Right after a level (or cutscene) starts the engine is still rebuilding its tables. The player pointer alone cannot tell that the new
    // world is running (it may still point at the previous level's Crash), so wait until the game has been seen updating this Crash for a
    // while, and only create objects while it still does.
    const bool running = lastPlayerStep + 3 >= p.frames;
    static const uint64_t settleSteps = std::getenv("CB2_MPSETTLE") ? std::strtoull(std::getenv("CB2_MPSETTLE"), nullptr, 10) : 40;   // tests: spawn sooner
    const bool settled = running && playerSteps >= settleSteps;
    if (debug() && p.frames % 30 == 0) {
        std::printf("[mp] frame %llu: global %08x player %08x (isPlayer %d, pose type %u, record ok %d, field5 %08x) level %02X steps %llu running %d | puppets:", (unsigned long long)p.frames,
                    p.peek32(cb2game::current().playerGlobal), player, int(isPlayer(player)), poseType(player), int(recordValid(player)), player ? p.peek32(player + F(kPlayer)) : 0, level,
                    (unsigned long long)playerSteps, int(running));
        for (const auto& [pid, pu] : puppets)
            std::printf(" [%u obj %08x isPuppet %d lastStep %llu pos %d,%d,%d]", pid, pu.obj, int(isPuppet(pu.obj)), (unsigned long long)pu.lastStep,
                        int(p.peek32(pu.obj + F(kX))), int(p.peek32(pu.obj + F(kY))), int(p.peek32(pu.obj + F(kZ))));
        std::printf("\n");
    }
    if (debug() && p.frames % 300 == 0) {
        size_t same = 0;
        for (const auto& o : others()) same += o.level == level;
        std::printf("[mp] frame %llu: level %02X, player %08x updated %llu frames, running %d, others %zu (%zu here), puppets %zu\n", (unsigned long long)p.frames, level, player,
                    (unsigned long long)playerSteps, int(running), others().size(), same, puppets.size());
    }
    std::map<uint8_t, cb2net::PlayerState> here;
    for (const cb2net::PlayerState& s : others())
        if (s.level == level && s.fgrp != 0) here[s.id] = s;

    // While the engine is loading (disc reads in the last half second) nothing is created or removed, for at most five seconds in a row.
    const bool busy = engineBusy();
    if (busy && firstWantFrame == 0) firstWantFrame = p.frames;
    if (!busy) firstWantFrame = 0;
    const bool deferWork = busy && p.frames < firstWantFrame + 300;
    if (deferWork) ++prof.deferred;
    for (auto it = puppets.begin(); it != puppets.end();) {
        const bool gone = here.find(it->first) == here.end();
        const Puppet& pp = it->second;
        // objects made while a cutscene runs can end up with an unusable record for good: replace them
        const bool stale = pp.broken || (isPuppet(pp.obj) && !recordValid(pp.obj) && p.frames > pp.spawnedAtFrame + 120) ||
                           (running && p.frames > std::max<uint64_t>(pp.spawnedAtFrame, pp.lastStep) + 90);   // the game stopped updating it: it is not in the live world
        if ((gone || stale || !isPuppet(pp.obj)) && !(deferWork && isPuppet(pp.obj))) {
            if (debug()) std::printf("[mp] frame %llu: removing puppet %08x (%s)\n", (unsigned long long)p.frames, it->second.obj, gone ? "player left" : stale ? "not updated by the game" : "object vanished");
            destroyPuppet(it->second);
            it = puppets.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [id, s] : here) {
        if (puppets.count(id)) { puppets[id].name = s.name; continue; }
        if (!settled || deferWork || p.frames < retryAt || p.frames < disabledUntil) continue;
        if (puppets.size() >= 8) break;
        Puppet pu;
        pu.obj = spawnPuppet();
        if (!pu.obj) continue;
        if (debug()) std::printf("[mp] frame %llu: puppet %08x for player %d \"%s\" at (%d,%d,%d) fgrp +%x\n", (unsigned long long)p.frames, pu.obj, id, s.name, s.pos[0], s.pos[1], s.pos[2], s.fgrp);
        for (pu.slot = 0;; ++pu.slot) {
            bool used = false;
            for (auto& [oid, other] : puppets) used = used || other.slot == pu.slot;
            if (!used) break;
        }
        pu.name = s.name;
        pu.spawnedAtFrame = uint32_t(p.frames);
        pu.lastStep = p.frames;
        // no script: the object only exists so that the game draws it where and how we say
        p.poke32(pu.obj + F(kMarkerField), kMarker);
        p.poke32(pu.obj + kTransBlock, 0);
        p.poke32(pu.obj + kEventBlock, 0);
        p.poke32(pu.obj + kHeadBlock, 0);
        puppets[id] = pu;
    }
    p.cpu.gteSamples = puppets.empty() ? nullptr : &samples;
}

void Multiplayer::drive(Puppet& pu, const cb2net::PlayerState& s) {
    const uint32_t o = pu.obj;
    // keep the script switched off even if the engine restored a block pointer
    p.poke32(o + kTransBlock, 0);
    p.poke32(o + kEventBlock, 0);
    p.poke32(o + kHeadBlock, 0);

    bool snap = !pu.placed;
    int32_t cur[3];
    for (int i = 0; i < 3; ++i) cur[i] = int32_t(p.peek32(o + F(kX + i)));
    for (int i = 0; i < 3; ++i) {
        if (std::abs(int64_t(s.pos[i]) - cur[i]) > 0x400000) snap = true;   // more than 16384 game units away: teleport
    }
    for (int i = 0; i < 3; ++i)
        p.poke32(o + F(kX + i), uint32_t(snap ? s.pos[i] : cur[i] + (s.pos[i] - cur[i]) / 2));
    // heading: shortest way round (4096 units per turn)
    const int32_t curYaw = int32_t(p.peek32(o + F(kRotY)));
    const int32_t diff = ((s.rot[1] - curYaw + 2048) & 4095) - 2048;
    p.poke32(o + F(kRotX), uint32_t(s.rot[0]));
    p.poke32(o + F(kRotY), uint32_t(snap ? s.rot[1] : ((curYaw + diff / 2) & 0xFFFFFFFF)));
    p.poke32(o + F(kRotZ), uint32_t(s.rot[2]));
    for (int i = 0; i < 3; ++i) p.poke32(o + F(kScaleX + i), uint32_t(s.scale[i] + 16 * (pu.slot + 1)));
    const uint32_t fgrp = p.peek32(o + kEntryPtr) + uint32_t(s.fgrp);
    // A pose whose frame group is not one of the kinds the game draws (garbage from a cutscene, or data this machine has not loaded) is not
    // shown; the puppet keeps its last good pose instead of vanishing.
    const uint32_t head = fgrp < kRamHigh && fgrp >= kRamLow ? p.peek32(fgrp & ~3u) >> ((fgrp & 3u) * 8) & 0xFF : 0;
    bool goodPose = head == 1 || head == 2 || head == 4 || head == 5;
    // A pose the game could not draw last time (its animation data is not loaded on this machine yet) is replaced by the last one that
    // worked, or failing that by whatever our own Crash is showing, which is always loaded; it is tried again now and then.
    if (goodPose && fgrp == pu.badPose && p.frames < pu.badUntil) goodPose = false;
    if (goodPose) pu.pose = fgrp;
    uint32_t shown = goodPose ? fgrp : (pu.lastDrawable ? pu.lastDrawable : (player ? p.peek32(player + F(kFrameGroup)) : 0));
    if (!goodPose && pu.lastDrawable == 0 && player) pu.lastDrawable = shown;
    p.poke32(o + F(kFrameGroup), shown);
    // the frame index must stay inside the group (the sender's value may already have advanced past the end)
    int32_t fnum = s.fnum;
    if (head == 1) {
        const int32_t count = int32_t(p.peek32(fgrp & ~3u) >> 16 & 0xFFFF);
        if (count > 0 && (fnum >> 8) >= count) fnum = ((count - 1) << 8) | (fnum & 0xFF);
    }
    p.poke32(o + F(kFrameNum), goodPose ? uint32_t(fnum) : (shown == pu.lastDrawable ? uint32_t(pu.lastDrawableFnum) : uint32_t(fnum)));
    pu.placed = true;
}

}  // namespace cb2mp
