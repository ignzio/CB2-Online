#pragma once
// First-run setup: the player points the game at their own Crash Bandicoot 2 disc image (.cue/.bin, .iso or .img) and the files the
// game needs are copied out of it into a folder (<exe folder>\data). Nothing is downloaded and nothing of the game ships with this
// program. Reads ISO9660 from raw 2352-byte (mode 1 or 2), 2336-byte or plain 2048-byte sector images.
#include <cstdint>
#include <functional>
#include <string>

namespace cb2import {

struct Result {
    bool ok = false;
    std::string error;   // when !ok: a sentence for the player
    int files = 0;
    uint64_t bytes = 0;
};

// Called while copying with the bytes done so far and the total.
using Progress = std::function<void(uint64_t done, uint64_t total)>;

// True for the file extensions importDisc understands (.cue .bin .iso .img), case-insensitive.
bool looksLikeImage(const std::string& path);

// Copies SCUS_941.54, SYSTEM.CNF and everything under S0..S3 from the image into `outDir` (created if needed). A .cue file is followed
// to its first data file. Existing files in outDir are replaced; files are written under a temporary name first, so a failed import
// never leaves half a file behind under its real name.
Result importDisc(const std::string& imagePath, const std::string& outDir, const Progress& progress = {});

// Windows: the "open file" dialog for the disc image, starting in `startDir`. Empty when cancelled (or on other systems).
std::string pickImage(const std::string& startDir);

// Windows message boxes for the steps around the import (no console in the packaged build). ask: OK / Cancel, true for OK.
bool ask(const std::string& text);
void inform(const std::string& text);

}  // namespace cb2import
