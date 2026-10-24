#pragma once
// Keyboard bindings for the PlayStation pad. The pad has a fixed set of buttons; each can be bound to one keyboard key (GLFW key code).
// Bindings are saved in cb2.cfg as `key_<id> = NAME`, e.g. `key_cross = Z`.
#include <cstdint>
#include <map>
#include <string>

namespace cb2input {

struct Binding {
    const char* id;       // config key suffix
    const char* label;    // shown in the menu (the font is upper case)
    uint16_t bit;         // PlayStation pad bit (0 = pressed)
    int defaultKey;       // GLFW key code
};

constexpr int kBindingCount = 14;
extern const Binding kBindings[kBindingCount];

struct Keys {
    int key[kBindingCount] = {};
    void reset();                                           // defaults
    // Takes the bindings saved in the config file (`id` -> key name); anything missing or unknown keeps its default.
    void load(const std::map<std::string, std::string>& names);
    // Binds `glfwKey` to binding `index`; the binding that had that key gets the old key of `index` (a swap), so no key does two things.
    void assign(int index, int glfwKey);
    int owner(int glfwKey) const;                           // index of the binding using this key, or -1
};

std::string keyName(int glfwKey);                           // "Z", "LEFT", "LEFT SHIFT", "NONE" for 0
int keyFromName(const std::string& name);                   // 0 when unknown
// Keys that cannot be bound because the player or the menu uses them: Esc, Tab (fast-forward), F1, F2, F11, F12.
bool reservedKey(int glfwKey);

}  // namespace cb2input
