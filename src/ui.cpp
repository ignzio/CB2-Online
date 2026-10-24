#include "ui.hpp"

#include <cstdio>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "crash_font.inc"

namespace cb2ui {

bool Canvas::init() {
    const char* vs = "#version 330 core\nlayout(location=0) in vec2 p; layout(location=1) in vec2 t; layout(location=2) in vec4 c;\n"
                     "uniform vec2 screen; out vec4 col; out vec2 uv;\n"
                     "void main(){ col = c; uv = t; gl_Position = vec4(p.x / screen.x * 2.0 - 1.0, 1.0 - p.y / screen.y * 2.0, 0.0, 1.0); }\n";
    const char* fs = "#version 330 core\nin vec4 col; in vec2 uv; out vec4 o; uniform sampler2D tex;\n"
                     "void main(){ float a = uv.x < 0.0 ? 1.0 : texture(tex, uv).r; o = vec4(col.rgb, col.a * a); }\n";
    prog = glCreateProgram();
    for (const auto& [type, src] : {std::pair<GLenum, const char*>{GL_VERTEX_SHADER, vs}, {GL_FRAGMENT_SHADER, fs}}) {
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "lobby shader error: %s\n", log); return false; }
        glAttachShader(prog, s);
        glDeleteShader(s);
    }
    glLinkProgram(prog);
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLsizei stride = 8 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(4 * sizeof(float)));
    // the font
    std::vector<unsigned char> bitmap(size_t(kAtlas) * kAtlasH);
    if (stbtt_BakeFontBitmap(kCrashFont, 0, kBake, bitmap.data(), kAtlas, kAtlasH, 32, 95, glyphs) <= 0) return false;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlas, kAtlasH, 0, GL_RED, GL_UNSIGNED_BYTE, bitmap.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

}  // namespace cb2ui
