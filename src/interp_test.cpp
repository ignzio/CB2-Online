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

static GpuCmd flip(int x, int y, uint32_t stamp) {
    GpuCmd f;
    f.kind = GpuCmd::Kind::Flip;
    f.color = stamp;
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
    fi.grow = 0;   // the positions below are exact
    std::vector<GpuCmd> out;
    std::vector<std::string> texts;

    // Frame A is drawn, flips in at vblank 10; frame B (the other buffer, moved by 6 pixels) is drawn and flips in at vblank 12.
    auto a = frame(512, 0, 5);
    a.push_back(flip(512, 0, 10));
    CHECK(fi.feed(a, none));
    CHECK(!fi.canInterpolate());   // nothing before it yet
    auto b = frame(0, 6, 5);
    b.push_back(flip(0, 0, 12));
    CHECK(fi.feed(b, none));
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
    c1.push_back(flip(512, 0, 13));
    CHECK(fi.feed(c1, none));
    CHECK(!fi.canInterpolate());
    // a frame period of three blanks does not either
    auto c2 = frame(0, 18, 5);
    c2.push_back(flip(0, 0, 16));
    CHECK(fi.feed(c2, none));
    CHECK(!fi.canInterpolate());
    auto c3 = frame(512, 24, 5);
    c3.push_back(flip(512, 0, 18));
    CHECK(fi.feed(c3, none));
    CHECK(fi.canInterpolate());

    // a pass without a flip is not a new frame; the commands wait for the next flip, even across passes
    auto d1 = frame(0, 30, 3);
    CHECK(!fi.feed(d1, none));
    CHECK(!fi.canInterpolate());
    auto d2 = frame(0, 30, 2, false);
    d2.erase(d2.begin(), d2.begin() + 0);
    d2.push_back(flip(0, 0, 20));
    CHECK(fi.feed(d2, none));
    CHECK(fi.canInterpolate());
    fi.build(0.5f, out, texts, &st);
    CHECK(st.triangles == 5);   // 3 + 2

    // triangles that jumped far, or have no partner, stay where the newest frame has them
    auto e1 = frame(512, 0, 4);
    e1.push_back(flip(512, 0, 22));
    fi.feed(e1, none);
    auto e2 = frame(0, 0, 4);
    e2[2].x[0] += 300; e2[2].x[1] += 300; e2[2].x[2] += 300;   // far away
    e2.push_back(tri(400, 100, 99));                           // new
    e2.push_back(flip(0, 0, 24));
    fi.feed(e2, none);
    CHECK(fi.canInterpolate());
    fi.build(0.5f, out, texts, &st);
    CHECK(st.triangles == 5 && st.matched == 3);
    CHECK(std::fabs(out.back().x[0] - 400.0f) < 1e-3f);

    // a frame with an upload is never interpolated
    GpuCmd up;
    up.kind = GpuCmd::Kind::Upload;
    auto f1 = frame(512, 0, 3);
    f1.push_back(up);
    f1.push_back(flip(512, 0, 26));
    fi.feed(f1, none);
    auto f2 = frame(0, 3, 3);
    f2.push_back(flip(0, 0, 28));
    fi.feed(f2, none);
    CHECK(!fi.canInterpolate());   // the previous frame had an upload
    auto f3 = frame(512, 6, 3);
    f3.push_back(flip(512, 0, 30));
    fi.feed(f3, none);
    CHECK(fi.canInterpolate());

    // a polygon that is a sliver in one frame and a big shape in the other is not moved into the other
    auto h2 = frame(0, 0, 1);
    h2[1].y[2] = h2[1].y[0] + 0.5f;   // squashed to a sliver, same texture
    h2.push_back(flip(0, 0, 38));
    auto h1 = frame(512, 0, 1);
    h1[1].x[1] += 60; h1[1].y[2] += 60; h1[1].x[2] += 30;   // big in the earlier frame
    h1.push_back(flip(512, 0, 36));
    fi.reset();
    fi.feed(h1, none);
    fi.feed(h2, none);
    CHECK(fi.canInterpolate());
    fi.build(0.5f, out, texts, &st);
    CHECK(st.triangles == 1 && st.matched == 0);

    // a triangle without a partner follows the corners it shares with a matched one, so no gap opens between them
    {
        FrameInterpolator f2;
        f2.grow = 0;
        std::vector<GpuCmd> p1 = {tri(612, 100, 0)};   // the buffer at x = 512: the triangle sits at (100, 100) on screen
        p1.push_back(flip(512, 0, 40));
        f2.feed(p1, none);
        GpuCmd b = tri(116, 100, 40);                  // new, no partner; its first corner is the second corner of the moved triangle below
        b.x[1] = 130; b.y[1] = 100; b.x[2] = 116; b.y[2] = 118;
        std::vector<GpuCmd> p2 = {tri(106, 100, 0), b};   // the same triangle 6 pixels to the right
        p2.push_back(flip(0, 0, 42));
        f2.feed(p2, none);
        CHECK(f2.canInterpolate());
        f2.build(0.5f, out, texts, &st);
        CHECK(st.triangles == 2 && st.matched == 1);
        CHECK(std::fabs(out[0].x[1] - 113.0f) < 1e-3f);   // matched: half way back
        CHECK(std::fabs(out[1].x[0] - 113.0f) < 1e-3f);   // the neighbour's shared corner moves with it
        CHECK(std::fabs(out[1].x[1] - 127.0f) < 1e-3f && std::fabs(out[1].x[2] - 113.0f) < 1e-3f);   // and its other corners come along, so its shape is kept
            }

            // two look-alike triangles (a left and a right glove): each pairs with the one that moved least, not with the one that comes first
    {
        FrameInterpolator f3;
        f3.grow = 0;
        std::vector<GpuCmd> q1 = {tri(612, 100, 0), tri(812, 100, 0)};   // prev: left at x=100, right at x=300 (buffer at 512)
        q1.push_back(flip(512, 0, 50));
        f3.feed(q1, none);
        std::vector<GpuCmd> q2 = {tri(304, 100, 0), tri(104, 100, 0)};   // now drawn in the opposite order, each moved 4 pixels
        q2.push_back(flip(0, 0, 52));
        f3.feed(q2, none);
        f3.build(1.0f, out, texts, &st);
        CHECK(st.matched == 2);
        CHECK(std::fabs(out[0].x[0] - 300.0f) < 1e-3f && std::fabs(out[1].x[0] - 100.0f) < 1e-3f);
    }

    // a wrong partner is dropped when its neighbour disagrees about a shared corner: two triangles share the corner (110,100); the first has a look-alike far away that
    // is closer in the drawing order but farther in space... here the look-alike of B (same texture coordinates) sits 40 pixels to the right in the previous frame
    {
        FrameInterpolator f4;
        f4.grow = 0;
        GpuCmd pa = tri(612, 100, 0), pb = tri(612, 100, 40), pdecoy = tri(612, 100, 40);
        pb.x[0] = 622; pb.y[0] = 100; pb.x[1] = 640; pb.y[1] = 100; pb.x[2] = 622; pb.y[2] = 118;      // B, shares (622,100) with A's second corner
        for (int v = 0; v < 3; ++v) { pdecoy.x[v] = pb.x[v] + 40.0f; pdecoy.y[v] = pb.y[v] + 5.0f; }   // a look-alike of B, 40 px away
        std::vector<GpuCmd> r1 = {pa, pdecoy, pb};
        r1.push_back(flip(512, 0, 60));
        f4.feed(r1, none);
        GpuCmd ca = tri(106, 100, 0), cb = pb;
        cb.x[0] = 116; cb.y[0] = 100; cb.x[1] = 134; cb.y[1] = 100; cb.x[2] = 116; cb.y[2] = 118;   // both moved 6 px right (ca at 106, cb at 116)
        std::vector<GpuCmd> r2 = {ca, cb};
        r2.push_back(flip(0, 0, 62));
        f4.feed(r2, none);
        f4.build(1.0f, out, texts, &st);
        CHECK(st.matched == 2);
        CHECK(std::fabs(out[0].x[0] - 100.0f) < 1e-3f && std::fabs(out[1].x[0] - 110.0f) < 1e-3f);   // B went to the nearer one, not the decoy
    }

    // text (name tags) moves with its string
    GpuCmd tag;
    tag.kind = GpuCmd::Kind::Text;
    tag.poolOffset = 0;
    tag.x[0] = 100; tag.y[0] = 50;
    auto g1 = frame(0, 0, 2);
    g1.push_back(tag);
    g1.push_back(flip(0, 0, 32));
    fi.feed(g1, {"ALICE"});
    GpuCmd tag2 = tag;
    tag2.x[0] = 512 + 108; tag2.y[0] = 54;
    auto g2 = frame(512, 8, 2);
    g2.push_back(tag2);
    g2.push_back(flip(512, 0, 34));
    fi.feed(g2, {"ALICE"});
    fi.build(0.5f, out, texts);
    CHECK(out.back().kind == GpuCmd::Kind::Text && texts[out.back().poolOffset] == "ALICE");
    CHECK(std::fabs(out.back().x[0] - (512 + 104)) < 1e-3f && std::fabs(out.back().y[0] - 52) < 1e-3f);

    std::printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
