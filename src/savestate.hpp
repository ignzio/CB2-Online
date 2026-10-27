#pragma once
// Save states: the whole emulated console (CPU, RAM, GPU/VRAM, SPU, CD-ROM, timers, DMA, BIOS state) in one file, so that a test or a
// practice run can start from a moment deep in the game instead of waiting for the intro and the cutscenes to play.
// Only the interpreter (not the translated engine) can be saved, and only between two calls of Platform::run. The memory card image is
// deliberately not part of a state: loading a state must never roll back the player's real saves.
#include <cstdint>
#include <string>
#include <vector>

#include "psx.hpp"

namespace psx {

// `extra` is opaque data the caller wants to keep with the state (the online module stores its level number there).
bool saveState(Platform& p, const std::string& path, const std::vector<uint8_t>& extra, std::string& error);
bool loadState(Platform& p, const std::string& path, std::vector<uint8_t>& extra, std::string& error);

// The same state kept in memory (the freeze watchdog takes one every few seconds and goes back to it when the game hangs).
bool saveStateToMemory(Platform& p, const std::vector<uint8_t>& extra, std::vector<uint8_t>& out, std::string& error);
bool loadStateFromMemory(Platform& p, const std::vector<uint8_t>& state, std::vector<uint8_t>& extra, std::string& error);

// A hash over everything a state contains. Two consoles with the same digest are in the same state (used by the determinism test).
uint64_t stateDigest(Platform& p);

}  // namespace psx
