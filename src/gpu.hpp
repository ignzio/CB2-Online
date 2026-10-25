#pragma once
// PlayStation GPU: GP0/GP1 command interpreter with a software rasteriser writing into a 1024x512 VRAM.
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <string>
#include <vector>

namespace psx {

struct StateAccess;   // save states (savestate.cpp)

// One drawing operation as the game issued it, for renderers that replay the frame on the host GPU at a higher
// resolution. The software rasteriser still maintains the exact VRAM (textures and render-to-texture effects read it).
struct GpuCmd {
    enum class Kind : uint8_t { Tri, Fill, Upload, Text, Flip } kind = Kind::Tri;
    // Tri: screen-space vertices in VRAM pixels (draw offset already applied), texel coordinates, vertex colours.
    float x[3] = {}, y[3] = {}, u[3] = {}, v[3] = {};
    uint8_t col[3][3] = {};
    bool textured = false, raw = false, semi = false;
    uint8_t semiMode = 0, depth = 0, tpx = 0, tpy = 0, twMX = 0, twMY = 0, twOX = 0, twOY = 0;
    uint16_t clutX = 0, clutY = 0;
    uint8_t alpha = 255;                   // Tri: opacity the host renderer gives it (other players' Crashes, see markAlpha); 255 = the game's own look
    int16_t area[4] = {};                  // draw area x0 y0 x1 y1 (inclusive)
    // Fill / Upload: VRAM rectangle. Upload carries the final pixels (offset into Gpu::cmdPool). Flip: the display area moved to rx, ry (size rw x rh).
    int16_t rx = 0, ry = 0, rw = 0, rh = 0;
    uint32_t color = 0, poolOffset = 0;   // Text: x[0], y[0] = centre of the text in VRAM pixels, poolOffset = index in Gpu::textPool, color = rgb
};

class Gpu {
    friend struct StateAccess;
public:
    static constexpr int kW = 1024, kH = 512;
    std::vector<uint16_t> vram = std::vector<uint16_t>(kW * kH, 0);

    // Command recording for host-GPU renderers (off by default). The consumer clears both vectors after use.
    bool recording = false;
    const std::unordered_map<uint32_t, std::array<float, 2>>* pgxp = nullptr;   // precise vertex positions from the GTE
    // When false, polygons, sprites and lines are only recorded (for a host-GPU renderer) and no pixels are drawn into
    // VRAM. Uploads, copies and fills still update VRAM, because textures and palettes are read from it.
    bool rasterize = true;
    std::vector<GpuCmd> cmds;
    std::vector<uint16_t> cmdPool;

    // Text drawn on top of the picture by the host renderer (name tags of other players). A tag is queued with the position the GTE
    // projected (screen space, before the draw offset); once the game has sent its ordering table to the GPU the queued tags become
    // Text commands at the end of that frame's command list, with the draw offset that was in effect.
    struct TextTag { std::string text; float x = 0, y = 0; uint32_t rgb = 0xFFFFFF; int offX = 0, offY = 0; };
    std::vector<TextTag> pendingTags;
    // Queue a tag in the coordinates of the frame being drawn now; it is added to the command list by flushTags (at the end of the frame, so it
    // lies over everything the game drew, whatever the order of its display lists).
    void queueTag(TextTag t) { t.offX = offX; t.offY = offY; pendingTags.push_back(std::move(t)); }
    std::vector<std::string> textPool;
    // Other players' Crashes can be drawn see-through: the vertices (screen positions before the draw offset, as the GTE projected them) that belong to
    // them are marked with markAlpha during the frame, and every recorded triangle whose three vertices are all marked gets `alphaValue`. The marks are
    // dropped by flushTags at the end of the frame.
    uint8_t alphaValue = 255;
    void markAlpha(float sx, float sy) { alphaKeys.insert(alphaKey(sx + float(offX), sy + float(offY))); }
    static uint64_t alphaKey(float x, float y) { return (uint64_t(uint32_t(int32_t(x * 8.0f + 0.5f + 65536.0f))) << 32) | uint32_t(int32_t(y * 8.0f + 0.5f + 65536.0f)); }
    std::unordered_set<uint64_t> alphaKeys;
    void flushTags() {
        alphaKeys.clear();
        if (pendingTags.empty()) return;
        if (recording) {
            for (const TextTag& t : pendingTags) {
                GpuCmd c;
                c.kind = GpuCmd::Kind::Text;
                c.x[0] = t.x + float(t.offX);
                c.y[0] = t.y + float(t.offY);
                c.color = t.rgb;
                c.poolOffset = uint32_t(textPool.size());
                textPool.push_back(t.text);
                cmds.push_back(c);
            }
        }
        pendingTags.clear();
    }

