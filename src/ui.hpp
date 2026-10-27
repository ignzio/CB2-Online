#pragma once
// Shared menu drawing: the Crash-a-Like font, wooden planks on warp-room stone. Used by the online lobby (lobby.cpp) and the in-game
// pause menu (pausemenu.cpp). Everything is laid out on a 640 x 480 screen that is scaled to the current viewport.
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include "stb_truetype.h"

namespace cb2ui {

inline constexpr float kW = 640.0f, kH = 480.0f;   // the menu is laid out on a 640 x 480 screen, scaled to the window
inline constexpr float kBake = 64.0f;              // pixel height the font is baked at; text is drawn scaled from it
inline constexpr int kAtlas = 1024, kAtlasH = 512;

struct Rgb { float r, g, b; };
constexpr Rgb rgb(int r, int g, int b) { return {r / 255.0f, g / 255.0f, b / 255.0f}; }

// Textured and flat coloured triangles: text comes from the Crash-a-Like font (baked into an atlas), the rest are plain rectangles.
class Canvas {
public:
    bool init();   // compiles the shaders and bakes the Crash-a-Like font (ui.cpp)
    ~Canvas() {
        if (prog) glDeleteProgram(prog);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (vao) glDeleteVertexArrays(1, &vao);
        if (tex) glDeleteTextures(1, &tex);
    }

    void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Rgb top, Rgb bottom, float alpha) {
        const float v[4][8] = {{x0, y0, u0, v0, top.r, top.g, top.b, alpha}, {x1, y0, u1, v0, top.r, top.g, top.b, alpha},
                               {x0, y1, u0, v1, bottom.r, bottom.g, bottom.b, alpha}, {x1, y1, u1, v1, bottom.r, bottom.g, bottom.b, alpha}};
        for (int i : {0, 1, 2, 1, 2, 3}) data.insert(data.end(), v[i], v[i] + 8);
    }
    void rect(float x, float y, float w, float h, Rgb top, Rgb bottom, float alpha = 1.0f) { quad(x, y, x + w, y + h, -1, -1, -1, -1, top, bottom, alpha); }
    void rect(float x, float y, float w, float h, Rgb c, float alpha = 1.0f) { rect(x, y, w, h, c, c, alpha); }
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, Rgb c) {
        const float xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
        for (int i = 0; i < 3; ++i) { const float v[8] = {xs[i], ys[i], -1, -1, c.r, c.g, c.b, 1}; data.insert(data.end(), v, v + 8); }
    }

    float width(const std::string& s, float size) const {
        float w = 0;
        for (char c : s) if (c >= 32 && c < 127) w += glyphs[c - 32].xadvance;
        return w * size / kBake;
    }
    // One run of text, filled with a vertical colour ramp.
    void glyphRun(const std::string& s, float x, float baseline, float size, Rgb top, Rgb bottom, float alpha) {
        const float k = size / kBake;
        float cx = 0, cy = 0;
        for (char c : s) {
            if (c < 32 || c >= 127) continue;
            stbtt_aligned_quad q;
            stbtt_GetBakedQuad(glyphs, kAtlas, kAtlasH, c - 32, &cx, &cy, &q, 1);
            quad(x + q.x0 * k, baseline + q.y0 * k, x + q.x1 * k, baseline + q.y1 * k, q.s0, q.t0, q.s1, q.t1, top, bottom, alpha);
        }
    }
    // y is the top of a line of text of the given size.
    void text(const std::string& s, float x, float y, float size, Rgb top, Rgb bottom, bool outline = true) {
        const float baseline = y + size * 0.78f;
        const float o = std::max(1.0f, size / 22.0f);
        if (outline) {
            const Rgb dark = rgb(8, 6, 14);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (dx || dy) glyphRun(s, x + dx * o, baseline + dy * o, size, dark, dark, 1.0f);
            glyphRun(s, x + o * 1.5f, baseline + o * 2.0f, size, dark, dark, 0.7f);
        }
        glyphRun(s, x, baseline, size, top, bottom, 1.0f);
    }
    void text(const std::string& s, float x, float y, float size, Rgb c, bool outline = true) { text(s, x, y, size, c, c, outline); }
    void centred(const std::string& s, float y, float size, Rgb top, Rgb bottom) { text(s, (kW - width(s, size)) * 0.5f, y, size, top, bottom); }
    void centred(const std::string& s, float y, float size, Rgb c) { centred(s, y, size, c, c); }

    void flush() {
        if (data.empty()) return;
        glUseProgram(prog);
        glUniform2f(glGetUniformLocation(prog, "screen"), kW, kH);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.size() * sizeof(float)), data.data(), GL_STREAM_DRAW);
        glEnable(GL_BLEND);
        // the game's own passes leave other state behind (e.g. subtractive blending for fades), so set all of it
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(data.size() / 8));
        glDisable(GL_BLEND);
        data.clear();
    }

