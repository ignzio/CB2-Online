#include "glrender.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "font5x7.hpp"

namespace psx {
namespace {

const char* kMainVert = R"(#version 330 core
layout(location=0) in vec2 pos;
layout(location=1) in vec2 uv;
layout(location=2) in vec3 col;
layout(location=3) in ivec4 a;
layout(location=4) in ivec4 b;
layout(location=5) in vec4 bounds;
layout(location=6) in float w;
out vec2 vUv; noperspective out vec3 vCol; flat out ivec4 vA; flat out ivec4 vB; flat out vec4 vBounds;
void main() {
    vUv = uv; vCol = col; vA = a; vB = b; vBounds = bounds;
    // w is the polygon's depth (1 where unknown): the position is multiplied by it so that the hardware interpolates the texture coordinates perspective-correctly
    gl_Position = vec4((pos.x / 1024.0 * 2.0 - 1.0) * w, (pos.y / 512.0 * 2.0 - 1.0) * w, 0.0, w);
}
)";

// a = (page x, page y, depth, flags: 1 textured, 2 raw, 4 opaque texels only, 8 semi-transparent texels only, bits 16..23 opacity 1..254 (0 = solid))
// b = (clut x, clut y, window mask x | y << 8, window offset x | y << 8)
const char* kMainFrag = R"(#version 330 core
in vec2 vUv; noperspective in vec3 vCol; flat in ivec4 vA; flat in ivec4 vB; flat in vec4 vBounds;
out vec4 o;
uniform usampler2D vram;
uniform int texFilter;   // 0 nearest (the console's own look), 1 bilinear
uint fetch(int x, int y) { return texelFetch(vram, ivec2(x & 1023, y & 511), 0).r; }
// One texel of the polygon's texture page (palette and texture window applied); 0 = transparent.
uint texel(int u, int v) {
    u &= 255; v &= 255;
    int mx = vB.z & 255, my = (vB.z >> 8) & 255, ox = vB.w & 255, oy = (vB.w >> 8) & 255;
    u = (u & ~(mx * 8)) | ((ox & mx) * 8);
    v = (v & ~(my * 8)) | ((oy & my) * 8);
    int base = vA.x * 64, row = vA.y * 256 + v;
    if (vA.z == 0) {
        uint w = fetch(base + u / 4, row);
        return fetch(vB.x + int((w >> uint((u & 3) * 4)) & 15u), vB.y);
    } else if (vA.z == 1) {
        uint w = fetch(base + u / 2, row);
        return fetch(vB.x + int((w >> uint((u & 1) * 8)) & 255u), vB.y);
    }
    return fetch(base + u, row);
}
vec3 rgb555(uint t) { return vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u)) / 31.0; }
void main() {
    vec3 c = vCol;
    if ((vA.w & 1) != 0) {
        int u = int(floor(vUv.x + 0.002));
        int v = int(floor(vUv.y + 0.002));
        uint t = texel(u, v);
        vec3 tc;
        if (texFilter == 0) {
            if (t == 0u) discard;
            tc = rgb555(t);
        } else {
            // four texels around the sample point, weighted; transparent ones do not count (so edges of cut-outs stay clean) and the
            // texel indices stay inside the polygon's own texture coordinates (no bleeding from the neighbouring picture in the page)
            vec2 p = vUv - 0.5;
            ivec2 i0 = ivec2(floor(p));
            vec2 f = p - vec2(i0);
            ivec2 lo = ivec2(floor(vBounds.xy)), hi = ivec2(max(ceil(vBounds.zw) - 1.0, floor(vBounds.xy)));
            ivec2 a = clamp(i0, lo, hi), b = clamp(i0 + 1, lo, hi);
            uint t00 = texel(a.x, a.y), t10 = texel(b.x, a.y), t01 = texel(a.x, b.y), t11 = texel(b.x, b.y);
            float w00 = (1.0 - f.x) * (1.0 - f.y), w10 = f.x * (1.0 - f.y), w01 = (1.0 - f.x) * f.y, w11 = f.x * f.y;
            float a00 = t00 == 0u ? 0.0 : w00, a10 = t10 == 0u ? 0.0 : w10, a01 = t01 == 0u ? 0.0 : w01, a11 = t11 == 0u ? 0.0 : w11;
            float sum = a00 + a10 + a01 + a11;
            if (sum < 0.5) discard;
            tc = (rgb555(t00) * a00 + rgb555(t10) * a10 + rgb555(t01) * a01 + rgb555(t11) * a11) / sum;
            t = f.x < 0.5 ? (f.y < 0.5 ? t00 : t01) : (f.y < 0.5 ? t10 : t11);   // the nearest texel decides the semi-transparency pass
            if (t == 0u) t = t00 | t10 | t01 | t11;
        }
        bool semiTexel = (t & 0x8000u) != 0u;
        if ((vA.w & 4) != 0 && semiTexel) discard;
        if ((vA.w & 8) != 0 && !semiTexel) discard;
        if ((vA.w & 2) == 0) tc = min(tc * vCol * 2.0, vec3(1.0));
        c = tc;
    }
    int ab = (vA.w >> 16) & 255;
    o = vec4(c, ab == 0 ? 1.0 : float(ab) / 255.0);
}
)";
const char* kBlitVert = R"(#version 330 core
uniform vec4 rect; // x y w h in VRAM pixels
out vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    uv = p;
    vec2 px = rect.xy + p * rect.zw;
    gl_Position = vec4(px.x / 1024.0 * 2.0 - 1.0, px.y / 512.0 * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kBlitFrag = R"(#version 330 core
in vec2 uv; out vec4 o;
uniform sampler2D tex; uniform vec4 region; // normalised sub-rectangle of the upload texture
void main() { o = vec4(texture(tex, region.xy + uv * region.zw).rgb, 1.0); }
)";

