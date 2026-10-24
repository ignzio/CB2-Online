#include "pausemenu.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cb2menu {

using namespace cb2ui;

namespace {

const char* filterName(int f) { return f == 0 ? "NEAREST" : f == 1 ? "SMOOTH" : "SHARP"; }

constexpr int kBindings = cb2input::kBindingCount;
constexpr int kResetRow = kBindings, kBackRow = kBindings + 1, kControlRows = kBindings + 2;

// Controls page geometry
constexpr float kRowY = 90, kRowStep = 22, kRowH = 20, kLabelX = 40, kKeyX = 290, kKeyW = 150, kPadX = 450, kPadW = 150;

// A slim row for the Controls list: dark frame, wooden face, the selection lights it up.
void slim(Canvas& c, float x, float y, float w, float h, bool on) {
    c.rect(x - 1, y - 1, w + 2, h + 2, rgb(20, 10, 4));
    c.rect(x, y, w, h, on ? rgb(214, 134, 40) : rgb(120, 74, 30), on ? rgb(150, 82, 20) : rgb(74, 42, 16));
    c.rect(x, y, w, 2, rgb(255, 230, 150), on ? 0.5f : 0.15f);
}

void pointer(Canvas& c, float x, float y, float h) {
    c.triangle(x - 15, y + h * 0.5f - 8, x - 15, y + h * 0.5f + 8, x - 3, y + h * 0.5f, rgb(255, 198, 40));
}

}  // namespace

bool PauseMenu::init(Settings* settings, const Context& context) {
    s = settings;
    ctx = context;
    return canvas.init();
}

void PauseMenu::open() {
    page = Page::Main;
    sel = mainSel = 0;
    message.clear();
    dragging = -1;
}

void PauseMenu::setPage(Page p) {
    if (page == Page::Main && p != Page::Main) mainSel = sel;
    page = p;
    if (p == Page::Main) sel = mainSel; else sel = 0;
    message.clear();
}

std::vector<PauseMenu::Item> PauseMenu::items() const {
    switch (page) {
    case Page::Main: {
        std::vector<Item> v = {kResume, kSound};
        if (ctx.online) v.push_back(kOnline);
        v.push_back(kRecords);
        v.push_back(kControls);
        v.push_back(kMainMenu);
        v.push_back(kQuit);
        return v;
    }
    case Page::Sound: return {kVolume, kFilter, kFullscreen, kTimer, kBack};
    case Page::Online: return {kTags, kOpacity, kRoster, kBack};
    default: return {};
    }
}

void PauseMenu::adjust(Item item, int d) {
    switch (item) {
    case kVolume: {
        const int v = std::max(0, std::min(100, s->volume + 5 * d));
        if (v != s->volume) { s->volume = v; changed = true; }
        break;
    }
    case kOpacity: {
        const int v = std::max(10, std::min(100, s->opacity + 5 * d));
        if (v != s->opacity) { s->opacity = v; changed = true; }
        break;
    }
    case kFilter: s->filter = (s->filter + 3 + d) % 3; changed = true; break;
    case kFullscreen: if (ctx.toggleFullscreen) ctx.toggleFullscreen(); break;
    case kTags: s->nameTags = !s->nameTags; changed = true; break;
    case kRoster: s->roster = !s->roster; changed = true; break;
    case kTimer: s->timer = !s->timer; changed = true; break;
    default: break;
    }
}

void PauseMenu::setSlider(Item item, float x) {
    const float f = std::max(0.0f, std::min(1.0f, (x - barX) / std::max(1.0f, barW)));
    int v;
    if (item == kVolume) v = int(std::lround(f * 20.0f)) * 5;                 // 0..100 in steps of 5
    else v = 10 + int(std::lround(f * 18.0f)) * 5;                            // 10..100 in steps of 5
    int& target = item == kVolume ? s->volume : s->opacity;
    if (v != target) { target = v; changed = true; }
}

