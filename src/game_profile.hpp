#pragma once
// The two releases of Crash Bandicoot 2 that this port runs: the North American one (NTSC-U, SCUS-94154, 60 Hz) and the European one (PAL, SCES-00967, 50 Hz).
// They are the same game built twice: the level files have the same names and layout, but the executable was compiled again (the game logic got extra code for 50 Hz, so
// every routine and every global moved), so the few places where this port reaches into the running game - the object loop, the level variable, the camera, ... -
// have one address per release. They were found with tools/pal_map.py (a signature match of the code around the NTSC address) and are listed here.
// `current()` is the release that is running; the platform sets it when the executable is loaded.
#include <cstdint>
#include <string>

namespace cb2game {

enum class Region : uint8_t { Ntsc = 0, Pal = 1 };

struct Profile {
    Region region;
    const char* id;           // "ntsc" / "pal": cb2.cfg, the lobby protocol and file names
    const char* title;        // for the player
    const char* exeName;      // the game's executable on the disc
    const char* folderName;   // where the importer puts the files, next to the program
    uint32_t entryPc;         // the entry point in the executable's header (tells the releases apart)
    uint64_t frameCycles;     // CPU cycles (33.8688 MHz) per vertical blank
    double frameRate;         // vertical blanks per second the game is meant to run at
    // engine code
    uint32_t objectStep, objectStepReturn, afterAnimate, runScript, createObject, destroyObject, openLevel, nextLevel;
    uint32_t allocFail;              // where the engine's page allocator prints "!mem" (no free slot for a page of level data): the freeze watchdog counts it
    uint32_t sceneryProjection[3];   // the GTE routines that project the level's scenery (native world)
    uint32_t vsyncWait, vsyncSpin;   // PsyQ VSync(): the routine and its polling loop
    uint32_t cardStub[3];            // PsyQ memory card routines that are answered by the platform
    // globals
    uint32_t playerGlobal, puppetParent, currentLevel, cameraPos, vsyncCounter, exeSizeWord;
    uint32_t pageTable;              // the engine's table of page slots (count, then 0x2C bytes per slot; the byte at +0xF of a slot is how many objects hold its page)
};

inline const Profile& ntsc() {
    static const Profile p = {Region::Ntsc, "ntsc", "NTSC-U (USA)", "SCUS_941.54", "data", 0x80049B2C, 564480, 60000.0 / 1001.0,
                              0x8001C718, 0x80018AD4, 0x8001DDC0, 0x8003A014, 0x80019800, 0x80019E08, 0x80012170, 0x80037DC8, 0x8001384C,
                              {0x80042420, 0x800426A0, 0x8004274C},
                              0x8004A5CC, 0x8004A5F4,
                              {0x8004A2C4, 0x8004A2D8, 0x8004A364},
                              0x8006CB10, 0x8006CB20, 0x8005F3EC, 0x80060768, 0x8005DC98, 0x8005F188, 0x800675B0};
    return p;
}

inline const Profile& pal() {
    static const Profile p = {Region::Pal, "pal", "PAL (Europe)", "SCES_009.67", "data-pal", 0x80049DC4, 677376, 50.0,
                              0x8001C850, 0x80018BF8, 0x8001E008, 0x8003A2AC, 0x80019928, 0x80019F40, 0x80012170, 0x80038060, 0x80013950,
                              {0x800426B8, 0x80042938, 0x800429E4},
                              0x8004A864, 0x8004A88C,
                              {0x8004A55C, 0x8004A570, 0x8004A5FC},
                              0x8006CDA8, 0x8006CDB8, 0x8005F684, 0x80060A00, 0x8005DF30, 0x8005F420, 0x80067848};
    return p;
}

inline const Profile& of(Region r) { return r == Region::Pal ? pal() : ntsc(); }

// Level numbers that mean the same place. The European release has one warp room per language (levels 0x2D-0x30 are copies of the warp room, 0x02, with the signs in
// French, German, Spanish and Italian), so two players of that release who chose different languages are in the same room with different level numbers.
// Everything that compares the levels of two players (who sees whom, the player list) goes through this.
inline uint32_t canonicalLevel(Region r, uint32_t id) {
    id &= 0xFF;
    return r == Region::Pal && id >= 0x2D && id <= 0x30 ? 0x02u : id;
}

// The profile of an executable by its entry point; nullptr for anything else.
inline const Profile* forEntry(uint32_t entryPc) {
    if (entryPc == ntsc().entryPc) return &ntsc();
    if (entryPc == pal().entryPc) return &pal();
    return nullptr;
}

// The release of a game folder: which executable is in it. nullptr when neither is.
const Profile* forFolder(const std::string& folder);

// The release that is running (NTSC until an executable has been loaded).
inline const Profile*& currentSlot() { static const Profile* p = &ntsc(); return p; }
inline const Profile& current() { return *currentSlot(); }

}  // namespace cb2game