const char* kCompositeFrag = R"(#version 330 core
in vec2 uv; out vec4 o;
uniform sampler2D mid; uniform sampler2D real; uniform vec4 region;
void main() {
    vec2 q = region.xy + uv * region.zw;
    vec3 m = texture(mid, q).rgb, r = texture(real, q).rgb;
    float lm = m.r + m.g + m.b, lr = r.r + r.g + r.b;
    // a gap (nothing drew, or a dark backdrop polygon that the moved scenery no longer covers) is dark in the in-between picture but not in the real one
    bool gap = (lm < 0.07 && lr >= 0.07) || (lm < 0.42 && lr > 1.0 && lm < 0.3 * lr);
    o = vec4(gap ? r : m, 1.0);
}
)";

GLuint compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader error: %s\n", log);
    }
    return s;
}

GLuint program(const char* vs, const char* fs) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

}  // namespace

bool HiResRenderer::State::operator!=(const State& o) const {
    return blend != o.blend || std::memcmp(area, o.area, sizeof area) != 0;
}

bool HiResRenderer::init(int scale) {
    scaleFactor = std::max(1, std::min(scale, 8));
    mainProg = program(kMainVert, kMainFrag);
    blitProg = program(kBlitVert, kBlitFrag);
    compProg = program(kBlitVert, kCompositeFrag);
    mainVram = glGetUniformLocation(mainProg, "vram");
    mainFilter = glGetUniformLocation(mainProg, "texFilter");
    blitRect = glGetUniformLocation(blitProg, "rect");
    blitTex = glGetUniformLocation(blitProg, "tex");

    glGenTextures(1, &vramTex);
    glBindTexture(GL_TEXTURE_2D, vramTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &uploadTex);
    glBindTexture(GL_TEXTURE_2D, uploadTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, textureWidth(), textureHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const GLsizei stride = sizeof(Vertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, r)));
    glEnableVertexAttribArray(3); glVertexAttribIPointer(3, 4, GL_INT, stride, reinterpret_cast<void*>(offsetof(Vertex, a)));
    glEnableVertexAttribArray(4); glVertexAttribIPointer(4, 4, GL_INT, stride, reinterpret_cast<void*>(offsetof(Vertex, b2)));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, bounds)));
    glEnableVertexAttribArray(6); glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, w)));
    glGenVertexArrays(1, &blitVao);
    return ok;
}