void PauseMenu::activate(Item item) {
    switch (item) {
    case kResume: close(); break;
    case kSound: setPage(Page::Sound); break;
    case kOnline: setPage(Page::Online); break;
    case kRecords: setPage(Page::Records); recordTop = 0; break;
    case kControls: setPage(Page::Controls); controlsSel = 0; column = 0; break;
    case kMainMenu: mainSel = sel; page = Page::ConfirmMenu; confirmSel = 1; break;
    case kQuit: mainSel = sel; page = Page::ConfirmQuit; confirmSel = 1; break;
    case kBack: setPage(Page::Main); break;
    case kFilter: case kFullscreen: case kTags: case kRoster: case kTimer: adjust(item, 1); break;
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
        const bool clear = k == GLFW_KEY_DELETE || k == GLFW_KEY_BACKSPACE;
        if (column == 1) {   // waiting for a game pad button: the keyboard can only clear or cancel
            if (clear) { s->keys.pad[controlsSel] = cb2input::kPadNone; changed = true; page = Page::Controls; }
            return;
        }
        if (clear) { s->keys.key[controlsSel] = 0; changed = true; page = Page::Controls; return; }
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
        if (up) move(controlsSel, kControlRows, up);
        else if (side) {
            if (controlsSel < kBindings) column = side < 0 ? 0 : 1;
            else controlsSel = side < 0 ? kResetRow : kBackRow;
        } else if (esc) setPage(Page::Main);
        else if (enter) {
            if (controlsSel < kBindings) startCapture(controlsSel, column);
            else if (controlsSel == kResetRow) { s->keys.reset(); changed = true; message = "DEFAULTS RESTORED"; messageUntil = now + 2.0; }
            else setPage(Page::Main);
        } else if (k == GLFW_KEY_DELETE && press && controlsSel < kBindings) {
            if (column == 0) s->keys.key[controlsSel] = 0; else s->keys.pad[controlsSel] = cb2input::kPadNone;
            changed = true;
        }
        return;
    }
    if (page == Page::Records) {
        const int n = int(recordRows.size());
        if (up) recordTop = std::max(0, std::min(std::max(0, n - 9), recordTop + up));
        else if (esc || enter) setPage(Page::Main);
        return;
    }
    // list pages
    const auto list = items();
    sel = std::min(sel, int(list.size()) - 1);
    if (up) move(sel, int(list.size()), up);
    else if (side) adjust(list[size_t(sel)], side);
    else if (esc) { if (page == Page::Main) close(); else setPage(Page::Main); }
    else if (enter) activate(list[size_t(sel)]);
}

void PauseMenu::padButton(int code) {
    if (!capturingPad()) return;
    s->keys.assignPad(controlsSel, code);
    changed = true;
    message.clear();
    page = Page::Controls;
}

int PauseMenu::hitAt(float x, float y) const {
    for (size_t i = 0; i < hits.size(); ++i)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h) return int(i);
    return -1;
}

void PauseMenu::wheel(float notches) {
    if (page == Page::Closed || notches == 0) return;
    const int d = notches > 0 ? -1 : 1;
    if (page == Page::Controls) move(controlsSel, kControlRows, d);
    else if (page == Page::ConfirmMenu || page == Page::ConfirmQuit) confirmSel = 1 - confirmSel;
    else if (page == Page::Records) recordTop = std::max(0, std::min(std::max(0, int(recordRows.size()) - 9), recordTop + (notches > 0 ? -1 : 1)));
    else if (page != Page::Capture) { const int n = int(items().size()); if (n) move(sel, n, d); }
}

void PauseMenu::mouse(float x, float y, bool down) {
    if (page == Page::Closed) { mouseDown = down; return; }
    mx = x; my = y;
    const bool moved = std::fabs(x - lastMx) > 0.5f || std::fabs(y - lastMy) > 0.5f;
    lastMx = x; lastMy = y;
    const bool pressed = down && !mouseDown;
    mouseDown = down;
    if (!down) dragging = -1;
    if (dragging >= 0) {
        const auto list = items();
        if (dragging < int(list.size())) setSlider(list[size_t(dragging)], x);
        return;
    }
    const int h = hitAt(x, y);
    if (h < 0) return;
    const int index = hits[size_t(h)].index;
    if (page == Page::Capture) return;
    if (page == Page::Records) { if (pressed) setPage(Page::Main); return; }
    if (page == Page::ConfirmMenu || page == Page::ConfirmQuit) {
        if (moved) confirmSel = index;
        if (pressed) { confirmSel = index; key(GLFW_KEY_ENTER, GLFW_PRESS); }
        return;
    }
    if (page == Page::Controls) {
        // index: row * 4 + part, part 0 label, 1 keyboard cell, 2 game pad cell
        const int row = index / 4, part = index % 4;
        if (moved) { controlsSel = row; if (part == 1) column = 0; else if (part == 2) column = 1; }
        if (pressed) {
            controlsSel = row;
            if (row < kBindings) { if (part == 1 || part == 2) startCapture(row, part == 2 ? 1 : 0); }
            else key(GLFW_KEY_ENTER, GLFW_PRESS);
        }
        return;
    }
    const auto list = items();
    if (index >= int(list.size())) return;
    if (moved) sel = index;
    if (pressed) {
        sel = index;
        const Item item = list[size_t(index)];
        if (isSlider(item) && x >= barX - 8 && x <= barX + barW + 8) { dragging = index; setSlider(item, x); }
        else activate(item);
    }
}

