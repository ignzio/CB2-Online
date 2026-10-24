#include "records.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace cb2run {

std::string todayText() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return b;
}

bool Records::load() {
    levels.clear();
    if (path.empty()) return false;
    std::ifstream in(std::filesystem::u8path(path));
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '\t')) f.push_back(item);
        if (f.size() < 6 || f[0] != "R") continue;
        const long level = std::strtol(f[1].c_str(), nullptr, 16);
        if (level < 0 || level > 0xFE) continue;
        Best b;
        b.bestMs = uint32_t(std::strtoul(f[2].c_str(), nullptr, 10));
        b.finishes = uint32_t(std::strtoul(f[3].c_str(), nullptr, 10));
        b.attempts = uint32_t(std::strtoul(f[4].c_str(), nullptr, 10));
        b.lastMs = uint32_t(std::strtoul(f[5].c_str(), nullptr, 10));
        if (f.size() > 6) b.bestDate = f[6].substr(0, 10);
        levels[int(level)] = b;
    }
    return true;
}

bool Records::save() const {
    if (path.empty()) return false;
    namespace fs = std::filesystem;
    const fs::path target = fs::u8path(path);
    fs::path tmp = target;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "# CB2-Online records. level (hex), best ms, finished runs, attempts, last ms, date of the best. Edit at your own risk.\n";
        for (const auto& [level, b] : levels) {
            char head[16];
            std::snprintf(head, sizeof head, "%02X", level);
            out << "R\t" << head << '\t' << b.bestMs << '\t' << b.finishes << '\t' << b.attempts << '\t' << b.lastMs << '\t' << b.bestDate << '\n';
        }
        if (!out) return false;
    }
    std::error_code ec;
    fs::remove(target, ec);
    fs::rename(tmp, target, ec);
    return !ec;
}

bool Records::add(int level, uint32_t ms, bool finished, const std::string& date) {
    Best& b = levels[level];
    ++b.attempts;
    if (!finished) return false;
    ++b.finishes;
    b.lastMs = ms;
    if (b.bestMs == 0 || ms < b.bestMs) {
        b.bestMs = ms;
        b.bestDate = date;
        return true;
    }
    return false;
}

const Best* Records::find(int level) const {
    const auto it = levels.find(level);
    return it == levels.end() ? nullptr : &it->second;
}

}  // namespace cb2run
