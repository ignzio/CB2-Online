// Crash Bandicoot 2 level viewer: OpenGL 3.3 core, loads the game's own NSF files at runtime.
//   cb2_viewer <NSF file | folder> [--screenshot out.png]
// Controls: hold right mouse to look, WASD move, Q/E down/up, Shift fast, Tab / Shift+Tab next/prev level,
//           F reframe, Esc quit.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "nsf.hpp"
#include "scenery.hpp"

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aColor;
layout(location=2) in vec2 aUv;
layout(location=3) in ivec4 aTex;
layout(location=4) in int aBlend;
uniform mat4 uViewProj;
out vec3 vColor;
out vec2 vUv;
flat out ivec4 vTex;
void main() {
    vColor = aColor;
    vUv = aUv;
    vTex = aTex;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
)";

// The page texture holds raw PS1 VRAM bytes: 512 bytes per row, 128 rows per page.
// Indexed modes look their colour up in a CLUT stored in the same page.
const char* kFragmentShader = R"(#version 330 core
in vec3 vColor;
in vec2 vUv;
flat in ivec4 vTex;
uniform usampler2D uPages;
uniform bool uTextured;
out vec4 oColor;

uint byteAt(int page, int x, int y) {
    return texelFetch(uPages, ivec2(x, page * 128 + y), 0).r;
}
uint halfAt(int page, int x, int y) {
    return byteAt(page, x * 2, y) | (byteAt(page, x * 2 + 1, y) << 8);
}

void main() {
    int page = vTex.x;
    if (!uTextured || page < 0) {
        oColor = vec4(clamp(vColor * 0.5, 0.0, 1.0), 1.0);
        return;
    }
    int mode = vTex.y;
    int u = int(floor(vUv.x));
    int v = clamp(int(floor(vUv.y)), 0, 127);
    uint texel;
    if (mode == 0) {
        uint b = byteAt(page, clamp(u >> 1, 0, 511), v);
        uint idx = ((u & 1) == 0) ? (b & 15u) : (b >> 4);
        texel = halfAt(page, vTex.z * 16 + int(idx), vTex.w);
    } else if (mode == 1) {
        uint idx = byteAt(page, clamp(u, 0, 511), v);
        texel = halfAt(page, vTex.z * 16 + int(idx), vTex.w);
    } else {
        texel = halfAt(page, clamp(u, 0, 255), v);
    }
    if (texel == 0u) discard;
    vec3 rgb = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u)) / 31.0;
    oColor = vec4(clamp(rgb * vColor, 0.0, 1.0), 1.0);
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
        std::fprintf(stderr, "shader error: %s\n", log);
        std::exit(2);
    }
    return s;
}

GLuint makeProgram() {
    const GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, kVertexShader));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, kFragmentShader));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        std::fprintf(stderr, "program link failed\n");
        std::exit(2);
    }
    return p;
}

struct Batch {
    GLuint vao = 0, vbo = 0;
    GLsizei count = 0;

    void create() {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const GLsizei stride = sizeof(cb2::GpuVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(cb2::GpuVertex, pos));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(cb2::GpuVertex, color));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(cb2::GpuVertex, uv));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 4, GL_INT, stride, (void*)offsetof(cb2::GpuVertex, tex));
        glEnableVertexAttribArray(4);
        glVertexAttribIPointer(4, 1, GL_INT, stride, (void*)offsetof(cb2::GpuVertex, blend));
    }

    void upload(const std::vector<cb2::GpuVertex>& v) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(cb2::GpuVertex)), v.data(), GL_DYNAMIC_DRAW);
        count = GLsizei(v.size());
    }

    void draw() const {
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, count);
    }
};

struct Camera {
    glm::vec3 pos{0, -5, 2};
    float yaw = glm::half_pi<float>(), pitch = 0;  // yaw 0 looks along +X; Z is up
    float speed = 5;

