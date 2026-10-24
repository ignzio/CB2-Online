#pragma once
// Other players inside the original game. Each machine runs its own copy of the game on the emulated PlayStation; this module
//   - reads Crash's state out of the game every frame (the game's player object: position, rotation, animation),
//   - sends it to the other players (net.hpp) and receives theirs,
//   - creates one extra Crash object per remote player with the game's own object-creation routine, switches off that object's script and
//     drives its position, rotation and animation from the network. The game itself draws it: same model, lighting, shadows and
//     animation as the real Crash, in the same world. A name tag floats over each of them.
// It hooks the game's per-object update routine (see Platform::addHook), so it needs the interpreter (not the translated engine).
// Offsets of the NTSC-U executable (SCUS_941.54): see docs/ONLINE.md.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "net.hpp"
#include "psx.hpp"

namespace cb2run { class Tracker; }

namespace cb2mp {

struct Options {
    std::string name;        // shown over your Crash; empty = Windows user name
    bool online = false;     // join the room (otherwise only the test ghost can run)
    std::string room = "PUBLIC";   // players who type the same room meet
    std::string rendezvous;  // "host[:port]" of the rendezvous server (cb2_server); empty = this PC / LAN only
    std::string directory;   // URL of the lobby service (docs/LOBBY_PROTOCOL.md): lists this room for others and carries the join hand-shake
    std::vector<std::string> seeds;   // "ip:port" of players of this room taken from the directory's list
    bool joinViaDirectory = false;
    // Test mode without a network: a ghost player copies your Crash a few steps to the side (fake = 1) or, with fake = 2, also
    // comes and goes every few seconds to exercise creating and removing the extra Crash.
    int fake = 0;
};

class Multiplayer {
public:
    explicit Multiplayer(psx::Platform& platform) : p(platform) {}
    bool start(const Options& options, std::string& error);   // opens the socket; call before install()
    void install();                                            // hooks the game
    bool active() const { return net.active() || opt.fake; }
    void stop() { net.close(); }                               // leave the session (the lobby's Back)
    const cb2net::Session& session() const { return net; }
    const Options& options() const { return opt; }
    std::string status() const;                               // for the window title
    void tick();                                               // networking; call every vertical blank
    std::vector<cb2net::PlayerState> others();                 // remote players (or the test ghost)
    int puppetCount() const { return int(puppets.size()); }
    uint32_t levelId() const { return level; }
    void setTracker(const cb2run::Tracker* t) { runs = t; }   // the speedrun data that is sent along with the player's state
    uint8_t runCounter = 0;                                   // goes up when a run ends
    // The console was rewound by a loaded save state: forget everything tracked about the game's objects (they are all different now).
    void stateLoaded(uint32_t levelNow);
    const std::string& selfName() const { return playerName; }
    // Where the time goes (CB2_FRAMELOG): milliseconds spent in manage() / buildTags() and how many puppets were created / removed since the start.
    struct Profile { uint64_t failures = 0, deferred = 0; double manageMs = 0, tagsMs = 0; uint64_t spawns = 0, kills = 0, tickCalls = 0; double tickMs = 0; } prof;
    int opacity = 100;                                         // how solid the other players' Crashes are drawn (percent, 10..100)
    double levelSeconds() const { return double(p.frames - levelStartFrame) / 59.94; }   // game time in the current level (stands still while paused)
    bool showTags = true;                                      // the pause menu's "Name tags"

