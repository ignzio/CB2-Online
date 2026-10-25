// Test for the frame interpolator (src/interp.cpp): frame splitting at display flips, matching of triangles between frames and the in-between positions.
#include <cmath>
#include <cstdio>

#include "interp.hpp"

using namespace psx;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static GpuCmd tri(float x, float y, float u, float size = 10) {
    GpuCmd c;
    c.kind = GpuCmd::Kind::Tri;
    c.textured = true;
    c.tpx = 5; c.clutX = 16; c.clutY = 480;
    c.x[0] = x; c.y[0] = y; c.x[1] = x + size; c.y[1] = y; c.x[2] = x; c.y[2] = y + size;
    c.u[0] = u; c.u[1] = u + 8; c.u[2] = u;
    c.v[0] = 0; c.v[1] = 0; c.v[2] = 8;
    return c;
}

static GpuCmd flip(int x, int y) {
    GpuCmd f;
    f.kind = GpuCmd::Kind::Flip;
    f.rx = int16_t(x); f.ry = int16_t(y); f.rw = 512; f.rh = 240;
    return f;
}

// A frame for the buffer at (ox, 0): `n` triangles on a row, shifted by `shift`.
static std::vector<GpuCmd> frame(int ox, float shift, int n, bool withFill = true) {
    std::vector<GpuCmd> v;
    if (withFill) { GpuCmd f; f.kind = GpuCmd::Kind::Fill; f.rx = int16_t(ox); f.rw = 512; f.rh = 240; v.push_back(f); }
    for (int i = 0; i < n; ++i) v.push_back(tri(float(ox) + 20.0f * float(i) + shift, 30.0f + shift * 0.5f, float(i * 8)));
    return v;
}

int main() {
    const std::vector<std::string> none;
    FrameInterpolator fi;
    std::vector<GpuCmd> out;
    std::vector<std::string> texts;

    // Frame A is drawn, flips in at vblank 10; frame B (the other buffer, moved by 6 pixels) is drawn and flips in at vblank 12.
    auto a = frame(512, 0, 5);
    a.push_back(flip(512, 0));
    CHECK(fi.feed(a, none, 10));
    CHECK(!fi.canInterpolate());   // nothing before it yet
    auto b = frame(0, 6, 5);
    b.push_back(flip(0, 0));
    CHECK(fi.feed(b, none, 12));
    CHECK(fi.canInterpolate());

    FrameInterpolator::Stats st;
    fi.build(0.5f, out, texts, &st);
    CHECK(st.triangles == 5 && st.matched == 5);
    int n = 0;
    for (const GpuCmd& c : out) if (c.kind == GpuCmd::Kind::Tri) {
        // half way back from B's position (6 px right, 3 px down of A's) is 3 px right of A's and 1.5 px down
        CHECK(std::fabs(c.x[0] - (20.0f * float(n) + 3.0f)) < 1e-3f);
        CHECK(std::fabs(c.y[0] - (30.0f + 1.5f)) < 1e-3f);
        ++n;
    }
    CHECK(n == 5);
    // t = 0 is the newest frame itself, t = 1 the one before
    fi.build(0.0f, out, texts);
    n = 0;
    for (const GpuCmd& c : out) if (c.kind == GpuCmd::Kind::Tri) { CHECK(std::fabs(c.x[0] - (20.0f * float(n) + 6.0f)) < 1e-3f); ++n; }
    fi.build(1.0f, out, texts);
    n = 0;
    for (const GpuCmd& c : out) if (c.kind == GpuCmd::Kind::Tri) { CHECK(std::fabs(c.x[0] - 20.0f * float(n)) < 1e-3f); ++n; }

    // a frame period of one blank leaves no gap
    auto c1 = frame(512, 12, 5);
    c1.push_back(flip(512, 0));
    CHECK(fi.feed(c1, none, 13));
    CHECK(!fi.canInterpolate());
    // a frame period of three blanks does not either
    auto c2 = frame(0, 18, 5);
    c2.push_back(flip(0, 0));
    CHECK(fi.feed(c2, none, 16));
    CHECK(!fi.canInterpolate());
    auto c3 = frame(512, 24, 5);
    c3.push_back(flip(512, 0));
    CHECK(fi.feed(c3, none, 18));
    CHECK(fi.canInterpolate());

    // a pass without a flip is not a new frame; the commands wait for the next flip, even across passes
    auto d1 = frame(0, 30, 3);
    CHECK(!fi.feed(d1, none, 19));
    CHECK(!fi.canInterpolate());
    auto d2 = frame(0, 30, 2, false);
    d2.erase(d2.begin(), d2.begin() + 0);
    d2.push_back(flip(0, 0));
    CHECK(fi.feed(d2, none, 20));
    CHECK(fi.canInterpolate());
    fi.build(0.5f, out, texts, &st);
    CHECK(st.triangles == 5);   // 3 + 2

    // triangles that jumped far, or have no partner, stay where the newest frame has them
    auto e1 = frame(512, 0, 4);
    e1.push_back(flip(512, 0));
    fi.feed(e1, none, 22);
    auto e2 = frame(0, 0, 4);
    e2[2].x[0] += 300; e2[2].x[1] += 300; e2[2].x[2] += 300;   // far away
    e2.push_back(tri(400, 100, 99));                           // new
    e2.push_back(flip(0, 0));
    fi.feed(e2, none, 24);
    CHECK(fi.canInterpolate());
    fi.build(0.5f, out, texts, &st);
    CHECK(st.triangles == 5 && st.matched == 3);
    CHECK(std::fabs(out.back().x[0] - 400.0f) < 1e-3f);

    // a frame with an upload is never interpolated
    GpuCmd up;
    up.kind = GpuCmd::Kind::Upload;
    auto f1 = frame(512, 0, 3);
    f1.push_back(up);
    f1.push_back(flip(512, 0));
    fi.feed(f1, none, 26);
    auto f2 = frame(0, 3, 3);
    f2.push_back(flip(0, 0));
    fi.feed(f2, none, 28);
    CHECK(!fi.canInterpolate());   // the previous frame had an upload
    auto f3 = frame(512, 6, 3);
    f3.push_back(flip(512, 0));
    fi.feed(f3, none, 30);
    CHECK(fi.canInterpolate());

    // text (name tags) moves with its string
    GpuCmd tag;
    tag.kind = GpuCmd::Kind::Text;
    tag.poolOffset = 0;
    tag.x[0] = 100; tag.y[0] = 50;
    auto g1 = frame(0, 0, 2);
    g1.push_back(tag);
    g1.push_back(flip(0, 0));
    fi.feed(g1, {"ALICE"}, 32);
    GpuCmd tag2 = tag;
    tag2.x[0] = 512 + 108; tag2.y[0] = 54;
    auto g2 = frame(512, 8, 2);
    g2.push_back(tag2);
    g2.push_back(flip(512, 0));
    fi.feed(g2, {"ALICE"}, 34);
    fi.build(0.5f, out, texts);
    CHECK(out.back().kind == GpuCmd::Kind::Text && texts[out.back().poolOffset] == "ALICE");
    CHECK(std::fabs(out.back().x[0] - (512 + 104)) < 1e-3f && std::fabs(out.back().y[0] - 52) < 1e-3f);

    std::printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
