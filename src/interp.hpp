#pragma once
// 60 fps from a 30 fps game. Crash Bandicoot 2 draws a new picture every second vertical blank; the host renderer replays what the GPU was told to
// draw (Gpu::cmds), so the picture in between can be drawn too: every triangle of the newest frame is looked up in the frame before it (same texture,
// same texel coordinates, same colour - a polygon keeps those from frame to frame) and drawn half way between the two screen positions. Triangles that
// have no partner, or whose partner is far away (a camera cut, a fade), are drawn where the newest frame has them.
//
// Frames are delimited by the display flips (Gpu::Kind::Flip): what was drawn between two flips is the picture the second flip puts on screen.
// Frames that contain framebuffer uploads (videos, loading pictures, render-to-texture effects) are never interpolated.
#include <cstdint>
#include <string>
#include <vector>

#include "gpu.hpp"

namespace psx {

class FrameInterpolator {
public:
    struct Frame {
        std::vector<GpuCmd> cmds;        // Tri, Fill and Text commands drawn into the buffer that became the display at the flip
        std::vector<std::string> texts;  // what the Text commands' poolOffset points into
        int ox = 0, oy = 0, w = 0, h = 0;   // the display area after the flip
        bool usable = true;              // false: the frame contains uploads
    };
    struct Stats { size_t triangles = 0, matched = 0; };

    // Called once per pass of the main loop with the commands recorded in it (before the renderer clears them). Returns true when a flip happened in this
    // pass: a new frame is on screen. The flips carry the number of the vertical blank they happened after, which is what the frame period is measured in.
    bool feed(const std::vector<GpuCmd>& cmds, const std::vector<std::string>& textPool);
    // The vertical blank (mod 2^32) the newest flip happened after.
    uint32_t flipStamp() const { return lastFlipAt; }
    // The frame that just flipped in can be preceded by an in-between picture: a previous frame exists, both are usable and the game is running at
    // one picture per two vertical blanks (otherwise there is no gap to fill).
    bool canInterpolate() const { return flipped && prev.usable && cur.usable && havePrev && period == 2 && prev.w == cur.w && prev.h == cur.h; }
    // The picture `t` of the way from the newest frame (0) back to the one before it (1), in the newest frame's coordinates and drawing order.
    void build(float t, std::vector<GpuCmd>& out, std::vector<std::string>& texts, Stats* stats = nullptr) const;
    // Why the newest flip has no in-between picture (for logs): "" when it has one.
    const char* whyNot() const {
        if (!flipped) return "no flip";
        if (!havePrev) return "first frame";
        if (!prev.usable) return "previous frame has uploads";
        if (!cur.usable) return "frame has uploads";
        if (period != 2) return period < 2 ? "period 1" : "period 3 or more";
        if (prev.w != cur.w || prev.h != cur.h) return "display size changed";
        return "";
    }
    void reset() { const float g = grow; *this = FrameInterpolator(); grow = g; }
    float grow = 0.35f;   // opaque triangles of the in-between picture are made this many pixels bigger so neighbours overlap (no hairline gaps)

    // Largest distance (VRAM pixels) a vertex may have moved between two frames for the triangles to count as the same one, and the largest
    // difference between the movements of a triangle's three vertices.
    static constexpr float kMaxMove = 80.0f, kMaxSpread = 40.0f;

private:
    Frame acc, cur, prev;
    bool havePrev = false, flipped = false, started = false;
    uint32_t lastFlipAt = 0;
    uint32_t period = 0;   // vertical blanks between the last two flips
};

}  // namespace psx
