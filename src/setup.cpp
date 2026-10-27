#include "setup.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include "discimport.hpp"
#include "stb_image_write.h"
#include "ui.hpp"

namespace cb2setup {
namespace {

namespace fs = std::filesystem;
using namespace cb2ui;

struct Event {
    enum Type { Up, Down, Enter, Esc, Click, Drop } type;
    float x = 0, y = 0;   // Click: window coordinates
    std::string text;     // Drop: the file
};
std::vector<Event>* gQueue = nullptr;

void onKey(GLFWwindow*, int key, int, int action, int) {
    if (!gQueue || action != GLFW_PRESS) return;
    switch (key) {
    case GLFW_KEY_UP: gQueue->push_back({Event::Up}); break;
    case GLFW_KEY_DOWN: gQueue->push_back({Event::Down}); break;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: gQueue->push_back({Event::Enter}); break;
    case GLFW_KEY_ESCAPE: gQueue->push_back({Event::Esc}); break;
    default: break;
    }
}
void onMouse(GLFWwindow* w, int button, int action, int) {
    if (!gQueue || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    Event e{Event::Click};
    e.x = float(x);
    e.y = float(y);
    gQueue->push_back(e);
}
void onDrop(GLFWwindow*, int count, const char** paths) {
    if (!gQueue || count < 1) return;
    Event e{Event::Drop};
    e.text = paths[0];
    gQueue->push_back(e);
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// The text that fits: the end of a long path, with "..." in front.
std::string tail(const Canvas& c, const std::string& s, float size, float maxWidth) {
    if (c.width(s, size) <= maxWidth) return s;
    std::string t = s;
    while (t.size() > 4 && c.width("..." + t, size) > maxWidth) t.erase(0, 1);
    return "..." + t;
}

std::vector<std::string> wrap(const Canvas& c, const std::string& text, float size, float maxWidth) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (!line.empty() && c.width(tryLine, size) > maxWidth) { lines.push_back(line); line = word; }
        else line = tryLine;
        word.clear();
    };
    for (char ch : text) { if (ch == ' ') flush(); else word += ch; }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

enum class Step { Release, Choose, Working, Done, Failed };

// The copying runs in a thread; the window keeps drawing its progress.
struct Job {
    std::atomic<uint64_t> done{0}, total{1};
    std::atomic<bool> finished{false};
    cb2import::Result result;
    std::thread th;
    ~Job() { if (th.joinable()) th.join(); }
};

}  // namespace

Result run(const std::string& exeDir, bool firstRun, bool hidden, const Script& script) {
    Result result;
    if (!glfwInit()) return result;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    if (hidden) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(960, 720, "Crash Bandicoot 2 - game files", nullptr, nullptr);
    if (!win) { glfwTerminate(); return result; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!gladLoadGL(glfwGetProcAddress)) { glfwDestroyWindow(win); glfwTerminate(); return result; }

    {
        Canvas canvas;
        if (canvas.init()) {
            std::vector<Event> queue;
            gQueue = &queue;
            glfwSetKeyCallback(win, onKey);
            glfwSetMouseButtonCallback(win, onMouse);
            glfwSetDropCallback(win, onDrop);

            const fs::path exeFolder = fs::u8path(exeDir);
            const std::string dataDirNtsc = (exeFolder / cb2game::ntsc().folderName).u8string(), dataDirPal = (exeFolder / cb2game::pal().folderName).u8string();
            auto installed = [&](const cb2game::Profile& g) {
                std::error_code ec;
                return fs::exists(exeFolder / g.folderName / g.exeName, ec);
            };

            Step step = Step::Release;
            int releaseSel = 0;   // 0 European, 1 American
            const cb2game::Profile* chosen = &cb2game::pal();
            std::string message, imageName, failure;
            bool openDialog = false;   // the Windows file window is opened after the screen behind it has been drawn once
            Job job;
            auto beginImport = [&](const fs::path& image) {
                imageName = image.filename().u8string();
                message.clear();
                job.done = 0;
                job.total = 1;
                job.finished = false;
                if (job.th.joinable()) job.th.join();
                const std::string img = image.u8string();
                job.th = std::thread([&job, img, chosen, dataDirNtsc, dataDirPal] {
                    job.result = cb2import::importDisc(img, dataDirNtsc, [&job](uint64_t d, uint64_t t) { job.done = d; job.total = t ? t : 1; }, dataDirPal, chosen);
                    job.finished = true;
                });
                step = Step::Working;
            };
            // the Windows "open file" window, on top of the game's window
            auto askForImage = [&] {
                void* owner = nullptr;
#ifdef _WIN32
                owner = glfwGetWin32Window(win);
#endif
                std::string start = exeDir;
                if (const char* h = std::getenv("USERPROFILE")) {
                    std::error_code ec;
                    const fs::path downloads = fs::u8path(h) / "Downloads";
                    if (fs::is_directory(downloads, ec)) start = downloads.u8string();
                }
                const std::string title = std::string("Choose the disc image of your ") + (chosen == &cb2game::pal() ? "European (PAL)" : "American (NTSC-U)") + " Crash Bandicoot 2";
                const std::string picked = cb2import::pickImage(start, owner, title);
                if (picked.empty()) message = "NO FILE CHOSEN";
                else beginImport(fs::u8path(picked));
            };

            size_t scriptPos = 0;
            int scriptWait = 0, shots = 0;
            bool wantShot = false, leave = false, padUp = false, padDown = false, padA = false, padB = false;
            const auto t0 = std::chrono::steady_clock::now();
            auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

            while (!glfwWindowShouldClose(win) && !leave) {
                glfwPollEvents();
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
                if (scriptWait > 0) --scriptWait;
                else if (scriptPos < script.tokens.size()) {
                    const std::string& tok = script.tokens[scriptPos++];
                    if (tok == "up") queue.push_back({Event::Up});
                    else if (tok == "down") queue.push_back({Event::Down});
                    else if (tok == "enter") queue.push_back({Event::Enter});
                    else if (tok == "esc") queue.push_back({Event::Esc});
                    else if (tok == "shot") wantShot = true;
                    else if (tok == "exit") leave = true;
                    else if (tok.rfind("wait:", 0) == 0) scriptWait = std::atoi(tok.c_str() + 5);
                    else if (tok.rfind("drop:", 0) == 0) { Event e{Event::Drop}; e.text = tok.substr(5); queue.push_back(e); }   // same as choosing that file in the Windows window
                }

                // window coordinates -> the menu's 640 x 480
                int ww = 1, wh = 1, fw = 1, fh = 1;
                glfwGetWindowSize(win, &ww, &wh);
                glfwGetFramebufferSize(win, &fw, &fh);
                int vw = fw, vh = fw * 3 / 4;
                if (vh > fh) { vh = fh; vw = fh * 4 / 3; }
                const float vx = float(fw - vw) * 0.5f, vy = float(fh - vh) * 0.5f;
                auto toMenuX = [&](float x) { return (x * float(fw) / float(std::max(ww, 1)) - vx) / float(vw) * kW; };
                auto toMenuY = [&](float y) { return (y * float(fh) / float(std::max(wh, 1)) - vy) / float(vh) * kH; };

                auto confirmRelease = [&] {
                    chosen = releaseSel == 0 ? &cb2game::pal() : &cb2game::ntsc();
                    step = Step::Choose;
                    message.clear();
                    openDialog = !hidden && script.tokens.empty();
                };
                auto goBack = [&] {
                    if (step == Step::Choose) { step = Step::Release; message.clear(); }
                    else if (step == Step::Failed) { step = Step::Choose; message.clear(); }
                    else if (step == Step::Release) leave = true;
                };
                for (const Event& ev : queue) {
                    if (step == Step::Working) continue;   // nothing to do while copying
                    if (ev.type == Event::Drop) {
                        const fs::path dropped = fs::u8path(ev.text);
                        std::error_code ec;
                        if (step != Step::Release && cb2import::looksLikeImage(ev.text) && fs::is_regular_file(dropped, ec)) beginImport(dropped);
                        else if (step == Step::Choose) message = "THAT IS NOT A DISC IMAGE (.CUE  .BIN  .ISO)";
                        continue;
                    }
                    if (step == Step::Release) {
                        if (ev.type == Event::Up || ev.type == Event::Down) releaseSel = 1 - releaseSel;
                        else if (ev.type == Event::Enter) confirmRelease();
                        else if (ev.type == Event::Esc) goBack();
                        else if (ev.type == Event::Click) {
                            const float x = toMenuX(ev.x), y = toMenuY(ev.y);
                            for (int i = 0; i < 2; ++i) if (x >= 127 && x <= 513 && y >= 193 + 54 * i && y <= 239 + 54 * i) { releaseSel = i; confirmRelease(); }
                        }
                    } else if (step == Step::Choose) {
                        if (ev.type == Event::Enter) openDialog = !hidden;
                        else if (ev.type == Event::Esc) goBack();
                        else if (ev.type == Event::Click) {
                            const float x = toMenuX(ev.x), y = toMenuY(ev.y);
                            if (x >= 127 && x <= 513 && y >= 253 && y <= 299) openDialog = !hidden;
                        }
                    } else if (step == Step::Done) {
                        if (ev.type == Event::Enter || ev.type == Event::Click) leave = true;
                    } else if (step == Step::Failed) {
                        if (ev.type == Event::Enter || ev.type == Event::Click) { step = Step::Choose; message.clear(); openDialog = !hidden && script.tokens.empty(); }
                        else if (ev.type == Event::Esc) goBack();
                    }
                }
                queue.clear();

                if (step == Step::Working && job.finished) {
                    if (job.th.joinable()) job.th.join();
                    if (job.result.ok) {
                        result.imported = true;
                        result.profile = job.result.profile;
                        result.folder = job.result.folder;
                        step = Step::Done;
                    } else {
                        failure = job.result.error;
                        step = Step::Failed;
                    }
                }

                // ---- draw ----
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, fw, fh);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glViewport(int(vx), int(vy), vw, vh);
                const double now = seconds();
                background(canvas, now);
                const std::string chosenName = chosen == &cb2game::pal() ? "EUROPEAN" : "AMERICAN";

                if (step == Step::Release) {
                    title(canvas, "GAME FILES");
                    canvas.centred("THE GAME ITSELF IS NOT INCLUDED: IT IS COPIED ONCE FROM YOUR OWN DISC", 134, 15, kDim);
                    canvas.centred("WHICH RELEASE OF THE GAME DO YOU HAVE?", 156, 24, kTeal);
                    struct Opt { const char* label; const cb2game::Profile* g; };
                    const Opt opts[2] = {{"EUROPEAN  PAL", &cb2game::pal()}, {"AMERICAN  NTSC-U", &cb2game::ntsc()}};
                    for (int i = 0; i < 2; ++i) {
                        const float y = 196.0f + 54.0f * float(i);
                        const bool on = i == releaseSel;
                        plank(canvas, 130, y, 380, 40, on);
                        canvas.text(opts[i].label, 146, y + 6, 26, on ? kGold1 : kWhite1, on ? kGold2 : kWhite2);
                        if (installed(*opts[i].g)) { const std::string t = "INSTALLED"; canvas.text(t, 494 - canvas.width(t, 16), y + 12, 16, kGreen); }
                    }
                    canvas.rect(56, 312, 528, 120, rgb(0, 6, 12), 0.5f);
                    const bool pal = releaseSel == 0;
                    canvas.centred(pal ? "EUROPEAN RELEASE (PAL, DISC CODE SCES-00967)" : "AMERICAN RELEASE (NTSC-U, DISC CODE SCUS-94154)", 318, 17, kGold1, kGold2);
                    const std::string body = pal
                        ? "CHOOSE THIS IF YOUR DISC IS THE EUROPEAN ONE. YOU PLAY THE EUROPEAN GAME AND MEET ONLY PLAYERS OF THE EUROPEAN RELEASE ONLINE. PLAYERS OF THE AMERICAN RELEASE ARE NOT IN YOUR ROOMS."
                        : "CHOOSE THIS IF YOUR DISC IS THE AMERICAN ONE. YOU PLAY THE AMERICAN GAME AND MEET ONLY PLAYERS OF THE AMERICAN RELEASE ONLINE. PLAYERS OF THE EUROPEAN RELEASE ARE NOT IN YOUR ROOMS.";
                    float ty = 342;
                    for (const std::string& l : wrap(canvas, body, 15, 500)) { canvas.centred(l, ty, 15, kWhite1, kWhite2); ty += 19; }
                    canvas.centred("SAVES, RECORDS AND SAVE STATES ARE KEPT APART. YOU CAN ADD THE OTHER RELEASE LATER.", 410, 13, kDim);
                    canvas.centred(firstRun ? "UP DOWN  SELECT      ENTER  CHOOSE      ESC  QUIT" : "UP DOWN  SELECT      ENTER  CHOOSE      ESC  BACK", 446, 18, kDim);
                } else if (step == Step::Choose) {
                    title(canvas, chosenName + " DISC IMAGE");
                    float ty = 140;
                    const std::string how = "A WINDOWS WINDOW OPENS: PICK THE .CUE (OR .BIN / .ISO) FILE OF YOUR " + chosenName + " DISC.";
                    for (const std::string& l : wrap(canvas, how, 18, 520)) { canvas.centred(l, ty, 18, kWhite1, kWhite2); ty += 24; }
                    canvas.centred(chosen == &cb2game::pal() ? "IT MUST BE THE EUROPEAN DISC (SCES-00967)." : "IT MUST BE THE AMERICAN DISC (SCUS-94154).", ty + 4, 15, kDim);
                    plank(canvas, 130, 256, 380, 40, true);
                    canvas.centred("CHOOSE THE DISC IMAGE", 262, 26, kGold1, kGold2);
                    canvas.centred("OR DROP THE FILE ON THIS WINDOW", 316, 16, kDim);
                    if (!message.empty()) canvas.centred(message, 362, 20, kRed);
                    canvas.centred("ENTER  OPEN THE FILE WINDOW      ESC  BACK", 446, 18, kDim);
                } else if (step == Step::Working) {
                    title(canvas, "COPYING");
                    canvas.centred(tail(canvas, upper(imageName), 18, 520), 150, 18, kTeal);
                    canvas.centred("COPYING THE GAME FILES FROM YOUR DISC IMAGE", 190, 20, kWhite1, kWhite2);
                    const float frac = float(job.done.load()) / float(std::max<uint64_t>(1, job.total.load()));
                    plank(canvas, 120, 236, 400, 34, false);
                    canvas.rect(124, 240, 392, 26, rgb(0, 0, 0), 0.6f);
                    canvas.rect(124, 240, 392 * std::min(1.0f, frac), 26, rgb(255, 232, 110), rgb(255, 140, 24));
                    const std::string pct = std::to_string(int(std::min(1.0f, frac) * 100.0f)) + "%";
                    canvas.centred(pct, 292, 34, kGold1, kGold2);
                    canvas.centred("THIS TAKES ABOUT A MINUTE. YOU DO IT ONLY ONCE.", 352, 16, kDim);
                } else if (step == Step::Done) {
                    title(canvas, "READY");
                    const std::string name = result.profile == &cb2game::pal() ? "EUROPEAN" : "AMERICAN";
                    canvas.centred("THE " + name + " RELEASE IS READY", 150, 30, kGold1, kGold2);
                    canvas.centred(tail(canvas, upper(result.folder), 15, 540), 206, 15, kTeal);
                    canvas.centred("YOU DO NOT NEED THE DISC IMAGE ANY MORE.", 244, 18, kWhite1, kWhite2);
                    const bool other = result.profile == &cb2game::pal() ? !installed(cb2game::ntsc()) : !installed(cb2game::pal());
                    if (other) canvas.centred("TO ADD THE OTHER RELEASE LATER, START THE GAME WITH  --SETUP.", 290, 14, kDim);
                    plank(canvas, 190, 340, 260, 40, true);
                    canvas.centred(firstRun ? "START THE GAME" : "CONTINUE", 346, 26, kGold1, kGold2);
                    canvas.centred("ENTER  CONTINUE", 446, 18, kDim);
                } else {
                    title(canvas, "NOT COPIED");
                    float ty = 160;
                    for (const std::string& l : wrap(canvas, upper(failure), 18, 520)) { canvas.centred(l, ty, 18, kRed); ty += 24; }
                    canvas.centred("NOTHING WAS CHANGED.", ty + 20, 16, kDim);
                    canvas.centred("ENTER  CHOOSE ANOTHER IMAGE      ESC  BACK", 446, 18, kDim);
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
                    std::printf("setup picture saved to %s\n", file.c_str());
                }
                wantShot = false;
                glfwSwapBuffers(win);
                if (openDialog && step == Step::Choose) {   // the screen behind the Windows window has just been drawn
                    openDialog = false;
                    askForImage();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            glfwSetKeyCallback(win, nullptr);
            glfwSetMouseButtonCallback(win, nullptr);
            glfwSetDropCallback(win, nullptr);
            gQueue = nullptr;
            // `job` joins its thread when it goes out of scope: the copy is never left running past the window
        }
    }
    glfwDestroyWindow(win);
    glfwTerminate();
    return result;
}

}  // namespace cb2setup
