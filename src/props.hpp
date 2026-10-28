#pragma once
// Props: things that are not in the game but live in it. A prop is an object of the game's own data (the "?" crate of Turtle Woods, with its real model, size and textures) placed where the
// game does not have it, for example in the warp room. This module
//   - draws it into the game's picture, among the polygons the game itself draws: the model's polygons are projected with the camera the game used for the level and put into the frame's polygon
//     list at the place their depth belongs to (the console's painter's algorithm), so Crash can walk in front of it and stand on it;
//   - lets the player's Crash interact with it by reading and writing the fields of the game's own player object (position and velocity): it is a wall at the sides and a spring on top.
// Nothing is sent over the network: every player has the same props in the same places, and each player's bounce is seen by the others as the Crash moving.
// It hooks the game's object update (see multiplayer.hpp), so it needs the interpreter, and it adds to the host renderer's command list (Gpu::cmds), so it needs the hi-res renderer.
// Units: the game keeps an object's position as 1/256 of a world unit; the camera position, the levels and everything here use world units, y up.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game_profile.hpp"
#include "propasset.hpp"
#include "psx.hpp"

namespace cb2prop {

struct Box {
    float origin[3] = {0, 0, 0};     // where the model's origin is, world units
    float scale = 1.0f;              // 1 = the size the game's model has
    float centre[3] = {0, 0, 0};     // the solid: middle and half sizes of the model's bounds
    float half[3] = {0, 0, 0};
    int asset = 0;                   // which of Props::assets (the crate it is made of)
    int32_t bounce = 1200000;        // upward speed the box gives, in the game's own velocity unit (a normal jump starts at 936025)
    float squash = 0;                // 0..1: how squashed the prop is right now (it springs back)
    float lastFeetY = 0;             // the player's feet at the previous update (to see them cross the top)
    bool haveLast = false;
};

class Props {
public:
    explicit Props(psx::Platform& platform);
    void install();                                   // hooks the game; call once, with the interpreter
    // Reads the props of the player's own file (props.txt next to the game; see docs/PROPS.md). Without the file, or when it holds no valid line, the built-in one stays. Returns how many were read.
    int loadFile(const std::string& path);
    bool enabled = false;                             // the menu's switch (Experimental > Props)
    void stateLoaded() { player = 0; boxes.clear(); boxesLevel = 0xFFFFFFFFu; lastProbeCount = ~0ull; crateLevel = 0xFFFFFFFFu; }
    const std::vector<Box>& current() const { return boxes; }
    struct Placement { uint32_t level; float x, z, floorY, scale; int32_t bounce; std::string model; };
    struct Camera {
        bool valid = false;
        float rot[9] = {};            // the GTE's rotation matrix, 1.0 = 4096, row-major
        float pos[3] = {};            // world units
        float h = 0, ofx = 0, ofy = 0;
    };
    const Camera& camera() const { return cam; }
    // A crate of the game's own crate script placed in a level (world units, y = the floor under it; sub = the script's subtype, which says what kind of crate it is). It exists as a real game
    // object only while Crash is near it (see streamCrates), so there can be as many as you like.
    struct Crate { uint32_t level; float x, y, z; int sub; uint32_t obj = 0; bool gone = false; float madeX = 0, madeY = 0, madeZ = 0; };
    int liveCrates() const { return liveCount; }               // crates that are real game objects right now
    size_t crateCount() const { return crates.size(); }        // crates placed, near or not
    uint64_t bounces = 0, drawnFrames = 0;            // how many times the prop sent Crash up, and how many pictures it was drawn into

private:
    psx::Platform& p;
    std::vector<Placement> custom;                    // from props.txt
    std::vector<Asset> assets;                        // the crates in use (at most four: they share one texture picture of the renderer)
    int assetFor(const std::string& model);           // reads the model out of the Turtle Woods level file, once; -1 when it cannot be had
    std::vector<uint8_t> levelFile;
    bool levelFileTried = false;
    std::vector<Box> boxes;
    uint32_t boxesLevel = 0xFFFFFFFFu;
    uint32_t player = 0;
    uint64_t lastProbeCount = ~0ull;
    Camera cam;
    r3000::GteSample probe;                           // the last scenery vertex the GTE projected (Cpu::probe): its matrix and offsets are the camera of the frame
    uint32_t stepping = 0;
    // The crates: placed in a list, made real (with the engine's own object-creation routine, as an object of the game's crate script Boxs.C) when Crash comes near and thrown away again when he
    // is far, untouched. A real crate does everything the game's crate does, because it is the game's crate; one that was broken stays broken until the level is entered again.
    std::vector<Crate> crates;
    uint32_t crateLevel = 0xFFFFFFFFu, crateParent = 0, lastPlayer = 0;
    bool crateAdopted = false, building = false;
    int liveCount = 0;
    uint64_t crateCheckedFrame = ~0ull, crateQuietSince = 0, crateLastSectors = ~0ull;
    std::vector<uint8_t> undoRam, undoScratch;
    void streamCrates();
    void adoptCrates(uint32_t level);
    bool isCrateObject(uint32_t a);
    bool transact(const char* what, const std::function<void()>& f);   // calls into the game; when it fails the engine's memory is put back
    static uint32_t F(int n) { return uint32_t(0x40 + 4 * n); }
    bool isObject(uint32_t a);
    uint32_t findPlayer();
    uint32_t levelNow();
    void loadLevel(uint32_t level);
    void onStepEnter(uint32_t obj);
    void onStepReturn();
    void onDisplayList();
    void interact(uint32_t obj);
    bool readCamera();
    bool project(const float world[3], float out[3]) const;   // screen x, y (before the draw offset) and the depth; false behind the camera
    void drawBox(const Box& b, std::vector<psx::GpuCmd>& out, float& key, int offX, int offY, const psx::GpuCmd& tmpl) const;
};

}  // namespace cb2prop