bool PauseMenu::script(const std::string& tok) {
    struct Map { const char* name; int key; };
    static const Map map[] = {{"up", GLFW_KEY_UP}, {"down", GLFW_KEY_DOWN}, {"left", GLFW_KEY_LEFT}, {"right", GLFW_KEY_RIGHT},
                              {"enter", GLFW_KEY_ENTER}, {"esc", GLFW_KEY_ESCAPE}, {"del", GLFW_KEY_DELETE}};
    for (const Map& m : map) if (tok == m.name) { key(m.key, GLFW_PRESS); return true; }
    if (tok.rfind("key:", 0) == 0) { const int k = cb2input::keyFromName(tok.substr(4)); if (k) { key(k, GLFW_PRESS); return true; } }
    if (tok.rfind("pad:", 0) == 0) { const int c = cb2input::padFromName(tok.substr(4)); if (c != cb2input::kPadNone) { padButton(c); return true; } }
    if (tok == "release") { scriptDown = false; mouse(mx, my, false); return true; }
    if (tok.rfind("move:", 0) == 0 || tok.rfind("click:", 0) == 0 || tok.rfind("drag:", 0) == 0) {
        const size_t colon = tok.find(':');
        const float x = float(std::atof(tok.c_str() + colon + 1));
        const size_t comma = tok.find('/');
        const float y = comma == std::string::npos ? 0.0f : float(std::atof(tok.c_str() + comma + 1));
        const bool isClick = tok[0] == 'c', isDrag = tok[0] == 'd';
        if (isClick) { scriptDown = false; mouse(x, y, false); mouse(x, y, true); mouse(x, y, false); }
        else if (isDrag) { scriptDown = true; mouse(x, y, false); mouse(x, y, true); }   // the button stays down for the following move tokens
        else mouse(x, y, scriptDown);
        return true;
    }
    return false;
}