    // Addresses in the executable
    static constexpr uint32_t kObjectStep = 0x8001C718;    // runs one object: a0 = object
    static constexpr uint32_t kObjectStepReturn = 0x80018AD4;   // where the object loop continues afterwards
    static constexpr uint32_t kAfterAnimate = 0x8001DDC0;    // in the object update: the frame's model has just been looked up (a0 = s1 = object)
    static constexpr uint32_t kPlayerGlobal = 0x8006CB10;    // pointer to the current player object
    static constexpr uint32_t kRunScript = 0x8003A014;       // script interpreter entry (a0 = object)
    static constexpr uint32_t kCreateObject = 0x80019800;  // (parent, type, subtype, argc, [stack] args, flag) -> object
    static constexpr uint32_t kDestroyObject = 0x80019E08; // (object, 0)
    static constexpr uint32_t kOpenLevel = 0x80012170;     // (0 = .NSD, level id)
    static constexpr uint32_t kPuppetParent = 0x8006CB20;  // list head the HUD objects hang from: always walked by the object loop
    static constexpr uint32_t kPuppetSubtype = 1;          // Crash's script with subtype 1 gives a second Crash object

private:
    struct Puppet {
        uint32_t obj = 0;
        std::string name;
        bool placed = false;
        uint32_t spawnedAtFrame = 0;
        uint32_t pose = 0;                   // the pose last asked for
        uint32_t lastDrawable = 0;           // the last pose the game actually drew for it
        int32_t lastDrawableFnum = 0;
        uint32_t badPose = 0;                // a pose the game could not draw ...
        uint64_t badUntil = 0;               // ... not offered again before this frame
        uint64_t lastStep = 0;               // frame in which the game last updated this object
        uint32_t badRecordSteps = 0;
        bool broken = false;     // its update crashed the guest once: it is replaced
        int slot = 0;            // 0..7; each puppet is drawn a hair bigger than Crash so that its polygons can be told apart on screen
        double column[3] = {0, 0, 0};   // lengths of the columns of its model matrix, measured when the game updates it
        bool haveColumns = false;
    };
    psx::Platform& p;
    const cb2run::Tracker* runs = nullptr;
    cb2net::Session net;
    Options opt;
    std::string playerName;
    uint32_t player = 0, playerSeen = 0;
    uint64_t lastPlayerStep = 0, playerSteps = 0;   // last frame in which the game updated the player object, and how many frames since it appeared
    uint64_t playerSince = 0;              // frame the current player object was first seen
    uint32_t level = 0xFF;                 // level the game has loaded (0xFF = unknown)
    uint64_t managedFrame = ~0ull, lastScan = 0;
    // Creating and removing objects calls into the engine's allocator. That must not happen while the engine is loading (its tables are being rebuilt),
    // and a call that fails must never end the game: it is dropped and tried again later.
    uint64_t lastCdSectors = 0, lastDiscFrame = 0, firstWantFrame = 0, retryAt = 0, disabledUntil = 0;
    int spawnFailures = 0;
    std::vector<uint8_t> undoRam, undoScratch;   // memory as it was before a call into the game, to put back if the call fails
    bool engineBusy();                      // the disc was read in the last half second
    template <class Fn> bool guarded(const char* what, Fn&& fn);   // runs fn; a thrown guest fault is logged and swallowed (false)
    uint64_t levelStartFrame = 0;          // frame in which the current level was opened
    uint32_t stepping = 0;                 // object being updated right now
    Puppet* steppingPuppet = nullptr;
    std::vector<r3000::GteSample> samples;   // every vertex the game projects while puppets exist (to find where they are on screen)
    std::map<uint8_t, Puppet> puppets;     // by remote player id
    std::vector<uint32_t> zombies;         // objects we could not remove yet: they are left alone until the engine lets us
    cb2net::PlayerState localState;
    bool haveLocal = false;

    static uint32_t F(int n) { return uint32_t(0x40 + 4 * n); }
    bool isObject(uint32_t a);
    bool isPlayer(uint32_t a);
    bool recordValid(uint32_t obj);   // the record at +8 is a real address
    uint32_t poseType(uint32_t obj);
    bool recordSound(uint32_t obj);   // the per-object record the drawing code follows is usable
    bool isOurs(uint32_t a);     // carries our marker and hangs from our list, whether or not it is still tracked
    bool isPuppet(uint32_t a);   // an object this module created and the engine has not freed or reused since
    uint32_t findPlayer();
    void onLevel(uint32_t id);
    void onStepEnter(uint32_t obj);
    void onStepReturn();
    void buildTags();     // once the frame's polygons are complete: find each puppet's polygons, queue its name tag
    void manage();
    uint32_t spawnPuppet();
    void destroyPuppet(Puppet& pu);
    void killObject(uint32_t obj);
    void drive(Puppet& pu, const cb2net::PlayerState& s);
    void publish();
    void applyRuns(cb2net::PlayerState& s) const;
    void announce(uint32_t lvl);
    double now() const;
};

}  // namespace cb2mp