void HiResRenderer::apply(const State& s) {
    glScissor(s.area[0] * scaleFactor, s.area[1] * scaleFactor, (s.area[2] - s.area[0] + 1) * scaleFactor, (s.area[3] - s.area[1] + 1) * scaleFactor);
    if (s.blend < 0) { glDisable(GL_BLEND); return; }
    glEnable(GL_BLEND);
    glBlendEquation(s.blend == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
    switch (s.blend) {
    case 0: glBlendColor(0.5f, 0.5f, 0.5f, 0.5f); glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_COLOR); break;  // (B + F) / 2
    case 1: glBlendFunc(GL_ONE, GL_ONE); break;                                                              // B + F
    case 2: glBlendFunc(GL_ONE, GL_ONE); break;                                                              // B - F
    case 5: glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE); break;               // see-through (other players)
    default: glBlendColor(0.25f, 0.25f, 0.25f, 0.25f); glBlendFunc(GL_CONSTANT_COLOR, GL_ONE); break;     // B + F / 4
    }
}

void HiResRenderer::flush() {
    if (batch.empty()) return;
    apply(cur);
    glUseProgram(mainProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vramTex);
    glUniform1i(mainVram, 0);
    glUniform1i(mainFilter, textureFilter);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(batch.size() * sizeof(Vertex)), batch.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(batch.size()));
    batch.clear();
}

void HiResRenderer::widen(GpuCmd& c) const {
    const float left = float(c.area[0]), width = float(c.area[2] - c.area[0] + 1);
    if (width < 250.0f) return;   // render-to-texture work, not a picture for the screen
    const float cx = left + width * 0.5f, f = wide;
    if (c.w[0] > 1.0f) {   // 3D: the GTE gave a depth for every corner
        for (int i = 0; i < 3; ++i) c.x[i] = cx + (c.x[i] - cx) * f;
        return;
    }
    const float lo = std::min({c.x[0], c.x[1], c.x[2]}), hi = std::max({c.x[0], c.x[1], c.x[2]});
    if (hi - lo > 0.6f * width) return;   // a backdrop or a fade: it fills the stretched picture as it is
    // 2D: a map that squeezes the outer quarters by f (where the HUD lives) and stretches the middle to make up for it, so it stays continuous across the polygons of one element
    const float A = width * 0.25f, s = (width - 2.0f * A * f) / (width - 2.0f * A);
    for (int i = 0; i < 3; ++i) {
        const float x = c.x[i] - left;
        const float m = x <= A ? x * f : x >= width - A ? width - (width - x) * f : A * f + (x - A) * s;
        c.x[i] = left + m;
    }
}

void HiResRenderer::pushTri(const GpuCmd& c0, int passFlag) {
    GpuCmd wc;
    if (wide < 0.999f) { wc = c0; widen(wc); }
    const GpuCmd& c = wide < 0.999f ? wc : c0;
    for (int i = 0; i < 3; ++i) {
        Vertex v{};
        v.x = c.x[i]; v.y = c.y[i]; v.u = c.u[i]; v.v = c.v[i];
        v.r = c.col[i][0] / 255.0f; v.g = c.col[i][1] / 255.0f; v.b = c.col[i][2] / 255.0f;
        v.a[0] = c.tpx; v.a[1] = c.tpy; v.a[2] = c.depth;
        v.a[3] = (c.textured ? 1 : 0) | (c.raw ? 2 : 0) | passFlag | (c.alpha < 255 ? int(c.alpha ? c.alpha : 1) << 16 : 0);
        v.b2[0] = c.clutX; v.b2[1] = c.clutY;
        v.b2[2] = c.twMX | (c.twMY << 8);
        v.b2[3] = c.twOX | (c.twOY << 8);
        v.bounds[0] = std::min({c.u[0], c.u[1], c.u[2]}); v.bounds[1] = std::min({c.v[0], c.v[1], c.v[2]});
        v.w = perspective ? c.w[i] : 1.0f;
        v.bounds[2] = std::max({c.u[0], c.u[1], c.u[2]}); v.bounds[3] = std::max({c.v[0], c.v[1], c.v[2]});
        batch.push_back(v);
    }
}

void HiResRenderer::pushQuad(float x0, float y0, float x1, float y1, uint32_t rgb) {
    GpuCmd c;
    const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
    const int tri[2][3] = {{0, 1, 2}, {1, 2, 3}};
    for (const auto& t : tri) {
        for (int i = 0; i < 3; ++i) {
            c.x[i] = xs[t[i]]; c.y[i] = ys[t[i]];
            c.col[i][0] = uint8_t(rgb); c.col[i][1] = uint8_t(rgb >> 8); c.col[i][2] = uint8_t(rgb >> 16);
        }
        pushTri(c, 0);
    }
}

