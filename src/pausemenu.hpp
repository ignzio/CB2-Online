#pragma once
// The in-game menu (Esc). It is drawn over the frozen game picture with the same look as the online lobby (ui.hpp); the game is paused while it
// is open and the caller saves the settings when changed() says so.
//   Main              Resume / Sound and picture / Online (online play only) / Controls / Main menu / Quit
//   Sound and picture volume slider, display filter, fullscreen
//   Online            name tags, how solid the other players are (slider), the player list at the right edge
//   Controls          keyboard and game pad bindings side by side; Enter binds the column under the cursor, a game pad button or key is then pressed
// Keyboard, game pad and mouse all work: arrows / D-pad / hover to pick, Enter / A / click to choose, Left / Right / drag to move a slider, Esc / B /
// right click to go back.
#include <cctype>
#include <functional>
#include <string>
#include <vector>

#include "keymap.hpp"
#include "ui.hpp"

struct GLFWwindow;

namespace cb2menu {

struct Settings {
    int volume = 100;        // 0..100
    int filter = 2;          // 0 nearest, 1 bilinear, 2 sharp bilinear
    bool nameTags = true;    // names over the other players (online)
    int opacity = 100;       // how solid the other players' Crashes are, 10..100
    bool roster = true;      // the player list at the right edge (online)
    bool timer = true;       // the level timer in the corner
    bool smooth = false;     // 60 fps: an in-between picture every other vertical blank
    int textures = 0;        // 0 sharp, 1 smooth (bilinear texture filtering)
    bool perspective = false; // perspective-correct texturing
    bool widescreen = false;  // show the picture at the window's aspect ratio instead of 4:3
    bool vsync = true;       // wait for the monitor's refresh when showing a picture
    cb2input::Keys keys;     // keyboard and game pad bindings
};

enum class Result { None, MainMenu, Quit };

// One line of the Records page.
struct RecordRow { std::string level, best, detail; };

class PauseMenu {
public:
    struct Context {
        bool online = false;                          // shows the online-only page
        std::function<bool()> isFullscreen;
        std::function<void()> toggleFullscreen;
    };

    bool init(Settings* settings, const Context& context);   // needs the OpenGL context
    void open();
    void close() { page = Page::Closed; dragging = -1; }
    bool isOpen() const { return page != Page::Closed; }
    // A key press / repeat from the window's key callback while the menu is open (GLFW key code, GLFW action).
    void key(int glfwKey, int action);
    // True while the menu waits for a game pad button to bind: the caller then passes every newly pressed button to padButton instead of using it to navigate.
    bool capturingPad() const { return page == Page::Capture && column == 1; }
    void padButton(int padCode);
    // Mouse: position in the menu's 640 x 480 coordinates (outside the picture: anything outside 0..640 / 0..480), left button state; wheel in notches.
    void mouse(float x, float y, bool leftDown);
    void rightClick() { key(256 /* GLFW_KEY_ESCAPE */, 1 /* GLFW_PRESS */); }
    void wheel(float notches);
    // Scripted input for tests: up down left right enter esc del, key:NAME, pad:NAME, move:x/y, click:x/y, drag:x/y (the button stays down for the following moves) and release. Returns false for anything else.
    bool script(const std::string& token);
    Result takeResult() { const Result r = result; result = Result::None; return r; }
    bool takeChanged() { const bool c = changed; changed = false; return c; }
    void draw(double seconds);   // also call it while the menu is closed: it then shows the toast, if any
    // A short message over the game ("state saved"), shown for two seconds.
    void toast(const std::string& text, double seconds) {
        toastText = text;
        for (char& c : toastText) c = char(std::toupper(static_cast<unsigned char>(c)));   // the font has capitals only
        toastUntil = seconds + 2.0;
    }
    void setRecords(std::vector<RecordRow> rows) { recordRows = std::move(rows); }   // shown on the Records page; set before open()
    cb2ui::Canvas& surface() { return canvas; }   // for the overlay (hud.hpp), which draws with the same font

private:
    enum class Page { Closed, Main, Sound, Graphics, Online, Records, Controls, Capture, ConfirmMenu, ConfirmQuit };
    enum Item { kResume, kSound, kGraphics, kPerspective, kWide, kOnline, kRecords, kControls, kMainMenu, kQuit, kVolume, kFilter, kFullscreen, kTimer, kSmooth, kVsync, kTextures, kTags, kOpacity, kRoster, kBack };
    struct Hit { float x, y, w, h; int index; };

    cb2ui::Canvas canvas;
    Settings* s = nullptr;
    Context ctx;
    Page page = Page::Closed;
    int sel = 0, mainSel = 0, controlsSel = 0, confirmSel = 1, column = 0;   // column: 0 keyboard, 1 game pad (Controls page)
    bool changed = false;
    Result result = Result::None;
    std::string message;
    double messageUntil = 0, now = 0;
    std::string toastText;
    double toastUntil = 0;
    // mouse
    float mx = -1, my = -1, lastMx = -1, lastMy = -1;
    bool mouseDown = false, scriptDown = false;
    int dragging = -1;                 // item whose slider is being dragged
    std::vector<RecordRow> recordRows;
    int recordTop = 0;
    std::vector<Hit> hits;             // the clickable areas of the page, rebuilt by draw()
    float barX = 0, barW = 0;          // the slider bar of the rows (same for all rows)

    std::vector<Item> items() const;
    void setPage(Page p);
    void adjust(Item item, int direction);
    void setSlider(Item item, float x);
    void activate(Item item);
    void startCapture(int row, int col) { controlsSel = row; column = col; page = Page::Capture; message.clear(); }
    void move(int& index, int count, int direction) { index = (index + count + direction) % count; }
    int hitAt(float x, float y) const;
    static bool isSlider(Item i) { return i == kVolume || i == kOpacity; }
};

}  // namespace cb2menu
