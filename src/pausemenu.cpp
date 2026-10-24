#include "pausemenu.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>

namespace cb2menu {

using namespace cb2ui;

namespace {

const char* filterName(int f) { return f == 0 ? "NEAREST" : f == 1 ? "SMOOTH" : "SHARP"; }

}  // namespace

bool PauseMenu::init(Settings* settings, const Context& context) {
    s = settings;
    ctx = context;
    return canvas.init();
}

void PauseMenu::open() {
    page = Page::Main;
    sel = 0;
    message.clear();
}

std::vector<PauseMenu::Item> PauseMenu::items() const {
    std::vector<Item> v = {kResume, kControls, kVolume, kFilter, kFullscreen};
    if (ctx.online) v.push_back(kTags);
    v.push_back(kMainMenu);
    v.push_back(kQuit);
    return v;
}

void PauseMenu::adjust(Item item, int d) {
    switch (item) {
    case kVolume: {
        const int v = std::max(0, std::min(100, s->volume + 5 * d));
        if (v != s->volume) { s->volume = v; changed = true; }
        break;
    }
    case kFilter:
        s->filter = (s->filter + 3 + d) % 3;
        changed = true;
        break;
    case kFullscreen:
        if (ctx.toggleFullscreen) ctx.toggleFullscreen();
        break;
    case kTags:
        s->nameTags = !s->nameTags;
        changed = true;
        break;
    default: break;
    }
}

void PauseMenu::activate(Item item) {
    switch (item) {
    case kResume: close(); break;
    case kControls: page = Page::Controls; controlsSel = 0; message.clear(); break;
    case kFilter: case kFullscreen: case kTags: adjust(item, 1); break;
    case kMainMenu: page = Page::ConfirmMenu; confirmSel = 1; break;
    case kQuit: page = Page::ConfirmQuit; confirmSel = 1; break;
    default: break;
    }
}

void PauseMenu::key(int k, int action) {
    if (page == Page::Closed || (action != GLFW_PRESS && action != GLFW_REPEAT)) return;
    const bool press = action == GLFW_PRESS;
    const int up = k == GLFW_KEY_UP ? -1 : k == GLFW_KEY_DOWN ? 1 : 0;
    const int side = k == GLFW_KEY_LEFT ? -1 : k == GLFW_KEY_RIGHT ? 1 : 0;
    const bool enter = press && (k == GLFW_KEY_ENTER || k == GLFW_KEY_KP_ENTER);
    const bool esc = press && k == GLFW_KEY_ESCAPE;

    if (page == Page::Capture) {
        if (!press) return;
        if (esc) { page = Page::Controls; return; }
        if (k == GLFW_KEY_DELETE || k == GLFW_KEY_BACKSPACE) { s->keys.key[controlsSel] = 0; changed = true; page = Page::Controls; return; }
        if (cb2input::reservedKey(k)) { message = "THAT KEY IS RESERVED"; messageUntil = now + 2.5; return; }
        s->keys.assign(controlsSel, k);
        changed = true;
        message.clear();
        page = Page::Controls;
        return;
    }
    if (page == Page::ConfirmMenu || page == Page::ConfirmQuit) {
        if (up || side) { if (press) confirmSel = 1 - confirmSel; }
        else if (esc) page = Page::Main;
        else if (enter) {
            if (confirmSel == 0) { result = page == Page::ConfirmMenu ? Result::MainMenu : Result::Quit; close(); }
            else page = Page::Main;
        }
        return;
    }
    if (page == Page::Controls) {
        const int rows = cb2input::kBindingCount + 2;   // bindings, reset, back
        if (up) move(controlsSel, rows, up);
        else if (esc) { page = Page::Main; message.clear(); }
        else if (enter) {
            if (controlsSel < cb2input::kBindingCount) { page = Page::Capture; message.clear(); }
            else if (controlsSel == cb2input::kBindingCount) { s->keys.reset(); changed = true; }
            else { page = Page::Main; message.clear(); }
        }
        return;
    }
    // main page
    const auto list = items();
    sel = std::min(sel, int(list.size()) - 1);
    if (up) move(sel, int(list.size()), up);
    else if (side) adjust(list[size_t(sel)], side);
    else if (esc) close();
    else if (enter) activate(list[size_t(sel)]);
}

bool PauseMenu::script(const std::string& tok) {
    struct Map { const char* name; int key; };
    static const Map map[] = {{"up", GLFW_KEY_UP}, {"down", GLFW_KEY_DOWN}, {"left", GLFW_KEY_LEFT}, {"right", GLFW_KEY_RIGHT},
                              {"enter", GLFW_KEY_ENTER}, {"esc", GLFW_KEY_ESCAPE}, {"del", GLFW_KEY_DELETE}};
    for (const Map& m : map) if (tok == m.name) { key(m.key, GLFW_PRESS); return true; }
    if (tok.rfind("key:", 0) == 0) { const int k = cb2input::keyFromName(tok.substr(4)); if (k) { key(k, GLFW_PRESS); return true; } }
    return false;
}

void PauseMenu::draw(double t) {
    if (page == Page::Closed) return;
    now = t;
    if (!message.empty() && now > messageUntil) message.clear();
    const bool blink = int(t * 2.0) % 2 == 0;
    canvas.rect(0, 0, kW, kH, rgb(0, 6, 12), 0.62f);
    canvas.rect(0, 22, kW, 4, rgb(150, 92, 38), 0.9f);
    canvas.rect(0, kH - 26, kW, 4, rgb(150, 92, 38), 0.9f);
    canvas.centred(page == Page::Controls || page == Page::Capture ? "CONTROLS" : "PAUSED", 38, 54, kGold1, kGold2);

    auto row = [&](float y, bool selected, const std::string& label, const std::string& value, Rgb valueColour) {
        plank(canvas, 130, y, 380, 36, selected);
        canvas.text(label, 146, y + 5, 22, selected ? kGold1 : kWhite1, selected ? kGold2 : kWhite2);
        if (!value.empty()) canvas.text(value, 496 - canvas.width(value, 20), y + 7, 20, selected ? rgb(255, 246, 210) : valueColour);
    };
    auto centredRow = [&](float y, bool selected, const std::string& label) {
        plank(canvas, 130, y, 380, 36, selected);
        canvas.text(label, 320 - canvas.width(label, 24) * 0.5f, y + 4, 24, selected ? kGold1 : kWhite1, selected ? kGold2 : kWhite2);
    };

    std::string hint;
    if (page == Page::Main) {
        const auto list = items();
        sel = std::min(sel, int(list.size()) - 1);
        float y = 112;
        for (size_t i = 0; i < list.size(); ++i, y += 40) {
            const bool on = int(i) == sel;
            switch (list[i]) {
            case kResume: centredRow(y, on, "RESUME"); break;
            case kControls: centredRow(y, on, "CONTROLS"); break;
            case kVolume: {
                row(y, on, "VOLUME", std::to_string(s->volume), kTeal);
                // the level as a bar between the label and the number
                canvas.rect(262, y + 12, 144, 12, rgb(0, 0, 0), 0.6f);
                canvas.rect(263, y + 13, 142.0f * float(s->volume) / 100.0f, 10, on ? rgb(255, 232, 110) : rgb(128, 232, 255), on ? rgb(255, 140, 24) : rgb(60, 160, 200));
                break;
            }
            case kFilter: row(y, on, "PICTURE", filterName(s->filter), kTeal); break;
            case kFullscreen: row(y, on, "FULLSCREEN", ctx.isFullscreen && ctx.isFullscreen() ? "ON" : "OFF", kTeal); break;
            case kTags: row(y, on, "NAME TAGS", s->nameTags ? "ON" : "OFF", kTeal); break;
            case kMainMenu: centredRow(y, on, "MAIN MENU"); break;
            case kQuit: centredRow(y, on, "QUIT GAME"); break;
            }
        }
        if (ctx.online) canvas.centred("ONLINE: THE OTHERS SEE YOU STAND STILL WHILE THIS IS OPEN", 432, 15, kDim);
        hint = "UP DOWN  SELECT      LEFT RIGHT  CHANGE      ENTER  CHOOSE      ESC  RESUME";
    } else if (page == Page::Controls || page == Page::Capture) {
        const int total = cb2input::kBindingCount + 2;
        const int visible = 8;
        const int top = std::max(0, std::min(controlsSel - visible / 2, total - visible));
        float y = 100;
        for (int i = top; i < std::min(total, top + visible); ++i, y += 40) {
            const bool on = i == controlsSel;
            if (i < cb2input::kBindingCount) {
                const bool waiting = page == Page::Capture && on;
                const std::string value = waiting ? (blink ? "PRESS A KEY" : "") : cb2input::keyName(s->keys.key[i]);
                row(y, on, cb2input::kBindings[i].label, value, s->keys.key[i] == 0 ? kDim : kWhite1);
                if (waiting) canvas.text(value, 496 - canvas.width("PRESS A KEY", 20), y + 7, 20, kGreen);
            } else if (i == cb2input::kBindingCount) {
                centredRow(y, on, "RESET TO DEFAULTS");
            } else {
                centredRow(y, on, "BACK");
            }
        }
        if (top > 0) canvas.triangle(320, 92, 308, 104, 332, 104, kGold1);
        if (top + visible < total) canvas.triangle(320, y + 4, 308, y - 8, 332, y - 8, kGold1);
        if (!message.empty()) canvas.centred(message, 416, 20, kRed);
        else canvas.centred("A GAME PAD WORKS ALONGSIDE THE KEYBOARD", 424, 15, kDim);
        hint = page == Page::Capture ? "PRESS THE NEW KEY      DELETE  NO KEY      ESC  CANCEL" : "UP DOWN  SELECT      ENTER  REBIND      ESC  BACK";
    } else {
        const bool toMenu = page == Page::ConfirmMenu;
        canvas.centred(toMenu ? "BACK TO THE MAIN MENU?" : "QUIT THE GAME?", 130, 30, kGold1, kGold2);
        canvas.centred("PROGRESS YOU HAVE NOT SAVED ON THE MEMORY CARD IS LOST", 176, 16, kRed);
        centredRow(230, confirmSel == 0, "YES");
        centredRow(276, confirmSel == 1, "NO");
        hint = "UP DOWN  SELECT      ENTER  CHOOSE      ESC  BACK";
    }
    canvas.centred(hint, 446, 16, kDim);
    canvas.flush();
}

}  // namespace cb2menu
