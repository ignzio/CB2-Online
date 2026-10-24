#pragma once
// Overlay drawn over the game: short notices (somebody joined, entered a level ...) and the list of players at the right edge with where
// each of them is and for how long. Drawn with the lobby's look (ui.hpp) on the same 640 x 480 virtual screen as the menu.
#include <string>
#include <vector>

#include "ui.hpp"

namespace cb2hud {

struct Row {
    std::string name;
    uint32_t rgb = 0xFFFFFF;   // the colour of the player's name tag
    std::string where;         // level name, or "MENU"
    std::string time;          // time in the level, or the last level's time ("LAST ..."), or empty
    std::string best;          // personal best for the level they are in, or empty
    std::string finished;      // "TURTLE WOODS 1:23.4" for their latest finished run, or empty
    bool self = false;
    bool inLevel = false;
};

class Hud {
public:
    // A notice in the corner for a few seconds; the newest three are kept.
    void notice(const std::string& text, cb2ui::Rgb colour, double now);
    void clearNotices() { notices.clear(); }
    void setTimer(const std::string& text) { timerText = text; }   // the level timer in the top right corner; empty hides it
    // `rows` are the players to list (yourself first). `panel` shows the list; the notices are always shown.
    void draw(cb2ui::Canvas& canvas, const std::vector<Row>& rows, bool panel, double now);

private:
    struct Notice { std::string text; cb2ui::Rgb colour; double until = 0; };
    std::vector<Notice> notices;
    std::string timerText;
};

}  // namespace cb2hud
