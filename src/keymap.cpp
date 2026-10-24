#include "keymap.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace cb2input {

// The defaults are the keys the game has always used (see the README).
const Binding kBindings[kBindingCount] = {
    {"up", "UP", 1u << 4, GLFW_KEY_UP},         {"down", "DOWN", 1u << 6, GLFW_KEY_DOWN},     {"left", "LEFT", 1u << 7, GLFW_KEY_LEFT},
    {"right", "RIGHT", 1u << 5, GLFW_KEY_RIGHT}, {"cross", "JUMP - CROSS", 1u << 14, GLFW_KEY_Z}, {"square", "SPIN - SQUARE", 1u << 15, GLFW_KEY_A},
    {"circle", "CIRCLE", 1u << 13, GLFW_KEY_X}, {"triangle", "TRIANGLE", 1u << 12, GLFW_KEY_S}, {"l1", "L1", 1u << 10, GLFW_KEY_Q},
    {"r1", "R1", 1u << 11, GLFW_KEY_W},         {"l2", "L2", 1u << 8, GLFW_KEY_E},            {"r2", "R2", 1u << 9, GLFW_KEY_R},
    {"start", "START", 1u << 3, GLFW_KEY_ENTER}, {"select", "SELECT", 1u << 0, GLFW_KEY_RIGHT_SHIFT},
};

namespace {

struct Named { int key; const char* name; };
const Named kNamed[] = {
    {GLFW_KEY_SPACE, "SPACE"}, {GLFW_KEY_ENTER, "ENTER"}, {GLFW_KEY_KP_ENTER, "NUMPAD ENTER"}, {GLFW_KEY_BACKSPACE, "BACKSPACE"},
    {GLFW_KEY_UP, "UP"}, {GLFW_KEY_DOWN, "DOWN"}, {GLFW_KEY_LEFT, "LEFT"}, {GLFW_KEY_RIGHT, "RIGHT"},
    {GLFW_KEY_LEFT_SHIFT, "LEFT SHIFT"}, {GLFW_KEY_RIGHT_SHIFT, "RIGHT SHIFT"}, {GLFW_KEY_LEFT_CONTROL, "LEFT CTRL"}, {GLFW_KEY_RIGHT_CONTROL, "RIGHT CTRL"},
    {GLFW_KEY_LEFT_ALT, "LEFT ALT"}, {GLFW_KEY_RIGHT_ALT, "RIGHT ALT"}, {GLFW_KEY_INSERT, "INSERT"}, {GLFW_KEY_DELETE, "DELETE"},
    {GLFW_KEY_HOME, "HOME"}, {GLFW_KEY_END, "END"}, {GLFW_KEY_PAGE_UP, "PAGE UP"}, {GLFW_KEY_PAGE_DOWN, "PAGE DOWN"},
    {GLFW_KEY_COMMA, "COMMA"}, {GLFW_KEY_PERIOD, "PERIOD"}, {GLFW_KEY_SLASH, "SLASH"}, {GLFW_KEY_SEMICOLON, "SEMICOLON"},
    {GLFW_KEY_APOSTROPHE, "APOSTROPHE"}, {GLFW_KEY_LEFT_BRACKET, "LEFT BRACKET"}, {GLFW_KEY_RIGHT_BRACKET, "RIGHT BRACKET"},
    {GLFW_KEY_BACKSLASH, "BACKSLASH"}, {GLFW_KEY_MINUS, "MINUS"}, {GLFW_KEY_EQUAL, "EQUALS"}, {GLFW_KEY_GRAVE_ACCENT, "GRAVE"},
    {GLFW_KEY_CAPS_LOCK, "CAPS LOCK"}, {GLFW_KEY_KP_ADD, "NUMPAD PLUS"}, {GLFW_KEY_KP_SUBTRACT, "NUMPAD MINUS"}, {GLFW_KEY_KP_MULTIPLY, "NUMPAD TIMES"},
    {GLFW_KEY_KP_DIVIDE, "NUMPAD DIVIDE"}, {GLFW_KEY_KP_DECIMAL, "NUMPAD DOT"},
};

}  // namespace

std::string keyName(int k) {
    if (k == 0) return "NONE";
    if (k >= GLFW_KEY_A && k <= GLFW_KEY_Z) return std::string(1, char(k));
    if (k >= GLFW_KEY_0 && k <= GLFW_KEY_9) return std::string(1, char(k));
    if (k >= GLFW_KEY_F3 && k <= GLFW_KEY_F25) return "F" + std::to_string(k - GLFW_KEY_F1 + 1);
    if (k >= GLFW_KEY_KP_0 && k <= GLFW_KEY_KP_9) return "NUMPAD " + std::to_string(k - GLFW_KEY_KP_0);
    for (const Named& n : kNamed) if (n.key == k) return n.name;
    return "KEY " + std::to_string(k);
}

int keyFromName(const std::string& text) {
    std::string s;
    for (char c : text) s += char(std::toupper(static_cast<unsigned char>(c)));
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty() || s == "NONE") return 0;
    if (s.size() == 1 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    for (const Named& n : kNamed) if (s == n.name) return n.key;
    for (int k = GLFW_KEY_F3; k <= GLFW_KEY_F25; ++k) if (s == keyName(k)) return k;
    for (int k = GLFW_KEY_KP_0; k <= GLFW_KEY_KP_9; ++k) if (s == keyName(k)) return k;
    if (s.rfind("KEY ", 0) == 0) return std::atoi(s.c_str() + 4);
    return 0;
}

bool reservedKey(int k) { return k == GLFW_KEY_ESCAPE || k == GLFW_KEY_TAB || k == GLFW_KEY_F1 || k == GLFW_KEY_F2 || k == GLFW_KEY_F5 || k == GLFW_KEY_F6 || k == GLFW_KEY_F9 || k == GLFW_KEY_F11 || k == GLFW_KEY_F12; }

void Keys::reset() { for (int i = 0; i < kBindingCount; ++i) key[i] = kBindings[i].defaultKey; }

void Keys::load(const std::map<std::string, std::string>& names) {
    reset();
    for (int i = 0; i < kBindingCount; ++i) {
        const auto it = names.find(kBindings[i].id);
        if (it == names.end()) continue;
        const int k = keyFromName(it->second);
        if (k != 0 && !reservedKey(k)) key[i] = k;
        else if (k == 0 && it->second == "NONE") key[i] = 0;   // explicitly unbound
    }
    // a hand-edited file could bind one key twice: the first binding keeps it
    for (int i = 0; i < kBindingCount; ++i)
        for (int j = i + 1; j < kBindingCount; ++j)
            if (key[i] != 0 && key[i] == key[j]) key[j] = 0;
}

int Keys::owner(int k) const {
    for (int i = 0; i < kBindingCount; ++i) if (key[i] == k && k != 0) return i;
    return -1;
}

void Keys::assign(int index, int k) {
    if (index < 0 || index >= kBindingCount || reservedKey(k)) return;
    const int other = owner(k);
    if (other >= 0 && other != index) key[other] = key[index];   // swap: the other action gets the key this one gave up
    key[index] = k;
}

}  // namespace cb2input