    glm::vec3 front() const {
        return {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    }
    void lookAt(const glm::vec3& target) {
        const glm::vec3 d = glm::normalize(target - pos);
        yaw = std::atan2(d.y, d.x);
        pitch = std::asin(d.z);
    }
};

struct Level {
    std::vector<cb2::Scenery> worlds;
    cb2::PageAtlas atlas;
    cb2::Nsf nsf;
    glm::vec3 center{0};
    float radius = 1;
};

bool loadLevel(const std::string& path, Level& lv) {
    try {
        lv = Level{};
        lv.nsf = cb2::loadNsf(path);
        lv.atlas = cb2::buildPageAtlas(lv.nsf);
        for (const cb2::Entry& e : lv.nsf.entries) {
            if (auto w = cb2::parseScenery(e)) lv.worlds.push_back(std::move(*w));
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), ex.what());
        return false;
    }
    // Percentiles keep a few stray vertices from pushing the camera far away from the level.
    std::vector<glm::vec3> pts;
    for (const cb2::Scenery& w : lv.worlds) {
        if (w.sky) continue;
        for (const cb2::SceneryVertex& v : w.vertices) {
            pts.emplace_back((v.x * 16.0f + w.xOffset) / cb2::kWorldScale, (v.y * 16.0f + w.yOffset) / cb2::kWorldScale,
                             (v.z * 16.0f + w.zOffset) / cb2::kWorldScale);
        }
    }
    if (pts.empty()) {
        lv.center = glm::vec3(0);
        lv.radius = 1;
        return true;
    }
    auto median = [&](int axis) {
        const size_t mid = pts.size() / 2;
        std::nth_element(pts.begin(), pts.begin() + mid, pts.end(),
                         [axis](const glm::vec3& a, const glm::vec3& b) { return a[axis] < b[axis]; });
        return pts[mid][axis];
    };
    lv.center = glm::vec3(median(0), median(1), median(2));
    std::vector<float> dist;
    dist.reserve(pts.size());
    for (const glm::vec3& p : pts) dist.push_back(glm::length(p - lv.center));
    const size_t k = dist.size() * 9 / 10;
    std::nth_element(dist.begin(), dist.begin() + k, dist.end());
    lv.radius = std::max(dist[k], 1.0f);
    return true;
}

GLuint uploadPages(const Level& lv) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const int pages = std::max<int>(1, int(lv.atlas.pages.size()));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 512, pages * 128, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    for (size_t i = 0; i < lv.atlas.pages.size(); ++i) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, int(i) * 128, 512, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        lv.atlas.pages[i]->data.data());
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return tex;
}

void rebuild(const Level& lv, int frame, Batch& world, Batch& sky) {
    std::vector<cb2::GpuVertex> w, s;
    for (const cb2::Scenery& sc : lv.worlds) cb2::buildMesh(sc, lv.atlas, frame, sc.sky ? s : w);
    world.upload(w);
    sky.upload(s);
}

void frameCamera(const Level& lv, Camera& cam) {
    cam.pos = lv.center + glm::vec3(0, -lv.radius * 1.1f, lv.radius * 0.5f);
    cam.lookAt(lv.center);
    cam.speed = lv.radius * 0.5f;
}