    // Display state (GP1)
    bool displayEnabled = false;
    int displayX = 0, displayY = 0;
    int hres = 320, vres = 240;
    bool color24 = false, interlaced = false, pal = false;
    uint32_t drawCalls = 0, primitives = 0;
    uint32_t unknownCommands = 0;

    void gp0(uint32_t w);
    void gp1(uint32_t w);
    uint32_t stat();
    uint32_t read();
    void dmaWrite(const uint32_t* words, uint32_t n) { for (uint32_t i = 0; i < n; ++i) gp0(words[i]); }
    uint32_t dmaRead() { return read(); }
    void vblank() { field ^= 1; }
    int dmaDirection() const { return dir; }

private:
    struct Vtx { int x, y; int r, g, b; int u, v; float fx = 1e30f, fy = 1e30f; };   // fx/fy: precise position when known

    // Command assembly
    std::vector<uint32_t> fifo;
    uint32_t need = 0;
    bool polyLine = false;
    // VRAM transfers
    enum class Xfer { None, ToVram, FromVram } xfer = Xfer::None;
    int xX = 0, xY = 0, xW = 0, xH = 0, xPos = 0;
    uint32_t readLatch = 0;

    // State
    int tpX = 0, tpY = 0, semi = 0, depth = 0;
    bool dither = false, drawToDisplay = false, texDisable = false, rectFlipX = false, rectFlipY = false;
    int twMaskX = 0, twMaskY = 0, twOffX = 0, twOffY = 0;
    int areaX0 = 0, areaY0 = 0, areaX1 = 1023, areaY1 = 511, offX = 0, offY = 0;
    bool setMask = false, checkMask = false;
    int dir = 0, field = 0;
    uint32_t infoLatch = 0;

    void execute();
    uint32_t commandLength(uint32_t cmd) const;
    void drawTriangle(const Vtx& a, const Vtx& b, const Vtx& c, bool textured, bool gouraud, bool semiOn, bool raw, int clutX, int clutY);
    void drawRect(int x, int y, int w, int h, int r, int g, int b, bool textured, bool semiOn, bool raw, int u0, int v0, int clutX, int clutY);
    void drawLine(const Vtx& a, const Vtx& b, bool gouraud, bool semiOn);
    void fillRect(uint32_t color, uint32_t xy, uint32_t wh);
    void copyRect(uint32_t src, uint32_t dst, uint32_t wh);
    void putPixel(int x, int y, int r, int g, int b, bool semiOn, bool texSemi);
    uint16_t texel(int u, int v, int clutX, int clutY, bool& transparent) const;
    void setTexpage(uint32_t v);
    // recording helpers
    std::vector<std::array<int, 4>> fbRects;  // regions used as frame buffers (draw areas, display areas)
    void noteFramebuffer(int x, int y, int w, int h);
    bool touchesFramebuffer(int x, int y, int w, int h) const;
    void recordUpload(int x, int y, int w, int h);
    void recordTri(const Vtx& a, const Vtx& b, const Vtx& c, bool textured, bool semiOn, bool raw, int clutX, int clutY);
    void recordRect(int x, int y, int w, int h, int r, int g, int b, bool textured, bool semiOn, bool raw, int u0, int v0, int clutX, int clutY);
};

}  // namespace psx