void HiResRenderer::blit(const GpuCmd& c, const uint16_t* px) {
    static std::vector<uint32_t> rgba;
    rgba.resize(size_t(c.rw) * size_t(c.rh));
    for (size_t i = 0; i < rgba.size(); ++i) {
        const uint16_t p = px[i];
        rgba[i] = 0xFF000000u | uint32_t(((p >> 10) & 31) * 255 / 31) << 16 | uint32_t(((p >> 5) & 31) * 255 / 31) << 8 | uint32_t((p & 31) * 255 / 31);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, uploadTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, c.rw, c.rh, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(blitProg);
    glUniform1i(blitTex, 0);
    glUniform4f(blitRect, float(c.rx), float(c.ry), float(c.rw), float(c.rh));
    glUniform4f(glGetUniformLocation(blitProg, "region"), 0.0f, 0.0f, float(c.rw) / 1024.0f, float(c.rh) / 512.0f);
    glBindVertexArray(blitVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glEnable(GL_SCISSOR_TEST);
}

void HiResRenderer::render(Gpu& gpu) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, textureWidth(), textureHeight());
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vramTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512, GL_RED_INTEGER, GL_UNSIGNED_SHORT, gpu.vram.data());

    textHres = gpu.hres;
    gpuAreaLeft = float(gpu.displayX); gpuAreaWidth = float(gpu.hres);
    replay(gpu.cmds, gpu.textPool, &gpu.cmdPool);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gpu.cmds.clear();
    gpu.cmdPool.clear();
    gpu.textPool.clear();
}