std::vector<std::string> findLevels(const std::filesystem::path& root) {
    std::vector<std::string> out;
    if (std::filesystem::is_regular_file(root)) {
        out.push_back(root.string());
        return out;
    }
    for (const auto& de : std::filesystem::recursive_directory_iterator(root)) {
        std::string ext = de.path().extension().string();
        for (auto& c : ext) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (de.is_regular_file() && ext == ".NSF") out.push_back(de.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: cb2_viewer <NSF file | folder> [--screenshot out.png] [--level N]\n");
        return 1;
    }
    std::string shotPath;
    int levelIndex = 0;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) shotPath = argv[++i];
        else if (!std::strcmp(argv[i], "--level") && i + 1 < argc) levelIndex = std::atoi(argv[++i]);
    }
    const auto levels = findLevels(argv[1]);
    if (levels.empty()) {
        std::fprintf(stderr, "no .NSF files found under %s\n", argv[1]);
        return 1;
    }
    levelIndex = std::clamp(levelIndex, 0, int(levels.size()) - 1);

    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, shotPath.empty() ? GLFW_TRUE : GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(1280, 720, "Crash Bandicoot 2 viewer", nullptr, nullptr);
    if (!win) { std::fprintf(stderr, "cannot create an OpenGL 3.3 window\n"); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(glfwGetProcAddress)) { std::fprintf(stderr, "cannot load OpenGL\n"); return 2; }
    glfwSwapInterval(1);

    const GLuint prog = makeProgram();
    const GLint locViewProj = glGetUniformLocation(prog, "uViewProj");
    const GLint locTextured = glGetUniformLocation(prog, "uTextured");
    Batch worldBatch, skyBatch;
    worldBatch.create();
    skyBatch.create();

    Level level;
    Camera cam;
    GLuint pageTex = 0;
    auto switchLevel = [&](int idx) {
        Level next;
        if (!loadLevel(levels[size_t(idx)], next)) return false;
        if (pageTex) glDeleteTextures(1, &pageTex);
        level = std::move(next);
        pageTex = uploadPages(level);
        frameCamera(level, cam);
        rebuild(level, 0, worldBatch, skyBatch);
        std::string title = "Crash Bandicoot 2 viewer - " + levels[size_t(idx)];
        glfwSetWindowTitle(win, title.c_str());
        std::printf("%s: %zu scenery worlds, %zu texture pages\n", levels[size_t(idx)].c_str(), level.worlds.size(),
                    level.atlas.pages.size());
        return true;
    };
    if (!switchLevel(levelIndex)) return 1;

    glEnable(GL_DEPTH_TEST);
    double lastTime = glfwGetTime(), lastX = 0, lastY = 0;
    int lastFrame = 0;
    bool tabHeld = false, fHeld = false;

    auto render = [&](int w, int h) {
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.07f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        const glm::mat4 proj = glm::perspective(glm::radians(60.0f), float(w) / float(std::max(h, 1)), 0.05f,
                                                level.radius * 50.0f);
        const glm::mat4 view = glm::lookAt(cam.pos, cam.pos + cam.front(), glm::vec3(0, 0, 1));
        glm::mat4 skyView = view;
        skyView[3] = glm::vec4(0, 0, 0, 1);
        glUseProgram(prog);
        glUniform1i(locTextured, 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, pageTex);
        glUniform1i(glGetUniformLocation(prog, "uPages"), 0);
        glDisable(GL_CULL_FACE);
        glDepthMask(GL_FALSE);
        glUniformMatrix4fv(locViewProj, 1, GL_FALSE, &(proj * skyView)[0][0]);
        skyBatch.draw();
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUniformMatrix4fv(locViewProj, 1, GL_FALSE, &(proj * view)[0][0]);
        worldBatch.draw();
    };

    if (!shotPath.empty()) {
        const int w = 1280, h = 720;
        render(w, h);
        std::vector<unsigned char> px(size_t(w) * h * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        stbi_flip_vertically_on_write(1);
        const int ok = stbi_write_png(shotPath.c_str(), w, h, 3, px.data(), w * 3);
        std::printf("screenshot %s (%s)\n", shotPath.c_str(), ok ? "ok" : "FAILED");
        glfwTerminate();
        return ok ? 0 : 3;
    }

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        const double now = glfwGetTime();
        const float dt = float(now - lastTime);
        lastTime = now;
        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
            cam.yaw -= float(mx - lastX) * 0.004f;
            cam.pitch = std::clamp(cam.pitch - float(my - lastY) * 0.004f, -1.55f, 1.55f);
        }
        lastX = mx;
        lastY = my;

        const glm::vec3 fwd = cam.front();
        const glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1)));
        const float step = cam.speed * dt * (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 4.0f : 1.0f);
        if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS) cam.pos += fwd * step;
        if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS) cam.pos -= fwd * step;
        if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS) cam.pos += right * step;
        if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS) cam.pos -= right * step;
        if (glfwGetKey(win, GLFW_KEY_E) == GLFW_PRESS) cam.pos.z += step;
        if (glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS) cam.pos.z -= step;

        const bool tab = glfwGetKey(win, GLFW_KEY_TAB) == GLFW_PRESS;
        if (tab && !tabHeld) {
            const int dir = glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? -1 : 1;
            const int n = int(levels.size());
            for (int tries = 0, i = levelIndex; tries < n; ++tries) {
                i = (i + dir + n) % n;
                if (switchLevel(i)) { levelIndex = i; break; }
            }
        }
        tabHeld = tab;
        const bool f = glfwGetKey(win, GLFW_KEY_F) == GLFW_PRESS;
        if (f && !fHeld) frameCamera(level, cam);
        fHeld = f;

        // Flipbook textures advance at 15 fps.
        const int frame = int(now * 15.0);
        if (frame != lastFrame) {
            lastFrame = frame;
            rebuild(level, frame / 2, worldBatch, skyBatch);
        }

        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        render(w, h);
        glfwSwapBuffers(win);
    }
    glfwTerminate();
    return 0;
}
