#pragma once
// High-resolution OpenGL backend for the PlayStation GPU. The software GPU keeps VRAM exact; every primitive it
// processes is also recorded (Gpu::cmds) and replayed here into a framebuffer that is `scale` times larger, using the
// native VRAM as the texture source (4/8/15-bit textures and CLUTs are decoded in the fragment shader).
#include <cstdint>
#include <string>
#include <vector>

#include <glad/gl.h>

#include "gpu.hpp"

namespace psx {

class HiResRenderer {
public:
    bool init(int scale);
    int scale() const { return scaleFactor; }
    // 0: every texel is read as it is (the console's look); 1: bilinear, within the polygon's own texture coordinates and without blending in transparent texels
    void setTextureFilter(int mode) { textureFilter = mode; }
    // Replays and clears gpu.cmds / gpu.cmdPool.
    void render(Gpu& gpu);
    // An in-between picture drawn from the given commands (without VRAM uploads) into a second target of the same layout; midTexture() holds it.
    void renderMid(const std::vector<GpuCmd>& cmds, const std::vector<std::string>& texts, int hres);
    GLuint colorTexture() const { return colorTex; }
    GLuint midTexture() const { return midTex; }
    int textureWidth() const { return 1024 * scaleFactor; }
    int textureHeight() const { return 512 * scaleFactor; }
    // RGBA8, rows top to bottom, of the area the console is currently displaying.
    void readDisplay(const Gpu& gpu, std::vector<uint32_t>& rgba, int& w, int& h, bool mid = false) const;

private:
    struct Vertex { float x, y, u, v, r, g, b; int32_t a[4]; int32_t b2[4]; float bounds[4]; };
    struct State { int blend = -1; int16_t area[4] = {0, 0, 1023, 511}; bool operator!=(const State& o) const; };

    int scaleFactor = 2, textHres = 320;
    GLuint fbo = 0, colorTex = 0, vramTex = 0, uploadTex = 0, mainProg = 0, blitProg = 0, vao = 0, vbo = 0, blitVao = 0, midFbo = 0, midTex = 0, midStencil = 0;
    std::vector<Vertex> batch;
    State cur;
    GLint blitRect = -1, blitTex = -1, mainVram = -1, mainFilter = -1;
    int textureFilter = 0;

    void replay(const std::vector<GpuCmd>& cmds, const std::vector<std::string>& texts, const std::vector<uint16_t>* pool);
    void flush();
    void apply(const State& s);
    void pushTri(const GpuCmd& c, int passFlag);
    void pushQuad(float x0, float y0, float x1, float y1, uint32_t rgb);
    void blit(const GpuCmd& c, const uint16_t* pixels);
};

}  // namespace psx
