#include "game_profile.hpp"

#include <filesystem>

namespace cb2game {

const Profile* forFolder(const std::string& folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const bool hasNtsc = fs::exists(fs::path(folder) / ntsc().exeName, ec), hasPal = fs::exists(fs::path(folder) / pal().exeName, ec);
    if (hasNtsc && !hasPal) return &ntsc();
    if (hasPal && !hasNtsc) return &pal();
    return nullptr;   // neither, or an ambiguous folder with both (the caller decides)
}

}  // namespace cb2game