void PauseMenu::draw(double t) {
    if (page == Page::Closed) {
        if (toastText.empty() || t > toastUntil) { toastText.clear(); return; }
        const float w = canvas.width(toastText, 18);
        canvas.rect(14, 30, w + 16, 30, rgb(0, 6, 12), 0.7f);
        canvas.text(toastText, 22, 34, 18, kWhite1, kWhite2);
        canvas.flush();
        return;
    }
    now = t;
    hits.clear();
    if (!message.empty() && now > messageUntil) message.clear();
    const bool blink = int(t * 2.0) % 2 == 0;
    canvas.rect(0, 0, kW, kH, rgb(0, 6, 12), 0.66f);
    canvas.rect(0, 22, kW, 4, rgb(150, 92, 38), 0.9f);
    canvas.rect(0, kH - 26, kW, 4, rgb(150, 92, 38), 0.9f);

    auto centredRow = [&](int index, float y, bool selected, const std::string& label) {
        plank(canvas, 130, y, 380, 36, selected);
        canvas.text(label, 320 - canvas.width(label, 24) * 0.5f, y + 4, 24, selected ? kGold1 : kWhite1, selected ? kGold2 : kWhite2);
        hits.push_back({127, y - 3, 386, 42, index});
    };
    auto valueRow = [&](int index, float y, bool selected, const std::string& label, const std::string& value, Rgb valueColour) {
        plank(canvas, 130, y, 380, 36, selected);
        canvas.text(label, 146, y + 5, 22, selected ? kGold1 : kWhite1, selected ? kGold2 : kWhite2);
        if (!value.empty()) canvas.text(value, 496 - canvas.width(value, 20), y + 7, 20, selected ? rgb(255, 246, 210) : valueColour);
        hits.push_back({127, y - 3, 386, 42, index});
    };

    std::string hint;
    const bool listPage = page == Page::Main || page == Page::Sound || page == Page::Online;
    if (listPage) {
        const char* heading = page == Page::Main ? "PAUSED" : page == Page::Sound ? "SOUND AND PICTURE" : "ONLINE";
        canvas.centred(heading, 38, page == Page::Main ? 54 : 44, kGold1, kGold2);
        const auto list = items();
        sel = std::min(sel, int(list.size()) - 1);
        barX = 262; barW = 144;
        float y = 112;
        for (size_t i = 0; i < list.size(); ++i, y += 44) {
            const bool on = int(i) == sel;
            const int idx = int(i);
            switch (list[i]) {
            case kResume: centredRow(idx, y, on, "RESUME"); break;
            case kSound: centredRow(idx, y, on, "SOUND AND PICTURE"); break;
            case kOnline: centredRow(idx, y, on, "ONLINE"); break;
            case kRecords: centredRow(idx, y, on, "RECORDS"); break;
            case kControls: centredRow(idx, y, on, "CONTROLS"); break;
            case kMainMenu: centredRow(idx, y, on, "MAIN MENU"); break;
            case kQuit: centredRow(idx, y, on, "QUIT GAME"); break;
            case kBack: centredRow(idx, y, on, "BACK"); break;
            case kVolume:
            case kOpacity: {
                const bool vol = list[i] == kVolume;
                const int v = vol ? s->volume : s->opacity;
                valueRow(idx, y, on, vol ? "VOLUME" : "OPACITY", std::to_string(v) + (vol ? "" : "%"), kTeal);
                const float f = vol ? float(v) / 100.0f : float(v - 10) / 90.0f;
                canvas.rect(barX, y + 12, barW, 12, rgb(0, 0, 0), 0.6f);
                canvas.rect(barX + 1, y + 13, (barW - 2) * f, 10, on ? rgb(255, 232, 110) : rgb(128, 232, 255), on ? rgb(255, 140, 24) : rgb(60, 160, 200));
                const float kx = barX + (barW - 4) * f;   // the handle
                canvas.rect(kx, y + 9, 5, 18, rgb(255, 246, 210), 0.95f);
                break;
            }
            case kFilter: valueRow(idx, y, on, "PICTURE", filterName(s->filter), kTeal); break;
            case kFullscreen: valueRow(idx, y, on, "FULLSCREEN", ctx.isFullscreen && ctx.isFullscreen() ? "ON" : "OFF", kTeal); break;
            case kTimer: valueRow(idx, y, on, "LEVEL TIMER", s->timer ? "ON" : "OFF", kTeal); break;
            case kTags: valueRow(idx, y, on, "NAME TAGS", s->nameTags ? "ON" : "OFF", kTeal); break;
            case kRoster: valueRow(idx, y, on, "PLAYER LIST", s->roster ? "ON" : "OFF", kTeal); break;
            }
        }
        if (page == Page::Online) {
            canvas.centred(sel == 1 ? "HOW SOLID THE OTHER PLAYERS' CRASHES ARE" : "F3 SHOWS OR HIDES THE PLAYER LIST IN THE GAME", 392, 15, kDim);
        }
        if (page == Page::Main && ctx.online) canvas.centred("ONLINE: THE OTHERS SEE YOU STAND STILL WHILE THIS IS OPEN", 432, 15, kDim);
        hint = "UP DOWN  SELECT      LEFT RIGHT  CHANGE      ENTER  CHOOSE      ESC  BACK";
    } else if (page == Page::Records) {
        canvas.centred("RECORDS", 22, 46, kGold1, kGold2);
        canvas.text("LEVEL", 70, 78, 14, kDim);
        canvas.text("BEST", 330, 78, 14, kDim);
        canvas.text("RUNS", 470, 78, 14, kDim);
        const int n = int(recordRows.size());
        if (n == 0) {
            canvas.centred("NO FINISHED LEVEL YET", 200, 24, kWhite1, kWhite2);
            canvas.centred("YOUR TIMES ARE SAVED IN RECORDS.TXT NEXT TO THE GAME", 240, 15, kDim);
        }
        for (int i = 0; i < std::min(9, n - recordTop); ++i) {
            const RecordRow& r = recordRows[size_t(recordTop + i)];
            const float y = 100 + float(i) * 34;
            slim(canvas, 56, y, 528, 28, i % 2 == 0);
            canvas.text(r.level, 70, y + 4, 18, kWhite1, kWhite2);
            canvas.text(r.best, 330, y + 4, 18, r.best == "-" ? kDim : kGreen);
            canvas.text(r.detail, 470, y + 6, 15, kDim);
        }
        if (recordTop > 0) canvas.triangle(320, 94, 308, 90, 332, 90, kGold1);
        if (recordTop + 9 < n) canvas.triangle(320, 414, 308, 404, 332, 404, kGold1);
        hint = "UP DOWN  SCROLL      ESC  BACK";
    } else if (page == Page::Controls || page == Page::Capture) {
        canvas.centred("CONTROLS", 20, 46, kGold1, kGold2);
        const bool capturing = page == Page::Capture;
        canvas.text("KEYBOARD", kKeyX + (kKeyW - canvas.width("KEYBOARD", 14)) * 0.5f, 70, 14, column == 0 ? kGold1 : kDim);
        canvas.text("GAME PAD", kPadX + (kPadW - canvas.width("GAME PAD", 14)) * 0.5f, 70, 14, column == 1 ? kGold1 : kDim);
        for (int i = 0; i < kBindings; ++i) {
            const float y = kRowY + float(i) * kRowStep;
            const bool rowOn = i == controlsSel;
            if (rowOn) pointer(canvas, 24, y, kRowH);
            canvas.text(cb2input::kBindings[i].label, kLabelX, y + 2, 15, rowOn ? kGold1 : kWhite1, rowOn ? kGold2 : kWhite2);
            for (int col = 0; col < 2; ++col) {
                const float cx = col == 0 ? kKeyX : kPadX, cw = col == 0 ? kKeyW : kPadW;
                const bool cellOn = rowOn && column == col;
                slim(canvas, cx, y, cw, kRowH, cellOn);
                const bool waiting = capturing && cellOn;
                const bool none = col == 0 ? s->keys.key[i] == 0 : s->keys.pad[i] == cb2input::kPadNone;
                std::string value = waiting ? (blink ? (col == 0 ? "PRESS A KEY" : "PRESS A BUTTON") : "") : col == 0 ? cb2input::keyName(s->keys.key[i]) : cb2input::padName(s->keys.pad[i]);
                const Rgb c = waiting ? kGreen : cellOn ? rgb(255, 246, 210) : none ? kDim : kWhite1;
                canvas.text(value, cx + (cw - canvas.width(value, 14)) * 0.5f, y + 3, 14, c);
            }
            hits.push_back({kLabelX, y, kKeyX - kLabelX, kRowH, i * 4});
            hits.push_back({kKeyX, y, kKeyW, kRowH, i * 4 + 1});
            hits.push_back({kPadX, y, kPadW, kRowH, i * 4 + 2});
        }
        const float by = kRowY + float(kBindings) * kRowStep + 6;
        for (int b = 0; b < 2; ++b) {
            const float bx = b == 0 ? 160.0f : 340.0f;
            const bool on = controlsSel == (b == 0 ? kResetRow : kBackRow);
            slim(canvas, bx, by, 140, 24, on);
            const std::string label = b == 0 ? "RESET" : "BACK";
            canvas.text(label, bx + (140 - canvas.width(label, 16)) * 0.5f, by + 3, 16, on ? kGold1 : kWhite1, on ? kGold2 : kWhite2);
            hits.push_back({bx, by, 140, 24, (b == 0 ? kResetRow : kBackRow) * 4});
        }
        if (!message.empty()) canvas.centred(message, by + 30, 15, kRed);
        hint = capturing ? (column == 0 ? "PRESS THE NEW KEY      DEL  NO KEY      ESC  CANCEL" : "PRESS A GAME PAD BUTTON      DEL  NO BUTTON      ESC  CANCEL")
                         : "ARROWS  PICK      ENTER  REBIND      DEL  CLEAR      ESC  BACK";
    } else {
        canvas.centred("PAUSED", 38, 54, kGold1, kGold2);
        const bool toMenu = page == Page::ConfirmMenu;
        canvas.centred(toMenu ? "BACK TO THE MAIN MENU?" : "QUIT THE GAME?", 130, 30, kGold1, kGold2);
        canvas.centred("PROGRESS YOU HAVE NOT SAVED ON THE MEMORY CARD IS LOST", 176, 16, kRed);
        centredRow(0, 230, confirmSel == 0, "YES");
        centredRow(1, 276, confirmSel == 1, "NO");
        hint = "UP DOWN  SELECT      ENTER  CHOOSE      ESC  BACK";
    }
    canvas.centred(hint, 446, 14, kDim);
    canvas.flush();
}

}  // namespace cb2menu
