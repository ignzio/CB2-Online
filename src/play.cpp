// Crash Bandicoot 2 on PC: runs the game's own code on the platform layer and shows the PSX display in a window.
//   cb2_play <game folder>          keys: arrows = D-pad, Z = Cross, X = Circle, A = Square, S = Triangle,
//                                         Q/W = L1/R1, Enter = Start, Right Shift = Select, F12 = screenshot
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include "audio.hpp"
#include "bundle.hpp"
#include "config.hpp"
#include "glrender.hpp"
#include "lobby.hpp"
#include "multiplayer.hpp"
#include "pausemenu.hpp"
#include "relaunch.hpp"
#include "updater.hpp"
#include "discimport.hpp"
#include "savestate.hpp"
#include "hud.hpp"
#include "roster.hpp"
#include "psx.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

#ifndef CB2_VERSION
#define CB2_VERSION "0.0.0"
#endif
#ifndef CB2_UPDATE_REPO
#define CB2_UPDATE_REPO ""
#endif

cb2menu::PauseMenu* gMenu = nullptr;   // the key callback needs it

void onScroll(GLFWwindow*, double, double dy) {
    if (gMenu && gMenu->isOpen()) gMenu->wheel(float(dy));
}

void onKeyEvent(GLFWwindow*, int key, int, int action, int) {
    if (!gMenu) return;
    if (gMenu->isOpen()) gMenu->key(key, action);
    else if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) gMenu->open();
}

const char* kVert = "#version 330 core\nlayout(location=0) in vec2 p; out vec2 uv;\n"
                    "void main(){ uv = vec2(p.x*0.5+0.5, 0.5-p.y*0.5); gl_Position = vec4(p,0,1); }\n";
const char* kFrag = R"(#version 330 core
in vec2 uv; out vec4 c;
uniform sampler2D tex;
uniform vec4 rect;      // xy = offset, zw = size of the displayed area in normalised texture coordinates
uniform vec2 area;      // displayed area in texels
uniform int mode;       // 0 nearest, 1 bilinear, 2 sharp bilinear (integer pre-scale, smooth edges only)
uniform float prescale; // window pixels per console pixel
void main() {
    vec2 t = uv * area;
    if (mode == 2) {
        vec2 fl = floor(t), fr = fract(t);
        vec2 region = vec2(0.5 - 0.5 / max(prescale, 1.0));
        vec2 d = fr - 0.5;
        t = fl + (d - clamp(d, -region, region)) * max(prescale, 1.0) + 0.5;
    }
    t = clamp(t, vec2(0.5), area - 0.5);
    c = texture(tex, rect.xy + t / area * rect.zw);
}
)";

GLuint compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "display shader error: %s\n", log);
    }
    return s;
}

uint16_t readPad(GLFWwindow* win, const cb2input::Keys& keys) {
    uint16_t pad = 0xFFFF;
    for (int i = 0; i < cb2input::kBindingCount; ++i)
        if (keys.key[i] && glfwGetKey(win, keys.key[i]) == GLFW_PRESS) pad &= uint16_t(~cb2input::kBindings[i].bit);
    GLFWgamepadstate gp;
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp)) {
        // the game pad buttons are bound in the menu (Controls); the left stick always works as the D-pad
        for (int i = 0; i < cb2input::kBindingCount; ++i) {
            const int code = keys.pad[i];
            if (code == cb2input::kPadNone) continue;
            const bool down = code >= cb2input::kPadAxisBase ? gp.axes[code - cb2input::kPadAxisBase] > 0.5f : (code >= 0 && code <= GLFW_GAMEPAD_BUTTON_LAST && gp.buttons[code]);
            if (down) pad &= uint16_t(~cb2input::kBindings[i].bit);
        }
        const float lx = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X], ly = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        if (lx < -0.5f) pad &= uint16_t(~(1u << 7));
        if (lx > 0.5f) pad &= uint16_t(~(1u << 5));
        if (ly < -0.5f) pad &= uint16_t(~(1u << 4));
        if (ly > 0.5f) pad &= uint16_t(~(1u << 6));
    }
    return pad;
}

}  // namespace

