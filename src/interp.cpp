#include "interp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

namespace psx {
namespace {

// The longest side of a triangle (Manhattan length).
float sizeOf(const GpuCmd& c) {
    auto side = [&](int a, int b) { return std::fabs(c.x[a] - c.x[b]) + std::fabs(c.y[a] - c.y[b]); };
    return std::max({side(0, 1), side(0, 2), side(1, 2)});
}

float areaOf(const GpuCmd& c) {
    return 0.5f * std::fabs((c.x[1] - c.x[0]) * (c.y[2] - c.y[0]) - (c.x[2] - c.x[0]) * (c.y[1] - c.y[0]));
}

// The same polygon one frame apart looks about the same size; a triangle that is a sliver in one frame and a big shape in the other is a different thing
// (a shadow that only some frames draw, a clipped polygon): moving one into the other would show a shape that exists in neither frame.
bool similarShape(const GpuCmd& a, const GpuCmd& b) {
    const float sa = sizeOf(a), sb = sizeOf(b);
    if (std::max(sa, sb) > 12.0f && std::max(sa, sb) > 1.7f * std::min(sa, sb)) return false;
    const float aa = areaOf(a), ab = areaOf(b);
    return !(std::max(aa, ab) > 40.0f && std::max(aa, ab) > 2.5f * std::min(aa, ab));
}

// A screen position (relative to the display area) as a map key, to 1/16 pixel.
uint64_t vertexKey(float x, float y) {
    return uint64_t(uint32_t(int32_t(std::lround(x * 16.0f)))) << 32 | uint32_t(int32_t(std::lround(y * 16.0f)));
}

uint64_t keyOf(const GpuCmd& c) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(uint64_t(c.textured) | uint64_t(c.raw) << 1 | uint64_t(c.semi) << 2 | uint64_t(c.semiMode) << 3 | uint64_t(c.depth) << 5 | uint64_t(c.tpx) << 7 | uint64_t(c.tpy) << 11);
    mix(uint64_t(c.clutX) | uint64_t(c.clutY) << 16 | uint64_t(c.alpha) << 32);
    mix(uint64_t(c.twMX) | uint64_t(c.twMY) << 8 | uint64_t(c.twOX) << 16 | uint64_t(c.twOY) << 24);
    for (int i = 0; i < 3; ++i) mix(uint64_t(uint32_t(int32_t(std::lround(c.u[i] * 4)))) | uint64_t(uint32_t(int32_t(std::lround(c.v[i] * 4)))) << 32);
    if (!c.textured) {   // flat triangles have nothing but colour and size to tell them apart
        for (int i = 0; i < 3; ++i) mix(uint64_t(c.col[i][0] >> 4) | uint64_t(c.col[i][1] >> 4) << 4 | uint64_t(c.col[i][2] >> 4) << 8);
        mix(uint64_t(std::ilogb(std::max(1.0f, sizeOf(c)))));
    }
    return h;
}

}  // namespace

bool FrameInterpolator::feed(const std::vector<GpuCmd>& cmds, const std::vector<std::string>& textPool) {
    flipped = false;
    for (const GpuCmd& c : cmds) {
        switch (c.kind) {
        case GpuCmd::Kind::Flip: {
            acc.ox = c.rx; acc.oy = c.ry; acc.w = c.rw; acc.h = c.rh;
            if (started) { prev = std::move(cur); havePrev = true; period = c.color - lastFlipAt; }
            cur = std::move(acc);
            acc = Frame();
            started = true;
            flipped = true;
            lastFlipAt = c.color;
            break;
        }
        case GpuCmd::Kind::Upload: acc.usable = false; break;
        case GpuCmd::Kind::Text: {
            GpuCmd t = c;
            t.poolOffset = uint32_t(acc.texts.size());
            acc.texts.push_back(c.poolOffset < textPool.size() ? textPool[c.poolOffset] : std::string());
            acc.cmds.push_back(t);
            break;
        }
        default: acc.cmds.push_back(c); break;
        }
    }
    if (acc.cmds.size() > 200000) acc = Frame();   // a scene that never flips (single buffered): do not grow without end
    return flipped;
}

