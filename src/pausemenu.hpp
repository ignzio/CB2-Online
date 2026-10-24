#pragma once
// The in-game menu (Esc): volume, display options, keyboard bindings, back to the main menu, quit. It is drawn over the frozen game picture
// with the same look as the online lobby (ui.hpp). The game is paused while it is open; settings are saved by the caller (changed()).
#include <functional>
#include <cctype>
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
    cb2input::Keys keys;
};

enum class Result { None, MainMenu, Quit };

class PauseMenu {
public:
    struct Context {
        bool online = false;                          // shows the online-only rows
        std::function<bool()> isFullscreen;
        std::function<void()> toggleFullscreen;
    };

    bool init(Settings* settings, const Context& context);   // needs the OpenGL context
    void open();
    void close() { page = Page::Closed; }
    bool isOpen() const { return page != Page::Closed; }
    // A key press / repeat from the window's key callback while the menu is open (GLFW key code, GLFW action).
    void key(int glfwKey, int action);
    // Scripted input for tests: up down left right enter esc del, or key:NAME. Returns false for anything else.
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

private:
    enum class Page { Closed, Main, Controls, Capture, ConfirmMenu, ConfirmQuit };
    enum Item { kResume, kControls, kVolume, kFilter, kFullscreen, kTags, kMainMenu, kQuit };

    cb2ui::Canvas canvas;
    Settings* s = nullptr;
    Context ctx;
    Page page = Page::Closed;
    int sel = 0, controlsSel = 0, confirmSel = 1;
    bool changed = false;
    Result result = Result::None;
    std::string message;
    double messageUntil = 0, now = 0;
    std::string toastText;
    double toastUntil = 0;

    std::vector<Item> items() const;
    void adjust(Item item, int direction);
    void activate(Item item);
    void move(int& index, int count, int direction) { index = (index + count + direction) % count; }
};

}  // namespace cb2menu