private:
    GLuint prog = 0, vao = 0, vbo = 0, tex = 0;
    std::vector<float> data;
    stbtt_bakedchar glyphs[95] = {};
};

// ---- look: the game's own palette (warp-room stone, wooden crate planks, wumpa orange) ----
inline void background(Canvas& c, double t) {
    c.rect(0, 0, kW, kH, rgb(22, 52, 66), rgb(6, 14, 22));
    // stone blocks of the warp room, faint
    for (int row = 0; row < 8; ++row)
        for (int col = -1; col < 7; ++col) {
            const float x = col * 110.0f + (row % 2 ? 55.0f : 0.0f), y = 70.0f + row * 46.0f;
            c.rect(x + 2, y + 2, 106, 42, rgb(255, 255, 255), 0.025f);
        }
    // a slow glow behind the title, like the portal's light
    const float pulse = 0.5f + 0.5f * float(std::sin(t * 1.3));
    c.rect(120, 20, 400, 150, rgb(70, 160, 190), rgb(22, 52, 66), 0.10f + 0.05f * pulse);
    // wooden beams top and bottom, nailed
    for (float y : {0.0f, kH - 30.0f}) {
        c.rect(0, y, kW, 30, rgb(150, 92, 38), rgb(96, 54, 20));
        c.rect(0, y + (y > 0 ? 0.0f : 28.0f), kW, 2, rgb(48, 26, 8));
        c.rect(0, y + 6, kW, 2, rgb(190, 126, 56), 0.5f);
        for (float x = 24; x < kW; x += 80) { c.rect(x, y + 11, 8, 8, rgb(40, 22, 8)); c.rect(x + 1, y + 12, 3, 3, rgb(210, 160, 90), 0.8f); }
    }
}

inline void plank(Canvas& c, float x, float y, float w, float h, bool selected) {
    const Rgb top = selected ? rgb(214, 134, 40) : rgb(120, 74, 30), bottom = selected ? rgb(150, 82, 20) : rgb(74, 42, 16);
    c.rect(x - 3, y - 3, w + 6, h + 6, rgb(20, 10, 4));
    c.rect(x, y, w, h, top, bottom);
    c.rect(x, y, w, 3, rgb(255, 230, 150), selected ? 0.55f : 0.18f);
    c.rect(x, y + h - 3, w, 3, rgb(0, 0, 0), 0.25f);
    for (float gx : {x + w * 0.27f, x + w * 0.64f}) c.rect(gx, y + 5, 2, h - 10, rgb(0, 0, 0), 0.12f);   // wood grain
    if (selected) {
        c.triangle(x - 30, y + h * 0.5f - 13, x - 30, y + h * 0.5f + 13, x - 8, y + h * 0.5f, rgb(255, 198, 40));
        c.triangle(x - 27, y + h * 0.5f - 8, x - 27, y + h * 0.5f + 8, x - 14, y + h * 0.5f, rgb(255, 120, 20));
    }
}

inline const Rgb kGold1 = rgb(255, 226, 90), kGold2 = rgb(255, 140, 24), kWhite1 = rgb(246, 244, 236), kWhite2 = rgb(200, 196, 188);
inline const Rgb kTeal = rgb(128, 232, 255), kDim = rgb(170, 190, 200), kGreen = rgb(130, 255, 150), kRed = rgb(255, 120, 96);

// A padlock (x, y = its top left corner; 16 wide, 20 high): marks rooms that ask for a password.
inline void lockIcon(Canvas& c, float x, float y, Rgb colour) {
    const Rgb dark = rgb(20, 10, 4);
    c.rect(x + 1, y - 1, 14, 9, dark);
    c.rect(x + 3, y, 3, 10, dark);
    c.rect(x + 10, y, 3, 10, dark);
    c.rect(x + 4, y, 2, 10, colour);
    c.rect(x + 10, y, 2, 10, colour);
    c.rect(x + 4, y, 8, 2, colour);
    c.rect(x - 1, y + 8, 18, 13, dark);
    c.rect(x, y + 9, 16, 11, colour);
    c.rect(x + 7, y + 12, 2, 5, dark);
}

inline void title(Canvas& c, const std::string& sub) {
    c.centred("CRASH BANDICOOT 2", 30, 60, kGold1, kGold2);
    c.centred(sub, 98, 30, kTeal);
}

}  // namespace cb2ui