void FrameInterpolator::build(float t, std::vector<GpuCmd>& out, std::vector<std::string>& texts, Stats* stats) const {
    out.clear();
    out.reserve(cur.cmds.size());
    texts = cur.texts;
    // the previous frame's triangles by what they look like; a cursor per look moves forward through them because the drawing order is stable
    std::unordered_map<uint64_t, std::vector<uint32_t>> byKey;
    byKey.reserve(prev.cmds.size());
    for (uint32_t i = 0; i < prev.cmds.size(); ++i)
        if (prev.cmds[i].kind == GpuCmd::Kind::Tri) byKey[keyOf(prev.cmds[i])].push_back(i);
    std::vector<uint8_t> used(prev.cmds.size(), 0);
    std::unordered_map<uint64_t, uint32_t> cursor;
    std::unordered_map<uint64_t, std::vector<std::array<float, 2>>> votes;
    std::vector<uint8_t> usedText(prev.texts.size(), 0);

    Stats local;
    for (const GpuCmd& c : cur.cmds) {
        if (c.kind == GpuCmd::Kind::Text) {
            GpuCmd o = c;
            for (size_t i = 0; i < prev.cmds.size(); ++i) {
                const GpuCmd& p = prev.cmds[i];
                if (p.kind != GpuCmd::Kind::Text || usedText[p.poolOffset] || prev.texts[p.poolOffset] != cur.texts[c.poolOffset]) continue;
                const float dx = (p.x[0] - float(prev.ox)) - (c.x[0] - float(cur.ox)), dy = (p.y[0] - float(prev.oy)) - (c.y[0] - float(cur.oy));
                if (std::fabs(dx) + std::fabs(dy) <= kMaxMove) { o.x[0] += dx * t; o.y[0] += dy * t; usedText[p.poolOffset] = 1; }
                break;
            }
            out.push_back(o);
            continue;
        }
        if (c.kind != GpuCmd::Kind::Tri) { out.push_back(c); continue; }
        ++local.triangles;
        GpuCmd o = c;
        const uint64_t key = keyOf(c);
        const auto found = byKey.find(key);
        if (found != byKey.end()) {
            const std::vector<uint32_t>& list = found->second;
            uint32_t& cur0 = cursor[key];
            const uint32_t from = cur0 > 4 ? cur0 - 4 : 0, to = std::min<uint32_t>(uint32_t(list.size()), cur0 + 40);
            float best = 1e30f;
            const float maxSpread = std::min(kMaxSpread, std::max(6.0f, 0.6f * sizeOf(c)));   // a small triangle cannot have been stretched much
            uint32_t bestAt = 0;
            float bd[3][2] = {};
            for (uint32_t j = from; j < to; ++j) {
                if (used[list[j]]) continue;
                const GpuCmd& p = prev.cmds[list[j]];
                if (!similarShape(c, p)) continue;
                float d[3][2], mx = 0, my = 0, cost = 0;
                for (int v = 0; v < 3; ++v) {
                    d[v][0] = (p.x[v] - float(prev.ox)) - (c.x[v] - float(cur.ox));
                    d[v][1] = (p.y[v] - float(prev.oy)) - (c.y[v] - float(cur.oy));
                    mx += d[v][0] / 3; my += d[v][1] / 3;
                    cost += std::fabs(d[v][0]) + std::fabs(d[v][1]);
                }
                float spread = 0, move = 0;
                for (int v = 0; v < 3; ++v) {
                    spread = std::max(spread, std::fabs(d[v][0] - mx) + std::fabs(d[v][1] - my));
                    move = std::max(move, std::fabs(d[v][0]) + std::fabs(d[v][1]));
                }
                if (move > kMaxMove || spread > maxSpread || cost >= best) continue;
                best = cost; bestAt = j;
                std::copy(&d[0][0], &d[0][0] + 6, &bd[0][0]);
            }
            if (best < 1e29f) {
                used[list[bestAt]] = 1;
                cur0 = bestAt + 1;
                ++local.matched;
                for (int v = 0; v < 3; ++v) votes[vertexKey(c.x[v] - float(cur.ox), c.y[v] - float(cur.oy))].push_back({bd[v][0], bd[v][1]});
            }
        }
        out.push_back(o);
    }

    // One movement per screen vertex: the median of what the matched triangles around it found. Triangles that share a corner then always move together (a
    // wrong partner is outvoted, and no gap opens between neighbours), and a triangle without a partner follows the corners it has in common with matched ones.
    std::unordered_map<uint64_t, std::array<float, 2>> field;
    field.reserve(votes.size());
    for (auto& [key, list] : votes) {
        std::array<float, 2> m{};
        for (int axis = 0; axis < 2; ++axis) {
            const size_t mid = list.size() / 2;
            std::nth_element(list.begin(), list.begin() + long(mid), list.end(), [&](const std::array<float, 2>& a, const std::array<float, 2>& b) { return a[size_t(axis)] < b[size_t(axis)]; });
            m[size_t(axis)] = list[mid][size_t(axis)];
        }
        field[key] = m;
    }
    for (GpuCmd& o : out) {
        if (o.kind != GpuCmd::Kind::Tri) continue;
        float nx[3], ny[3];
        for (int v = 0; v < 3; ++v) {
            nx[v] = o.x[v]; ny[v] = o.y[v];
            const auto it = field.find(vertexKey(o.x[v] - float(cur.ox), o.y[v] - float(cur.oy)));
            if (it != field.end()) { nx[v] += it->second[0] * t; ny[v] += it->second[1] * t; }
        }
        for (int v = 0; v < 3; ++v) { o.x[v] = nx[v]; o.y[v] = ny[v]; }
    }
    if (stats) *stats = local;
}

}  // namespace psx
