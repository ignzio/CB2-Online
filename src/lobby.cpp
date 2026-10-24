#include "lobby.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include "multiplayer.hpp"
#include "net.hpp"
#include "stb_image_write.h"
#include "ui.hpp"

namespace cb2lobby {
namespace {

using namespace cb2ui;

struct Event {
    enum Type { Up, Down, Enter, Esc, Back, Char } type;
    char c = 0;
};

std::vector<Event>* gQueue = nullptr;

void onKey(GLFWwindow*, int key, int, int action, int) {
    if (!gQueue || action == GLFW_RELEASE) return;
    switch (key) {
    case GLFW_KEY_UP: gQueue->push_back({Event::Up}); break;
    case GLFW_KEY_DOWN: gQueue->push_back({Event::Down}); break;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: if (action == GLFW_PRESS) gQueue->push_back({Event::Enter}); break;
    case GLFW_KEY_ESCAPE: if (action == GLFW_PRESS) gQueue->push_back({Event::Esc}); break;
    case GLFW_KEY_BACKSPACE: gQueue->push_back({Event::Back}); break;
    default: break;
    }
}

void onChar(GLFWwindow*, unsigned int cp) {
    if (gQueue && cp >= 32 && cp < 127) gQueue->push_back({Event::Char, char(cp)});
}

std::string levelText(unsigned level) {
    if (level == 0xFF) return "IN THE MENU";
    char b[24];
    std::snprintf(b, sizeof b, "LEVEL %02X", level);
    return b;
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.'; }

enum class Screen { Main, Rooms, Waiting };
enum Item { kOffline, kLocal, kOnline, kName, kQuit, kItems };

// One line of the room list: the "create" row, a room, or a dedicated server from the lobby directory.
struct Row {
    enum Kind { Create, Room, Server } kind = Room;
    std::string name;
    int players = 0;
    std::string rendezvous;                  // room: the server it lives on (from the directory)
    std::vector<cb2net::Endpoint> endpoints; // room: players to contact (from the directory, for rooms without a server)
    bool listed = false;                     // the directory knows this room
    std::string serverText;                  // server: "ip:port"
};

std::string upperKey(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Outcome run(GLFWwindow* win, cb2mp::Multiplayer& mp, cb2mp::Options& options, const Script& script) {
    Canvas canvas;
    if (!canvas.init()) return Outcome::Offline;

    std::vector<Event> queue;
    gQueue = &queue;
    glfwSetKeyCallback(win, onKey);
    glfwSetCharCallback(win, onChar);
    struct Restore {
        GLFWwindow* w;
        ~Restore() { glfwSetKeyCallback(w, nullptr); glfwSetCharCallback(w, nullptr); gQueue = nullptr; }
    } restore{win};

    std::string name = options.name;
    if (name.empty()) { const char* u = std::getenv("USERNAME"); name = u && *u ? u : "CRASH"; }
    name = name.substr(0, cb2net::kNameLength - 1);
    std::string server = options.rendezvous, directoryUrl = options.directory, room = options.room.empty() ? "PUBLIC" : options.room, newRoom;
    Screen screen = Screen::Main;
    int selected = kOnline, editing = -1, roomSel = 0, roomTop = 0;
    bool creating = false, localMode = false;   // localMode: only this PC and the local network (no lobby service)
    std::string message;
    double stagingSince = 0;
    size_t scriptPos = 0;
    int scriptWait = 0, shots = 0;
    bool wantShot = false;
    bool padUp = false, padDown = false, padA = false, padB = false;
    cb2net::RoomBrowser browser;
    cb2net::DirectoryClient dirBrowse;
    const auto t0 = std::chrono::steady_clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    auto openRooms = [&](bool local) {
        localMode = local;
        browser.start(local ? std::string() : server);
        dirBrowse.stop();
        if (!local && !directoryUrl.empty() && dirBrowse.start(directoryUrl)) dirBrowse.wantList(true);
        screen = Screen::Rooms;
        roomSel = 0;
        roomTop = 0;
        creating = false;
        message.clear();
    };
    auto enterRoom = [&](const std::string& which, const Row* row) {
        options.name = name;
        options.online = true;
        options.room = which.empty() ? "PUBLIC" : which;
        options.rendezvous = localMode ? std::string() : server;
        options.directory = localMode ? std::string() : directoryUrl;
        options.seeds.clear();
        options.joinViaDirectory = false;
        if (localMode) {}
        else if (row && !row->rendezvous.empty()) options.rendezvous = row->rendezvous;      // the room lives on a server: go through it
        else if (row && row->listed && !row->endpoints.empty() && !directoryUrl.empty()) {   // a player's room: contact the members, hand-shake through the directory
            for (const auto& e : row->endpoints) options.seeds.push_back(cb2net::endpointText(e));
            options.joinViaDirectory = true;
        }
        room = options.room;
        std::string err;
        if (mp.start(options, err)) { browser.close(); dirBrowse.stop(); screen = Screen::Waiting; stagingSince = seconds(); message.clear(); }
        else message = err;
    };
    // The room list: the rooms heard of on this PC / network / server, the rooms in the directory, and the directory's dedicated servers.
    auto buildRows = [&] {
        std::vector<Row> rows;
        Row create;
        create.kind = Row::Create;
        rows.push_back(create);
        std::vector<Row> rooms;
        auto find = [&](const std::string& n) -> Row* { for (Row& r : rooms) if (upperKey(r.name) == upperKey(n)) return &r; return nullptr; };
        for (const auto& r : browser.rooms()) { Row row; row.name = r.name; row.players = r.players; rooms.push_back(row); }
        for (const auto& r : dirBrowse.rooms()) {
            Row* row = find(r.name);
            if (!row) { rooms.emplace_back(); row = &rooms.back(); row->name = r.name; }
            row->players = std::max(row->players, r.players);
            row->rendezvous = r.rendezvous;
            row->endpoints = r.endpoints;
            row->listed = true;
        }
        std::stable_sort(rooms.begin(), rooms.end(), [](const Row& a, const Row& b) { return a.players > b.players; });
        rows.insert(rows.end(), rooms.begin(), rooms.end());
        for (const auto& s : dirBrowse.servers()) { Row row; row.kind = Row::Server; row.name = s.name; row.players = s.players; row.serverText = s.text; rows.push_back(row); }
        return rows;
    };

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        // game pad: D-pad up/down, A or Start = Enter, B = Esc
        GLFWgamepadstate gp;
        if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &gp)) {
            const bool up = gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] < -0.6f;
            const bool down = gp.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] || gp.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] > 0.6f;
            const bool a = gp.buttons[GLFW_GAMEPAD_BUTTON_A] || gp.buttons[GLFW_GAMEPAD_BUTTON_START], b = gp.buttons[GLFW_GAMEPAD_BUTTON_B];
            if (up && !padUp) queue.push_back({Event::Up});
            if (down && !padDown) queue.push_back({Event::Down});
            if (a && !padA) queue.push_back({Event::Enter});
            if (b && !padB) queue.push_back({Event::Esc});
            padUp = up; padDown = down; padA = a; padB = b;
        }
        // scripted input (tests)
        if (scriptWait > 0) --scriptWait;
        else if (scriptPos < script.tokens.size()) {
            const std::string& tok = script.tokens[scriptPos++];
            if (tok == "up") queue.push_back({Event::Up});
            else if (tok == "down") queue.push_back({Event::Down});
            else if (tok == "enter") queue.push_back({Event::Enter});
            else if (tok == "esc") queue.push_back({Event::Esc});
            else if (tok == "back") queue.push_back({Event::Back});
            else if (tok == "shot") wantShot = true;
            else if (tok == "exit") return Outcome::Quit;
            else if (tok.rfind("wait:", 0) == 0) scriptWait = std::atoi(tok.c_str() + 5);
            else if (tok.rfind("text:", 0) == 0) for (char c : tok.substr(5)) queue.push_back({Event::Char, c});
        }

        std::vector<Row> rows;
        if (screen == Screen::Rooms) { browser.update(seconds()); rows = buildRows(); }
        const int rowCount = int(rows.size());   // the first row creates a room

        for (const Event& ev : queue) {
            if (screen == Screen::Waiting) {
                if (ev.type == Event::Enter) { options.name = name; return Outcome::Online; }
                if (ev.type == Event::Esc) { mp.stop(); openRooms(localMode); }
                continue;
            }
            if (screen == Screen::Rooms && creating) {   // typing the name of a new room
                if (ev.type == Event::Char) { if (nameChar(ev.c) && newRoom.size() < size_t(cb2net::kNameLength - 1)) newRoom += ev.c; }
                else if (ev.type == Event::Back) { if (!newRoom.empty()) newRoom.pop_back(); }
                else if (ev.type == Event::Esc) creating = false;
                else if (ev.type == Event::Enter) {
                    while (!newRoom.empty() && newRoom.back() == ' ') newRoom.pop_back();
                    if (newRoom.empty()) message = "GIVE THE ROOM A NAME";
                    else { creating = false; enterRoom(newRoom, nullptr); }
                }
                continue;
            }
            if (screen == Screen::Rooms) {
                if (ev.type == Event::Up) roomSel = (roomSel + rowCount - 1) % rowCount;
                else if (ev.type == Event::Down) roomSel = (roomSel + 1) % rowCount;
                else if (ev.type == Event::Esc) { browser.close(); screen = Screen::Main; message.clear(); }
                else if (ev.type == Event::Enter) {
                    if (roomSel == 0) {
                        std::mt19937 rng{std::random_device{}()};
                        newRoom = "ROOM " + std::to_string(1000 + rng() % 9000);
                        creating = true;
                        message.clear();
                    } else if (roomSel < int(rows.size()) && rows[size_t(roomSel)].kind == Row::Server) {
                        // a dedicated server: join its room (everybody who picks the server meets there); it also relays for strict routers
                        Row row = rows[size_t(roomSel)];
                        row.rendezvous = row.serverText;
                        enterRoom(row.name, &row);
                    } else if (roomSel < int(rows.size())) {
                        const Row row = rows[size_t(roomSel)];
                        enterRoom(row.name, &row);
                    }
                }
                continue;
            }
            // main screen
            if (editing >= 0) {
                std::string& field = name;
                if (ev.type == Event::Char) { if (nameChar(ev.c) && field.size() < size_t(cb2net::kNameLength - 1)) field += ev.c; }
                else if (ev.type == Event::Back) { if (!field.empty()) field.pop_back(); }
                else if (ev.type == Event::Enter || ev.type == Event::Esc) editing = -1;
                else if (ev.type == Event::Up || ev.type == Event::Down) { editing = -1; selected = (selected + (ev.type == Event::Down ? 1 : kItems - 1)) % kItems; }
                continue;
            }
            if (ev.type == Event::Up) { selected = (selected + kItems - 1) % kItems; message.clear(); }
            else if (ev.type == Event::Down) { selected = (selected + 1) % kItems; message.clear(); }
            else if (ev.type == Event::Esc) return Outcome::Quit;
            else if (ev.type == Event::Enter) {
                message.clear();
                switch (selected) {
                case kOffline: return Outcome::Offline;
                case kLocal: openRooms(true); break;
                case kOnline: openRooms(false); break;
                case kName: editing = selected; break;
                case kQuit: return Outcome::Quit;
                }
            }
        }
        queue.clear();
        if (screen == Screen::Waiting) mp.tick();
        if (screen == Screen::Rooms) {
            roomSel = std::min(roomSel, std::max(0, rowCount - 1));
            if (roomSel < roomTop) roomTop = roomSel;
            if (roomSel >= roomTop + 5) roomTop = roomSel - 4;
        }

        // ---- draw ----
        int fw, fh;
        glfwGetFramebufferSize(win, &fw, &fh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fw, fh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        int vw = fw, vh = fw * 3 / 4;
        if (vh > fh) { vh = fh; vw = fh * 4 / 3; }
        glViewport((fw - vw) / 2, (fh - vh) / 2, vw, vh);

        const double now = seconds();
        const bool blink = int(now * 2.0) % 2 == 0;
        background(canvas, now);

        if (screen == Screen::Main) {
            title(canvas, "PLAY");
            const char* labels[kItems] = {"PLAY OFFLINE", "PLAY LOCAL", "PLAY ONLINE", "NAME", "QUIT"};
            const char* hints[kItems] = {"JUST THE GAME, LIKE ALWAYS", "FRIENDS ON THIS PC OR YOUR LOCAL NETWORK", "ROOMS AND SERVERS FROM EVERYWHERE - NOTHING TO TYPE", "SHOWN OVER YOUR CRASH", ""};
            float y = 150;
            for (int i = 0; i < kItems; ++i, y += 52) {
                const bool sel = i == selected;
                plank(canvas, 130, y, 380, 42, sel);
                const std::string label = labels[i];
                if (i == kName) {
                    const bool edit = editing == i;
                    std::string shown = name + (edit && blink ? "_" : "");
                    canvas.text(label, 146, y + 6, 26, sel ? kGold1 : kWhite1, sel ? kGold2 : kWhite2);
                    const float w = canvas.width(shown, 24);
                    canvas.text(shown, 496 - w, y + 8, 24, edit ? kGreen : kWhite1);
                } else {
                    canvas.text(label, 320 - canvas.width(label, 30) * 0.5f, y + 5, 30, sel ? kGold1 : kWhite1, sel ? kGold2 : kWhite2);
                }
            }
            if (editing < 0 && hints[selected][0]) canvas.centred(hints[selected], 418, 16, kDim);
            if (!message.empty()) canvas.centred(message, 394, 22, kRed);
            canvas.centred(editing >= 0 ? "TYPE.  BACKSPACE DELETES.  ENTER WHEN DONE" : "UP DOWN  SELECT      ENTER  CHOOSE      ESC  QUIT", 446, 18, kDim);
        } else if (screen == Screen::Rooms) {
            title(canvas, localMode ? "LOCAL ROOMS" : "ONLINE ROOMS");
            if (localMode) {
                canvas.centred("THIS PC AND YOUR LOCAL NETWORK", 134, 16, kDim);
            } else if (directoryUrl.empty()) {
                canvas.centred("NO LOBBY SERVICE IN THIS BUILD - SHOWING LOCAL ROOMS ONLY", 134, 16, kRed);
            } else {
                const bool dirOk = dirBrowse.reachable();
                canvas.centred(dirOk ? "CONNECTED" : "CONNECTING...", 134, 16, dirOk ? kGreen : kRed);
            }
            float y = 172;
            if (creating) {
                plank(canvas, 120, 200, 400, 56, true);
                canvas.centred("NAME OF THE NEW ROOM", 166, 22, kTeal);
                const std::string shown = newRoom + (blink ? "_" : "");
                canvas.centred(shown, 208, 34, kGold1, kGold2);
                canvas.centred("ENTER  CREATE      ESC  CANCEL", 300, 20, kDim);
                if (!message.empty()) canvas.centred(message, 340, 22, kRed);
            } else {
                for (int row = roomTop; row < std::min(rowCount, roomTop + 5); ++row, y += 44) {
                    const bool sel = row == roomSel;
                    const Row& r = rows[size_t(row)];
                    plank(canvas, 130, y, 380, 38, sel);
                    if (r.kind == Row::Create) {
                        canvas.text("+ CREATE A ROOM", 320 - canvas.width("+ CREATE A ROOM", 26) * 0.5f, y + 5, 26, sel ? kGold1 : kGreen, sel ? kGold2 : kGreen);
                    } else if (r.kind == Row::Server) {
                        canvas.text(upper(r.name), 146, y + 5, 26, sel ? kGold1 : kGreen, sel ? kGold2 : kGreen);
                        const std::string tag = "SERVER " + std::to_string(r.players);
                        canvas.text(tag, 494 - canvas.width(tag, 20), y + 9, 20, sel ? rgb(255, 244, 200) : kGreen);
                    } else {
                        canvas.text(upper(r.name), 146, y + 5, 26, sel ? kGold1 : kWhite1, sel ? kGold2 : kWhite2);
                        const std::string count = std::to_string(r.players) + (r.players == 1 ? " PLAYER" : " PLAYERS");
                        canvas.text(count, 494 - canvas.width(count, 22), y + 8, 22, sel ? rgb(255, 244, 200) : kTeal);
                    }
                }
                if (rowCount <= 1) canvas.centred(localMode ? "NO LOCAL ROOMS YET - CREATE ONE" : "NO ROOMS YET - BE THE FIRST, CREATE ONE", y + 4, 17, kDim);
                if (roomTop > 0) canvas.triangle(320, 166, 308, 178, 332, 178, kGold1);
                if (rowCount > roomTop + 5) canvas.triangle(320, y + 6, 308, y - 6, 332, y - 6, kGold1);
                if (!message.empty()) canvas.centred(message, 410, 22, kRed);
                canvas.centred("UP DOWN  SELECT      ENTER  JOIN      ESC  BACK", 446, 18, kDim);
            }
        } else {
            title(canvas, "WAITING ROOM");
            const cb2net::Session& net = mp.session();
            canvas.centred(upper(room), 134, 34, kGold1, kGold2);
            if (!net.directoryEnabled() && net.serverState() == 0) {
                canvas.centred("THIS PC AND LOCAL NETWORK", 176, 16, kDim);
            } else if (net.serverState() == 0) {
                canvas.centred("", 176, 16, kDim);
            } else {
                const bool up = net.serverState() == 2;
                std::string st = up ? "SERVER CONNECTED" : "CONNECTING TO THE SERVER...";
                if (!up && seconds() - stagingSince > 6.0) st = "SERVER DOES NOT ANSWER - LOCAL PLAYERS STILL WORK";
                canvas.centred(st, 176, 18, up ? kGreen : kRed);
            }
            if (net.directoryEnabled()) {
                const bool listed = net.directoryReachable() && net.publicEndpoint().valid();
                canvas.centred(listed ? "YOUR ROOM IS LISTED - OTHERS CAN JOIN IT" : (net.directoryReachable() ? "FINDING YOUR PUBLIC ADDRESS..." : "CONNECTING..."), 176, 16, listed ? kGreen : kDim);
            }
            plank(canvas, 130, 206, 380, 192, false);
            float y = 214;
            canvas.text(upper(name), 150, y, 24, kGold1, kGold2);
            canvas.text("YOU", 490 - canvas.width("YOU", 20), y + 3, 20, kTeal);
            y += 30;
            int shown = 0;
            for (const cb2net::PlayerState& s : mp.others()) {
                if (shown++ >= 5) break;
                char nm[cb2net::kNameLength + 1] = {};
                std::copy(s.name, s.name + cb2net::kNameLength, nm);
                canvas.text(upper(nm[0] ? nm : "?"), 150, y, 24, kWhite1, kWhite2);
                const std::string lv = levelText(s.level);
                canvas.text(lv, 490 - canvas.width(lv, 20), y + 3, 20, kTeal);
                y += 30;
            }
            if (shown == 0) canvas.text("WAITING FOR OTHERS...", 150, y + 6, 20, kDim, false);
            canvas.centred("NO HOST - LEAVE AND COME BACK ANY TIME", 410, 17, kDim);
            canvas.centred("ENTER  START THE GAME      ESC  BACK", 446, 18, kDim);
        }
        canvas.flush();

        if (wantShot && !script.shotPrefix.empty()) {
            std::vector<uint8_t> px(size_t(fw) * size_t(fh) * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            stbi_flip_vertically_on_write(1);
            const std::string file = script.shotPrefix + "_" + std::to_string(shots++) + ".png";
            stbi_write_png(file.c_str(), fw, fh, 4, px.data(), fw * 4);
            std::printf("lobby picture saved to %s\n", file.c_str());
        }
        wantShot = false;
        glfwSwapBuffers(win);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return Outcome::Quit;
}

}  // namespace cb2lobby