// An in-between picture (see interp.hpp): the same replay into a second target, with the VRAM texture as the last render() left it.
void HiResRenderer::renderMid(const std::vector<GpuCmd>& cmds, const std::vector<std::string>& texts, int hres, int vres, int displayX, int displayY) {
    textHres = hres;
    gpuAreaLeft = float(displayX); gpuAreaWidth = float(hres);
    // Only the part of the target that is on screen is cleared, copied and composed: the whole texture is 16 times larger than the picture and touching it all took 8 ms.
    const int rx = std::max(0, displayX * scaleFactor - 2), ry = std::max(0, displayY * scaleFactor - 2);
    const int rw = std::min(textureWidth() - rx, hres * scaleFactor + 4), rh = std::min(textureHeight() - ry, vres * scaleFactor + 4);
    if (!midFbo) {
        glGenTextures(1, &midTex);
        glBindTexture(GL_TEXTURE_2D, midTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, textureWidth(), textureHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &midFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, midFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, midTex, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, midFbo);
    glViewport(0, 0, textureWidth(), textureHeight());
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rx, ry, rw, rh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vramTex);
    replay(cmds, texts, nullptr);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    // Where the moved triangles left a gap, the picture is black (a pixel nothing drew, or a black backdrop polygon the scenery above it no longer covers): take the real
    // picture there. Copy the result aside first, then compose the two into the target.
    if (!tmpFbo) {
        glGenTextures(1, &tmpTex);
        glBindTexture(GL_TEXTURE_2D, tmpTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, textureWidth(), textureHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &tmpFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, tmpFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmpTex, 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, midFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmpFbo);
    glBlitFramebuffer(rx, ry, rx + rw, ry + rh, rx, ry, rx + rw, ry + rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, midFbo);
    glUseProgram(compProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tmpTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glUniform1i(glGetUniformLocation(compProg, "mid"), 0);
    glUniform1i(glGetUniformLocation(compProg, "real"), 1);
    glUniform4f(glGetUniformLocation(compProg, "rect"), float(rx) / float(scaleFactor), float(ry) / float(scaleFactor), float(rw) / float(scaleFactor), float(rh) / float(scaleFactor));
    glUniform4f(glGetUniformLocation(compProg, "region"), float(rx) / float(textureWidth()), float(ry) / float(textureHeight()), float(rw) / float(textureWidth()), float(rh) / float(textureHeight()));
    glBindVertexArray(blitVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
void HiResRenderer::replay(const std::vector<GpuCmd>& cmds, const std::vector<std::string>& texts, const std::vector<uint16_t>* pool) {
    cur = State{};
    batch.clear();
    auto want = [&](int blend, const int16_t* area) {
        State s;
        s.blend = blend;
        std::memcpy(s.area, area, sizeof s.area);
        if (s != cur) { flush(); cur = s; }
    };
    static const int16_t kFull[4] = {0, 0, 1023, 511};
    for (const GpuCmd& c : cmds) {
        switch (c.kind) {
        case GpuCmd::Kind::Fill:
            want(-1, kFull);
            pushQuad(float(c.rx), float(c.ry), float(c.rx + c.rw), float(c.ry + c.rh), c.color);
            break;
        case GpuCmd::Kind::Flip: break;
        case GpuCmd::Kind::Upload:
            if (!pool) break;
            flush();
            blit(c, pool->data() + c.poolOffset);
            cur = State{};
            cur.blend = -2;  // force the state to be re-applied
            break;
        case GpuCmd::Kind::Text: {
            // 2 x 3 console pixels per font pixel; the tag is centred on x and sits above y
            want(-1, kFull);
            const std::string& s = texts[c.poolOffset];
            // glyph pixels are square on the 4:3 picture whatever the console's horizontal resolution
            const float px = std::max(1.0f, float(textHres) / 320.0f), py = 1.0f;
            const float w = (float(s.size()) * 6.0f + 1.0f) * px, h = 9.0f * py;
            // a name tag is anchored to a 3D position: it moves as 3D geometry does (its text is not squeezed)
            float tagX = c.x[0];
            if (wide < 0.999f && gpuAreaWidth > 250.0f) tagX = gpuAreaLeft + gpuAreaWidth * 0.5f + (tagX - (gpuAreaLeft + gpuAreaWidth * 0.5f)) * wide;
            const float x0 = tagX - w * 0.5f, y0 = c.y[0] - h;
            pushQuad(x0 - 1, y0 - 1, x0 + w + 1, y0 + h + 1, 0x000000);
            pushQuad(x0, y0, x0 + w, y0 + h, 0x20182C);
            for (size_t i = 0; i < s.size(); ++i) {
                const uint8_t* g = glyph5x7(s[i]);
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if ((g[row] >> (4 - col)) & 1) {
                            const float gx = x0 + (1.0f + float(i) * 6.0f + float(col)) * px, gy = y0 + (1.0f + float(row)) * py;
                            pushQuad(gx, gy, gx + px, gy + py, c.color);
                        }
            }
            break;
        }
        case GpuCmd::Kind::Tri:
            if (c.semi && c.textured) {
                want(-1, c.area); pushTri(c, 4);
                want(c.semiMode, c.area); pushTri(c, 8);
            } else {
                want(c.semi ? c.semiMode : (c.alpha < 255 ? 5 : -1), c.area);
                pushTri(c, 0);
            }
            break;
        }
    }
    flush();
    if (pool && std::getenv("CB2_GLDEBUG")) {
        int cnt[2][2][5] = {};
        for (const GpuCmd& c : cmds) if (c.kind == GpuCmd::Kind::Tri) ++cnt[c.textured][c.raw][c.semi ? c.semiMode : 4];
        for (int t = 0; t < 2; ++t) for (int r = 0; r < 2; ++r) for (int m = 0; m < 5; ++m) if (cnt[t][r][m]) std::printf("[gl] textured=%d raw=%d %s=%d : %d\n", t, r, m == 4 ? "opaque" : "semi", m == 4 ? 0 : m, cnt[t][r][m]);
    }
}

void HiResRenderer::readDisplay(const Gpu& gpu, std::vector<uint32_t>& rgba, int& w, int& h, bool mid) const {
    w = gpu.hres * scaleFactor;
    h = gpu.vres * scaleFactor;
    std::vector<uint32_t> raw(size_t(w) * size_t(h));
    glBindFramebuffer(GL_FRAMEBUFFER, mid && midFbo ? midFbo : fbo);
    glReadPixels(gpu.displayX * scaleFactor, gpu.displayY * scaleFactor, w, h, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    rgba.resize(raw.size());
    // blending also acts on alpha (subtract mode drives it to 0); the picture itself is opaque
    for (size_t i = 0; i < raw.size(); ++i) rgba[i] = raw[i] | 0xFF000000u;
}

}  // namespace psx