int main(int argc, char** argv) {
    // The windowed game has no console: CB2_MPLOG=file.txt (with CB2_MPDEBUG=1) writes the online diagnostics to a file.
    if (const char* log = std::getenv("CB2_MPLOG")) { std::freopen(log, "w", stdout); std::setvbuf(stdout, nullptr, _IOLBF, 0); }
    // Settings come from cb2.cfg (see src/config.hpp); the command line overrides them. The first argument may be the game folder.
    cb2::Config cfg;
    // A packed executable carries the game data itself (see cb2_pack): then no game folder is needed.
    psx::Bundle bundle;
    const bool bundled = bundle.openSelf();
    const std::string exeDir = psx::selfPath().empty() ? std::filesystem::path(argv[0]).parent_path().string()
                                                       : std::filesystem::path(psx::selfPath()).parent_path().string();
    cfg.load(exeDir);
    int first = 1;
    std::string loadStatePath, saveStatePath;   // --load-state file, --save-state-at FRAME:file
    uint64_t saveStateFrame = 0;
    bool relativeFrames = false;
    std::string importImage;                  // a disc image (.cue/.bin/.iso) dropped on the exe or given with --import: its files are copied to <exe folder>\data
    if (argc > 1 && std::strncmp(argv[1], "--", 2) != 0) {
        std::error_code isFile;
        if (cb2import::looksLikeImage(argv[1]) && std::filesystem::is_regular_file(argv[1], isFile)) importImage = argv[1];
        else cfg.game = argv[1];
        first = 2;
    }
    std::vector<std::string> mods = cfg.mods;
    std::string cardPath = cfg.card;
    uint32_t ramMb = uint32_t(cfg.ramMb);
    int hiresScale = cfg.scale;               // internal resolution multiplier of the OpenGL renderer (1 = software picture only)
    uint64_t captureFrame = 0;                // --capture N:file.png writes screenshots at frame N and exits (headless testing)
    std::string capturePath;
    bool autoStart = false, hidden = false, interpOnly = false;   // --autostart presses Start at the title screen; --hidden hides the window
    bool windowCapture = false;
    cb2mp::Options online;                    // --online / --room / --rendezvous / --name: other players inside the game (docs/ONLINE.md)
    online.name = cfg.name;
    online.room = cfg.room;
    online.rendezvous = cfg.rendezvous;
    online.directory = cfg.directory;
    bool noLobby = false, forceLobby = false;   // --no-lobby skips the lobby screen; --lobby shows it even with --hidden (tests)
    cb2lobby::Script lobbyScript;               // --lobby-script up,down,enter,... and --lobby-shot prefix: scripted lobby for tests
    uint64_t captureAfterPuppet = 0, puppetSince = 0;
    uint64_t menuAt = 0;                        // --menu-at F / --menu-script open,down,...: scripted input for the in-game menu (tests)
    std::vector<std::string> menuScript;
    uint64_t paceFrom = 0;                    // headless runs: from this frame on, run at real speed (so that two instances can meet)
    struct PadPress { uint64_t from, to; uint16_t mask; };
    std::vector<PadPress> pressList;          // --press 2310-2314:start+cross : scripted pad input for tests
    bool nativeCode = false;                  // --native: run the translated engine (identical output, not faster yet)
    for (int i = first; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--game") && more) cfg.game = argv[++i];
        else if (!std::strcmp(argv[i], "--import") && more) importImage = argv[++i];
        else if (!std::strcmp(argv[i], "--load-state") && more) loadStatePath = argv[++i];
        else if (!std::strcmp(argv[i], "--rel")) relativeFrames = true;   // frame numbers of --press / --capture / --save-state-at / --pace-from count from the loaded state
        else if (!std::strcmp(argv[i], "--save-state-at") && more) {   // FRAME:file - save a state at that frame and quit (makes a state from a scripted run)
            const std::string s = argv[++i];
            const size_t colon = s.find(':');
            if (colon != std::string::npos) { saveStateFrame = std::strtoull(s.c_str(), nullptr, 10); saveStatePath = s.substr(colon + 1); }
        }
        else if (!std::strcmp(argv[i], "--native")) nativeCode = true;
        else if (!std::strcmp(argv[i], "--mod") && more) mods.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--card") && more) cardPath = argv[++i];
        else if (!std::strcmp(argv[i], "--ram-mb") && more) ramMb = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--scale") && more) hiresScale = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--capture") && more) {
            const std::string s = argv[++i];
            captureFrame = std::strtoull(s.c_str(), nullptr, 10);
            capturePath = s.substr(s.find(char(58)) + 1);
        }
        else if (!std::strcmp(argv[i], "--autostart")) autoStart = true;
        else if (!std::strcmp(argv[i], "--window-capture")) windowCapture = true;   // with --capture: save the window's own picture
        else if (!std::strcmp(argv[i], "--hidden")) hidden = true;
        else if (!std::strcmp(argv[i], "--online")) online.online = true;   // straight into the room, without the lobby
        else if (!std::strcmp(argv[i], "--room") && more) { online.room = argv[++i]; online.online = true; }
        else if (!std::strcmp(argv[i], "--directory") && more) { online.directory = argv[++i]; online.online = true; }
        else if (!std::strcmp(argv[i], "--rendezvous") && more) { online.rendezvous = argv[++i]; online.online = true; }
        else if (!std::strcmp(argv[i], "--fake-player")) online.fake = (more && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) ? std::atoi(argv[++i]) : 1;   // test ghost, no network
        else if (!std::strcmp(argv[i], "--name") && more) online.name = argv[++i];
        else if (!std::strcmp(argv[i], "--no-lobby")) noLobby = true;
        else if (!std::strcmp(argv[i], "--menu-at") && more) menuAt = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--menu-script") && more) {
            const std::string s = argv[++i];
            for (size_t pos = 0; pos <= s.size();) {
                const size_t comma = s.find(',', pos);
                const std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!tok.empty()) menuScript.push_back(tok);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (!std::strcmp(argv[i], "--lobby")) forceLobby = true;
        else if (!std::strcmp(argv[i], "--lobby-shot") && more) lobbyScript.shotPrefix = argv[++i];
        else if (!std::strcmp(argv[i], "--lobby-script") && more) {
            const std::string s = argv[++i];
            for (size_t pos = 0; pos <= s.size();) {
                const size_t comma = s.find(',', pos);
                const std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!tok.empty()) lobbyScript.tokens.push_back(tok);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        else if (!std::strcmp(argv[i], "--pace-from") && more) paceFrom = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--capture-after-puppet") && more) captureAfterPuppet = std::strtoull(argv[++i], nullptr, 10);   // wait for another player, then N frames
        else if (!std::strcmp(argv[i], "--press") && more) {
            static const struct { const char* n; uint16_t b; } kNames[] = {{"select", 1u << 0}, {"start", 1u << 3}, {"up", 1u << 4}, {"right", 1u << 5},
                {"down", 1u << 6}, {"left", 1u << 7}, {"l1", 1u << 10}, {"r1", 1u << 11}, {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};
            const std::string s = argv[++i];
            const size_t dash = s.find('-'), colon = s.find(':');
            if (dash != std::string::npos && colon != std::string::npos) {
                PadPress pp{std::strtoull(s.c_str(), nullptr, 10), std::strtoull(s.c_str() + dash + 1, nullptr, 10), 0};
                const std::string names = s.substr(colon + 1);
                for (size_t pos = 0; pos <= names.size();) {
                    const size_t plus = names.find('+', pos);
                    const std::string n = names.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
                    for (const auto& k : kNames) if (n == k.n) pp.mask |= k.b;
                    if (plus == std::string::npos) break;
                    pos = plus + 1;
                }
                pressList.push_back(pp);
            }
        }
        else if (!std::strcmp(argv[i], "--interp")) interpOnly = true;   // run everything on the interpreter (reference mode)
    }
    if (online.online || online.fake) nativeCode = false;   // online play hooks the interpreter
    if (!bundled) cfg.resolvePaths(exeDir);
    if (cardPath == "memcard0.mcr") cardPath = (std::filesystem::path(exeDir) / "memcard0.mcr").string();   // saves live next to the executable
    if (!bundled) {
        // First run: the game files are not here. Copy them out of the player's own disc image.
        std::string err = cfg.validate();
        const bool wantImport = !importImage.empty() || (!err.empty() && !hidden);
        if (wantImport) {
            const std::string dataDir = (std::filesystem::path(exeDir) / "data").string();
            if (importImage.empty() && cb2import::ask("The game files were not found.\n\nThis program does not include the game. Choose your own Crash Bandicoot 2 (NTSC-U) disc image "
                                                      "(.cue/.bin or .iso) and the files will be copied for you, once.\n\nPress OK to choose the disc image."))
                importImage = cb2import::pickImage(exeDir);
            if (!importImage.empty()) {
                const cb2import::Result r = cb2import::importDisc(importImage, dataDir);
                if (r.ok) {
                    cfg.game = dataDir;
                    cfg.remember(exeDir, "game", "data");
                    err = cfg.validate();
                    if (err.empty() && !hidden) cb2import::inform("Done: " + std::to_string(r.files) + " game files were copied to\n" + dataDir + "\n\nThe game starts now. You do not need the disc image any more.");
                } else {
                    psx::showError("The game files could not be imported: " + r.error);
                    return 2;
                }
            }
        }
        if (!err.empty()) {
            const std::string usage = err + "\nusage: cb2_play [game folder] [--game <folder>] [--mod <folder>]... [--card file.mcr] [--ram-mb 2..16] [--scale 1..8] [--native]";
            std::fprintf(stderr, "%s\n", usage.c_str());
            psx::showError(usage);
            return 2;
        }
    }
    const std::string folder = cfg.game;
    psx::Platform p;
    if (ramMb > 2) p.setRamMegabytes(ramMb);
    p.echoPrintf = false;
    p.card.path = cardPath;
    if (!p.card.load()) p.card.save();   // a missing card file starts as a new, formatted card
    std::string exe = folder + "\\SCUS_941.54";
    for (const auto& m : mods) if (std::filesystem::exists(m + "\\SCUS_941.54")) exe = m + "\\SCUS_941.54";
    if (bundled) {
        std::vector<uint8_t> image;
        const bool modExe = exe != folder + "\\SCUS_941.54";
        const bool ok = modExe ? p.loadExe(exe) : (bundle.read("SCUS_941.54", image) && p.loadExeImage(image));
        if (!ok) { psx::showError("cannot load the game executable from the packed data"); return 2; }
        if (!p.mountBundle(bundle.file, bundle.files, mods)) { psx::showError("cannot read the packed game data"); return 2; }
    } else {
        if (!p.loadExe(exe)) { std::fprintf(stderr, "cannot load %s\n", exe.c_str()); psx::showError("cannot load " + exe); return 2; }
        if (!p.mountDisc(folder, mods)) { std::fprintf(stderr, "cannot mount %s\n", folder.c_str()); psx::showError("cannot mount " + folder); return 2; }
    }
    if (nativeCode && !interpOnly) {
        if (p.enableNative()) p.yieldOnVblank = true;
        else std::fprintf(stderr, "no translated engine code linked in (run tools/recomp.py); using the interpreter\n");
    }

    if (!glfwInit()) { psx::showError("cannot initialise the window system"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(960, 720, "Crash Bandicoot 2 (native port)", nullptr, nullptr);
    if (!win) { psx::showError("cannot create an OpenGL 3.3 window (update the graphics driver)"); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(0);
    if (!gladLoadGL(glfwGetProcAddress)) return 1;

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, kVert));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, kFrag));
    glLinkProgram(prog);
    {
        GLint linked = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[2048];
            glGetProgramInfoLog(prog, sizeof log, nullptr, log);
            std::fprintf(stderr, "display program link error: %s\n", log);
        }
    }
    const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    GLuint vao, vbo, tex;
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    psx::HiResRenderer hires;
    r3000::PgxpMap preciseVerts;
    bool useHires = false;
    if (hiresScale > 1) {
        useHires = hires.init(hiresScale);
        if (!useHires) std::fprintf(stderr, "OpenGL renderer could not be created; using the software picture\n");
        p.gpu.recording = useHires;
        if (useHires && !std::getenv("CB2_NOPRECISE")) { p.cpu.pgxp = &preciseVerts; p.gpu.pgxp = &preciseVerts; }
        p.gpu.rasterize = !useHires || std::getenv("CB2_SOFTRASTER") != nullptr;   // the picture comes from the OpenGL renderer
    }
    bool showHires = useHires;

    // Online play hooks the original game's object update, which only the interpreter runs.
    cb2mp::Multiplayer mp(p);
    bool onlineStarted = false;
    // The lobby comes first when nothing on the command line already decided between playing alone and online.
    const bool commandLineOnline = online.online || online.fake;
    if (cfg.lobby && !noLobby && !nativeCode && !interpOnly && !commandLineOnline && (forceLobby || (!captureFrame && !hidden))) {
        // Look for a newer release while the lobby is open. CB2_FAKE_VERSION pretends to be another version (to try the updater).
        std::string version = CB2_VERSION;
        if (const char* fake = std::getenv("CB2_FAKE_VERSION")) version = fake;
        cb2update::cleanLeftovers();
        cb2update::Updater updater;
        const bool checkUpdates = cfg.update && !std::string(CB2_UPDATE_REPO).empty();
        if (checkUpdates) updater.startCheck(CB2_UPDATE_REPO, version);
        const cb2lobby::Outcome outcome = cb2lobby::run(win, mp, online, lobbyScript, checkUpdates ? &updater : nullptr, version);
        if (outcome == cb2lobby::Outcome::Quit) { glfwTerminate(); return 0; }
        if (outcome == cb2lobby::Outcome::Update) {
            std::string err;
            if (updater.install(err)) {
                std::printf("updated to %s, restarting\n", updater.latest().c_str());
                glfwTerminate();
                cb2::relaunchToLobby(argc, argv);
                return 0;
            }
            std::fprintf(stderr, "update: %s\n", err.c_str());
            psx::showError("The update could not be installed: " + err);
        }
        if (outcome == cb2lobby::Outcome::Online) {
            onlineStarted = true;
            mp.install();
            cfg.remember(exeDir, "name", online.name);
            cfg.remember(exeDir, "room", online.room);
            cfg.remember(exeDir, "rendezvous", online.rendezvous);
            cfg.remember(exeDir, "directory", online.directory);
            std::printf("online play: room %s, %s\n", online.room.c_str(), online.rendezvous.empty() ? "this PC / LAN only" : ("server " + online.rendezvous).c_str());
        } else {
            cfg.remember(exeDir, "name", online.name);
        }
    }
    if (!onlineStarted && commandLineOnline) {
        std::string err;
        if (!mp.start(online, err)) {
            std::fprintf(stderr, "online play: %s\n", err.c_str());
            psx::showError("Online play: " + err);
        } else {
            nativeCode = false;
            mp.install();
            std::printf("online play: %s\n", online.fake ? "test ghost" : ("room " + online.room + (online.rendezvous.empty() ? ", this PC / LAN only" : ", server " + online.rendezvous)).c_str());
        }
    }
    bool frameReady = false;
    p.onVblank = [&] { frameReady = true; mp.tick(); };
    // settings of the in-game menu (Esc), kept in cb2.cfg
    cb2menu::Settings settings;
    settings.volume = cfg.volume;
    settings.filter = cfg.filter;
    settings.nameTags = cfg.nameTags;
    settings.opacity = cfg.opacity;
    settings.roster = cfg.roster;
    settings.keys.load(cfg.keyNames, cfg.padNames);
    psx::AudioOut audio;
    if (audio.start(44100)) p.spu.sink = [&](int16_t l, int16_t r) {
        const int gain = settings.volume * settings.volume;   // 0..10000: a quadratic curve sounds more even than a linear one
        audio.push(int16_t(int(l) * gain / 10000), int16_t(int(r) * gain / 10000));
    };
    else std::fprintf(stderr, "no audio device; continuing silently\n");
    std::vector<uint32_t> rgba(1024 * 512, 0xFF000000u);
    int shot = 0;
    bool f1Down = false, f2Down = false, f11Down = false, f5Down = false, f6Down = false, f9Down = false, f3Down = false, rightWas = false;
    cb2hud::Hud hud;
    cb2roster::Roster roster;
    std::vector<cb2roster::Event> rosterEvents;
    uint64_t lastTitle = 0;
    auto toggleFullscreen = [&] {
        if (glfwGetWindowMonitor(win)) glfwSetWindowMonitor(win, nullptr, 100, 100, 960, 720, 0);
        else if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
            const GLFWvidmode* vm = glfwGetVideoMode(m);
            glfwSetWindowMonitor(win, m, 0, 0, vm->width, vm->height, vm->refreshRate);
        }
    };
    cb2menu::PauseMenu menu;
    {
        cb2menu::PauseMenu::Context mc;
        mc.online = mp.active();
        mc.isFullscreen = [&] { return glfwGetWindowMonitor(win) != nullptr; };
        mc.toggleFullscreen = toggleFullscreen;
        if (!menu.init(&settings, mc)) std::fprintf(stderr, "the in-game menu could not start\n");
        gMenu = &menu;
        glfwSetKeyCallback(win, onKeyEvent);
        glfwSetScrollCallback(win, onScroll);
    }
    bool wasPaused = false, suppressPad = false, relaunch = false, padPrimed = false;
    size_t menuPos = 0;
    double menuWaitUntil = 0;
    std::string menuShotPath;
    auto saveWindow = [&](const std::string& path, int fw, int fh) {
        std::vector<uint8_t> px(size_t(fw) * size_t(fh) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        stbi_flip_vertically_on_write(1);
        stbi_write_png(path.c_str(), fw, fh, 4, px.data(), fw * 4);
    };
    auto saveSettings = [&] {
        cfg.remember(exeDir, "volume", std::to_string(settings.volume));
        cfg.remember(exeDir, "filter", std::to_string(settings.filter));
        cfg.remember(exeDir, "nametags", settings.nameTags ? "on" : "off");
        cfg.remember(exeDir, "opacity", std::to_string(settings.opacity));
        cfg.remember(exeDir, "roster", settings.roster ? "on" : "off");
        for (int i = 0; i < cb2input::kBindingCount; ++i) {
            cfg.remember(exeDir, std::string("key_") + cb2input::kBindings[i].id, cb2input::keyName(settings.keys.key[i]));
            cfg.remember(exeDir, std::string("pad_") + cb2input::kBindings[i].id, cb2input::padName(settings.keys.pad[i]));
        }
    };

    auto padNow = [&]() {
        uint16_t pad = readPad(win, settings.keys);
        // after the menu closes, keys still held from it (Enter!) must not reach the game until they have been released
        if (suppressPad) { if (pad == 0xFFFF) suppressPad = false; else return uint16_t(0xFFFF); }
        if (autoStart && p.frames >= 2310 && p.frames <= 2314) pad &= uint16_t(~(1u << 3));
        for (const PadPress& pp : pressList) if (p.frames >= pp.from && p.frames <= pp.to) pad &= uint16_t(~pp.mask);
        return pad;
    };
    double t0 = glfwGetTime();
    uint64_t base = p.frames;
    bool quitRequested = false;
    // Save states (F5 save, F9 load, F6 next slot): practice and test runs start from any moment of the game. Files: <exe folder>\states\slotN.cb2state
    int stateSlot = 1;
    auto slotPath = [&](int n) { return (std::filesystem::path(exeDir) / "states" / ("slot" + std::to_string(n) + ".cb2state")).string(); };
    auto doSave = [&](const std::string& path) {
        std::vector<uint8_t> extra(4);
        const uint32_t lvl = mp.levelId();
        std::memcpy(extra.data(), &lvl, 4);
        std::string err;
        const bool ok = psx::saveState(p, path, extra, err);
        if (!ok) std::fprintf(stderr, "save state: %s\n", err.c_str());
        return std::make_pair(ok, err);
    };
    auto doLoad = [&](const std::string& path) {
        std::vector<uint8_t> extra;
        std::string err;
        const bool ok = psx::loadState(p, path, extra, err);
        if (ok) {
            uint32_t lvl = 0xFF;
            if (extra.size() >= 4) std::memcpy(&lvl, extra.data(), 4);
            mp.stateLoaded(lvl);
            t0 = glfwGetTime();
            base = p.frames;
            suppressPad = true;
        } else {
            std::fprintf(stderr, "load state: %s\n", err.c_str());
        }
        return std::make_pair(ok, err);
    };
    if (!loadStatePath.empty()) {
        const auto r = doLoad(loadStatePath);
        if (!r.first) { psx::showError("Cannot load the saved state: " + r.second); glfwTerminate(); return 2; }
        std::printf("loaded state %s (frame %llu)\n", loadStatePath.c_str(), (unsigned long long)p.frames);
        if (relativeFrames) {
            for (PadPress& pp : pressList) { pp.from += p.frames; pp.to += p.frames; }
            if (captureFrame) captureFrame += p.frames;
            if (saveStateFrame) saveStateFrame += p.frames;
            if (paceFrom) paceFrom += p.frames;
            if (menuAt) menuAt += p.frames;
        }
    }
    while (!glfwWindowShouldClose(win) && !quitRequested) {
        // scripted menu input (tests): open, wait:N (N x 16 ms), shot:file.png, exit, or anything the menu understands
        if (!menuScript.empty() && p.frames >= menuAt && menuPos < menuScript.size() && glfwGetTime() >= menuWaitUntil) {
            const std::string& tok = menuScript[menuPos++];
            if (tok == "open") menu.open();
            else if (tok == "exit") break;
            else if (tok.rfind("wait:", 0) == 0) menuWaitUntil = glfwGetTime() + 0.016 * std::atoi(tok.c_str() + 5);
            else if (tok.rfind("shot:", 0) == 0) menuShotPath = tok.substr(5);
            else if (!menu.script(tok)) std::fprintf(stderr, "unknown menu script step: %s\n", tok.c_str());
        }
        const bool paused = menu.isOpen();
        if (wasPaused && !paused) { wasPaused = false; padPrimed = false; suppressPad = true; t0 = glfwGetTime(); base = p.frames; }
        std::string err;
        if (paused) {
            // The game is frozen. The network keeps running (we keep announcing the state we froze in) so that the others do not drop us.
            wasPaused = true;
            glfwPollEvents();
            if (menuScript.empty()) {   // the real mouse (scripted tests move a pretend one)
                double cx = 0, cy = 0;
                int ww = 1, wh = 1;
                glfwGetCursorPos(win, &cx, &cy);
                glfwGetWindowSize(win, &ww, &wh);
                int vw = ww, vh = ww * 3 / 4;
                if (vh > wh) { vh = wh; vw = wh * 4 / 3; }
                const float ux = (float(cx) - float(ww - vw) * 0.5f) / float(std::max(1, vw)) * cb2ui::kW, uy = (float(cy) - float(wh - vh) * 0.5f) / float(std::max(1, vh)) * cb2ui::kH;
                menu.mouse(ux, uy, glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
                const bool right = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                if (right && !rightWas) menu.rightClick();
                rightWas = right;
            }
            GLFWgamepadstate gp;
            static bool padWas[5] = {};
            static bool allWas[17] = {};
            if (menuScript.empty() && glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp)) {
                // binding a button: any newly pressed button or trigger goes to the menu instead of moving in it
                bool allNow[17] = {};
                for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) allNow[b] = gp.buttons[b];
                allNow[15] = gp.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.5f;
                allNow[16] = gp.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.5f;
                const bool capturingPad = menu.capturingPad();
                if (capturingPad && padPrimed) {
                    for (int i = 0; i < 17; ++i)
                        if (allNow[i] && !allWas[i]) { menu.padButton(i < 15 ? i : cb2input::kPadAxisBase + (i == 15 ? GLFW_GAMEPAD_AXIS_LEFT_TRIGGER : GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)); break; }
                }
                for (int i = 0; i < 17; ++i) allWas[i] = allNow[i];
                const bool now[5] = {bool(gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]) || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.6f, bool(gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]) || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > 0.6f,
                                     bool(gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT]) || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X] < -0.6f, bool(gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]) || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_X] > 0.6f,
                                     bool(gp.buttons[GLFW_GAMEPAD_BUTTON_A])};
                static const int keysFor[5] = {GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT, GLFW_KEY_ENTER};
                static bool bWas = false;
                const bool b = gp.buttons[GLFW_GAMEPAD_BUTTON_B];
                // whatever is already held when the menu opens does not count as a press
                if (padPrimed && !capturingPad) {
                    for (int i = 0; i < 5; ++i) if (now[i] && !padWas[i]) menu.key(keysFor[i], GLFW_PRESS);
                    if (b && !bWas) menu.key(GLFW_KEY_ESCAPE, GLFW_PRESS);
                }
                for (int i = 0; i < 5; ++i) padWas[i] = now[i];
                bWas = b;
                padPrimed = true;
            }
            p.padButtons = 0xFFFF;
            mp.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        } else {
        // Emulation is paced against the wall clock at the console's 59.94 Hz so the monitor refresh does not matter.
        // Holding Tab runs unthrottled (fast-forward).
        const bool fast = glfwGetKey(win, GLFW_KEY_TAB) == GLFW_PRESS || (captureFrame != 0 && (!paceFrom || p.frames < paceFrom));
        const double due = fast ? 4.0 : (glfwGetTime() - t0) * 59.94 - double(p.frames - base);
        if (due < 1.0) {
            glfwPollEvents();
            p.padButtons = padNow();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (due > 8.0 || fast) { t0 = glfwGetTime(); base = p.frames; }
        for (int step = 0; step < std::min(int(due), 4) && err.empty(); ++step) {
            frameReady = false;
            while (!frameReady && err.empty()) err = p.run(20000);
        }
        }
        if (!err.empty()) { std::fprintf(stderr, "guest stopped: %s\n", err.c_str()); break; }

        if (saveStateFrame && p.frames >= saveStateFrame) {
            const auto r = doSave(saveStatePath);
            std::printf(r.first ? "state saved at frame %llu to %s\n" : "state NOT saved at frame %llu to %s\n", (unsigned long long)p.frames, saveStatePath.c_str());
            break;
        }
        mp.showTags = settings.nameTags;
        mp.opacity = settings.opacity;
        if (mp.active()) {
            roster.update(mp.others(), glfwGetTime(), rosterEvents);
            for (const cb2roster::Event& e : rosterEvents) {
                using K = cb2roster::EventKind;
                if (e.kind == K::Joined) hud.notice(e.name + " JOINED THE ROOM", cb2ui::kGreen, glfwGetTime());
                else if (e.kind == K::Left) hud.notice(e.name + " LEFT THE ROOM", cb2ui::kRed, glfwGetTime());
                else if (e.kind == K::EnteredLevel) hud.notice(e.name + " ENTERED " + cb2roster::levelName(e.level), cb2ui::kTeal, glfwGetTime());
                else hud.notice(e.name + " LEFT " + cb2roster::levelName(e.level) + "  " + cb2roster::formatTime(e.seconds), cb2ui::rgb(255, 176, 80), glfwGetTime());
            }
            rosterEvents.clear();
        }
        if (captureAfterPuppet && mp.puppetCount() > 0 && !puppetSince) puppetSince = p.frames;
        const bool captureNow = captureFrame && p.frames >= captureFrame && (!captureAfterPuppet || (puppetSince && p.frames >= puppetSince + captureAfterPuppet));
        if (!paused) {
            p.gpu.flushTags();
            if (useHires) hires.render(p.gpu);
        }
        const auto& g = p.gpu;
        const bool hiPath = showHires && g.displayEnabled && !g.color24;
        if (captureNow && !windowCapture) {
            std::vector<uint32_t> img;
            int iw = 0, ih = 0;
            if (useHires) {
                hires.readDisplay(g, img, iw, ih);
            }
            if (useHires) stbi_write_png(capturePath.c_str(), iw, ih, 4, img.data(), iw * 4);
            std::printf("captured frame %llu to %s (%dx%d)\n", (unsigned long long)p.frames, capturePath.c_str(), iw, ih);
            break;
        }
        for (int y = 0; y < (hiPath ? 0 : g.vres); ++y) {
            for (int x = 0; x < g.hres; ++x) {
                uint32_t c = 0xFF000000u;
                if (g.displayEnabled) {
                    if (g.color24) {
                        const int bx = (g.displayX * 2 + x * 3) / 2 * 1;  // 24-bit pixels are packed three bytes each
                        const uint8_t* row = reinterpret_cast<const uint8_t*>(&g.vram[size_t((g.displayY + y) & 511) * 1024]);
                        const size_t o = size_t(g.displayX) * 2 + size_t(x) * 3;
                        (void)bx;
                        c = 0xFF000000u | uint32_t(row[o + 2]) << 16 | uint32_t(row[o + 1]) << 8 | row[o];
                    } else {
                        const uint16_t px = g.vram[size_t((g.displayY + y) & 511) * 1024 + size_t((g.displayX + x) & 1023)];
                        c = 0xFF000000u | uint32_t(((px >> 10) & 31) << 3) << 16 | uint32_t(((px >> 5) & 31) << 3) << 8 | uint32_t((px & 31) << 3);
                    }
                }
                rgba[size_t(y) * 1024 + size_t(x)] = c;
            }
        }
        glBindTexture(GL_TEXTURE_2D, hiPath ? hires.colorTexture() : tex);
        if (!hiPath) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        int fw, fh;
        glfwGetFramebufferSize(win, &fw, &fh);
        glClear(GL_COLOR_BUFFER_BIT);
        // keep the console's 4:3 picture whatever the window shape
        int vw = fw, vh = fw * 3 / 4;
        if (vh > fh) { vh = fh; vw = fh * 4 / 3; }
        glViewport((fw - vw) / 2, (fh - vh) / 2, vw, vh);
        glUseProgram(prog);
        const float srcScale = hiPath ? float(hires.scale()) : 1.0f;
        glUniform4f(glGetUniformLocation(prog, "rect"), hiPath ? float(g.displayX) / 1024.0f : 0.0f, hiPath ? float(g.displayY) / 512.0f : 0.0f,
                    float(g.hres) / 1024.0f, float(g.vres) / 512.0f);
        glUniform2f(glGetUniformLocation(prog, "area"), float(g.hres) * srcScale, float(g.vres) * srcScale);
        glUniform1i(glGetUniformLocation(prog, "mode"), settings.filter);
        glUniform1f(glGetUniformLocation(prog, "prescale"), std::max(1.0f, std::ceil(float(vh) / (float(g.vres) * srcScale))));
        const GLint mag = (settings.filter == 0 && !hiPath) ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mag);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        menu.draw(glfwGetTime());
        if (mp.active() && !menu.isOpen()) {
            std::vector<cb2hud::Row> rows;
            cb2hud::Row self;
            self.name = mp.selfName();
            self.rgb = 0xFFD040;
            self.self = true;
            self.inLevel = mp.levelId() != 0xFF;
            self.where = self.inLevel ? cb2roster::levelName(int(mp.levelId())) : "MENU";
            if (self.inLevel) self.time = cb2roster::formatTime(mp.levelSeconds());
            rows.push_back(self);
            for (const cb2roster::Member& m : roster.list()) {
                cb2hud::Row r;
                r.name = m.name;
                r.rgb = cb2roster::playerRgb(m.id);
                r.inLevel = m.level != 0xFF;
                r.where = r.inLevel ? cb2roster::levelName(m.level) : "MENU";
                if (r.inLevel) r.time = cb2roster::formatTime(roster.levelTime(m, glfwGetTime()));
                else if (m.lastLevel != 0xFF) r.time = "LAST " + cb2roster::formatTime(m.lastTime);
                rows.push_back(r);
            }
            hud.draw(menu.surface(), rows, settings.roster, glfwGetTime());
        }
        {
            if (menu.isOpen() && !menuShotPath.empty()) { saveWindow(menuShotPath, fw, fh); std::printf("menu picture saved to %s\n", menuShotPath.c_str()); menuShotPath.clear(); }
        }
        const cb2menu::Result menuResult = menu.takeResult();
        if (menuResult == cb2menu::Result::Quit) quitRequested = true;
        else if (menuResult == cb2menu::Result::MainMenu) { quitRequested = true; relaunch = true; }
        if (menu.takeChanged()) saveSettings();
        if (captureNow && windowCapture) {
            std::vector<uint8_t> px(size_t(fw) * size_t(fh) * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            stbi_flip_vertically_on_write(1);
            stbi_write_png(capturePath.c_str(), fw, fh, 4, px.data(), fw * 4);
            std::printf("window captured at frame %llu to %s (%dx%d), path %s\n", (unsigned long long)p.frames, capturePath.c_str(), fw, fh, hiPath ? "opengl" : "software");
            break;
        }
        glfwSwapBuffers(win);
        glfwPollEvents();

        p.padButtons = menu.isOpen() ? uint16_t(0xFFFF) : padNow();
        auto edge = [&](int key, bool& was) { const bool down = glfwGetKey(win, key) == GLFW_PRESS; const bool hit = down && !was; was = down; return hit; };
        if (edge(GLFW_KEY_F1, f1Down)) { settings.filter = (settings.filter + 1) % 3; saveSettings(); }
        if (edge(GLFW_KEY_F2, f2Down) && useHires) { showHires = !showHires; p.gpu.rasterize = !showHires; }
        if (edge(GLFW_KEY_F11, f11Down)) toggleFullscreen();
        if (edge(GLFW_KEY_F3, f3Down) && mp.active()) { settings.roster = !settings.roster; saveSettings(); }
        if (!menu.isOpen()) {
            if (edge(GLFW_KEY_F6, f6Down)) { stateSlot = stateSlot % 4 + 1; menu.toast("STATE SLOT " + std::to_string(stateSlot), glfwGetTime()); }
            if (edge(GLFW_KEY_F5, f5Down)) {
                const auto r = doSave(slotPath(stateSlot));
                menu.toast(r.first ? "STATE " + std::to_string(stateSlot) + " SAVED" : "CANNOT SAVE: " + r.second, glfwGetTime());
            }
            if (edge(GLFW_KEY_F9, f9Down)) {
                const auto r = doLoad(slotPath(stateSlot));
                menu.toast(r.first ? "STATE " + std::to_string(stateSlot) + " LOADED" : "CANNOT LOAD: " + r.second, glfwGetTime());
            }
        }
        if (glfwGetKey(win, GLFW_KEY_F12) == GLFW_PRESS) {
            char name[64];
            std::snprintf(name, sizeof name, "cb2_shot_%03d.png", shot++);
            stbi_write_png(name, 1024, 512, 4, rgba.data(), 1024 * 4);
        }
        if (p.frames - lastTitle >= 60) {
            lastTitle = p.frames;
            char t[128];
            if (mp.active()) std::snprintf(t, sizeof t, "Crash Bandicoot 2 - online: %s", mp.status().c_str());
            else std::snprintf(t, sizeof t, "Crash Bandicoot 2 (native port) - frame %llu", (unsigned long long)p.frames);
            glfwSetWindowTitle(win, t);
        }
    }
    if (relaunch) {
        mp.stop();   // frees the network port for the new copy
        glfwTerminate();
        cb2::relaunchToLobby(argc, argv);
        return 0;
    }
    glfwTerminate();
    return 0;
}
