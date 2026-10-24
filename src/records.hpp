#pragma once
// Personal records, kept in a text file next to the game (records.txt) so that they survive updates and can be shared or uploaded later.
// One line per level, tab separated:   R <level hex> <best ms> <finished runs> <attempts> <last ms> <date of the best>
// The format is deliberately plain: a future leaderboard service only needs these lines (plus the player's name).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cb2run {

struct Best {
    uint32_t bestMs = 0;       // 0 = never finished
    uint32_t finishes = 0;     // runs that reached the end of the level
    uint32_t attempts = 0;     // runs that ended (finished or not)
    uint32_t lastMs = 0;       // the time of the most recent finished run
    std::string bestDate;      // YYYY-MM-DD
};

class Records {
public:
    explicit Records(std::string file = {}) : path(std::move(file)) {}
    bool load();                              // false when there is no file yet (that is fine)
    bool save() const;                        // written to a temporary name first, so a crash never leaves half a file
    // Adds a finished or failed run; true when it is a new personal best.
    bool add(int level, uint32_t ms, bool finished, const std::string& date);
    const Best* find(int level) const;
    const std::map<int, Best>& all() const { return levels; }
    const std::string& file() const { return path; }

private:
    std::string path;
    std::map<int, Best> levels;
};

std::string todayText();   // YYYY-MM-DD in local time

}  // namespace cb2run
