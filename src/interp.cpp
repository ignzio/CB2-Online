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
            prepared = false;
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

// The matching of the newest frame against the one before it: what every triangle of the newest frame does to get to where it was. It depends on the pair only, so it is done once per
// flip (prepare) and every picture in between (build) just scales the movements by its own "how far back".
void FrameInterpolator::prepare() const {
    std::vector<GpuCmd> out = cur.cmds;   // the geometry the matching reads (never drawn)
    vdisp.assign(cur.cmds.size(), std::array<float, 6>{});
    vgrow.assign(cur.cmds.size(), 0);
    textMoves.clear();
    prepared = true;

    // ---- name tags: follow the same string
    {
        std::vector<uint8_t> usedText(prev.texts.size(), 0);
        for (uint32_t oi = 0; oi < out.size(); ++oi) {
            GpuCmd& o = out[oi];
            if (o.kind != GpuCmd::Kind::Text) continue;
            for (const GpuCmd& p : prev.cmds) {
                if (p.kind != GpuCmd::Kind::Text || usedText[p.poolOffset] || prev.texts[p.poolOffset] != cur.texts[o.poolOffset]) continue;
                const float dx = (p.x[0] - float(prev.ox)) - (o.x[0] - float(cur.ox)), dy = (p.y[0] - float(prev.oy)) - (o.y[0] - float(cur.oy));
                if (std::fabs(dx) + std::fabs(dy) <= kMaxMove) { textMoves.push_back({float(oi), dx, dy}); usedText[p.poolOffset] = 1; }
                break;
            }
        }
    }

    // ---- triangles: find each one's partner in the previous frame
    struct Match { uint32_t curIdx = 0; float d[3][2] = {}; float cost = 0; bool alive = true; };
    std::vector<Match> matches;
    std::unordered_map<uint64_t, std::vector<uint32_t>> curBy, prevBy;
    Stats local;
    for (uint32_t i = 0; i < cur.cmds.size(); ++i) if (cur.cmds[i].kind == GpuCmd::Kind::Tri) { curBy[keyOf(cur.cmds[i])].push_back(i); ++local.triangles; if (cur.cmds[i].semi) ++local.semi; }
    for (uint32_t i = 0; i < prev.cmds.size(); ++i) if (prev.cmds[i].kind == GpuCmd::Kind::Tri) prevBy[keyOf(prev.cmds[i])].push_back(i);

    struct Pair { float cost; uint32_t ci, pi; float d[3][2]; };
    std::vector<Pair> pairs;
    for (auto& [key, cl] : curBy) {
        const auto pf = prevBy.find(key);
        if (pf == prevBy.end()) continue;
        const std::vector<uint32_t>& pl = pf->second;
        pairs.clear();
        const bool big = uint64_t(cl.size()) * pl.size() > 40000;   // many look-alikes (flat shapes): only compare those at a similar place in the drawing order
        for (size_t a = 0; a < cl.size(); ++a) {
            const GpuCmd& c = cur.cmds[cl[a]];
            const float maxSpread = std::min(kMaxSpread, std::max(6.0f, 0.6f * sizeOf(c)));
            size_t from = 0, to = pl.size();
            if (big) {
                const size_t mid = a * pl.size() / cl.size();
                from = mid > 150 ? mid - 150 : 0; to = std::min(pl.size(), mid + 150);
            }
            for (size_t b = from; b < to; ++b) {
                const GpuCmd& p = prev.cmds[pl[b]];
                if (!similarShape(c, p)) continue;
                Pair pr{};
                pr.ci = cl[a]; pr.pi = pl[b];
                float mx = 0, my = 0;
                for (int v = 0; v < 3; ++v) {
                    pr.d[v][0] = (p.x[v] - float(prev.ox)) - (c.x[v] - float(cur.ox));
                    pr.d[v][1] = (p.y[v] - float(prev.oy)) - (c.y[v] - float(cur.oy));
                    mx += pr.d[v][0] / 3; my += pr.d[v][1] / 3;
                    pr.cost += std::fabs(pr.d[v][0]) + std::fabs(pr.d[v][1]);
                }
                float spread = 0, move = 0;
                for (int v = 0; v < 3; ++v) {
                    spread = std::max(spread, std::fabs(pr.d[v][0] - mx) + std::fabs(pr.d[v][1] - my));
                    move = std::max(move, std::fabs(pr.d[v][0]) + std::fabs(pr.d[v][1]));
                }
                // Effects drawn see-through (water surfaces, whirlpools, glass, smoke, shadows) are meshes whose corners swirl and morph; a partner found for one of them is often a
                // look-alike a few triangles away and the half-way shape is garbage (shards). They are only moved when they translate almost rigidly.
                if (c.semi && (spread > kSemiSpread || move > 40.0f)) continue;
                if (move <= kMaxMove && spread <= maxSpread) pairs.push_back(pr);
            }
        }
        // the closest pairs first: the polygon that moved least is the likeliest partner (a left glove and a right glove have the same texture coordinates)
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.cost < y.cost; });
        std::unordered_map<uint32_t, uint8_t> usedCur, usedPrev;
        for (const Pair& pr : pairs) {
            if (usedCur.count(pr.ci) || usedPrev.count(pr.pi)) continue;
            usedCur[pr.ci] = 1; usedPrev[pr.pi] = 1;
            Match m;
            m.curIdx = pr.ci; m.cost = pr.cost;
            std::copy(&pr.d[0][0], &pr.d[0][0] + 6, &m.d[0][0]);
            matches.push_back(m);
        }
    }

    // ---- neighbours must agree: two matched triangles that share a screen corner must have seen that corner go to the same place. When they do not, one of them
    // has the wrong partner; the one that moved farther is the likelier culprit and is dropped (it then follows its neighbours instead).
    constexpr float kAgree = 3.0f;
    for (int round = 0; round < 4; ++round) {
        std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint8_t>>> at;   // corner -> (match, vertex)
        for (uint32_t m = 0; m < matches.size(); ++m) {
            if (!matches[m].alive) continue;
            const GpuCmd& c = cur.cmds[matches[m].curIdx];
            for (uint8_t v = 0; v < 3; ++v) at[vertexKey(c.x[v] - float(cur.ox), c.y[v] - float(cur.oy))].push_back({m, v});
        }
        bool dropped = false;
        for (auto& [key, list] : at) {
            if (list.size() < 2) continue;
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (const auto& e : list) for (int k = 0; k < 2; ++k) { lo[k] = std::min(lo[k], matches[e.first].d[e.second][k]); hi[k] = std::max(hi[k], matches[e.first].d[e.second][k]); }
            if (hi[0] - lo[0] <= kAgree && hi[1] - lo[1] <= kAgree) continue;
            // majority first: the movement most of the votes are near
            uint32_t worst = list[0].first;
            float worstScore = -1;
            for (const auto& e : list) {
                int friends = 0;
                for (const auto& o : list)
                    if (std::fabs(matches[o.first].d[o.second][0] - matches[e.first].d[e.second][0]) <= kAgree && std::fabs(matches[o.first].d[o.second][1] - matches[e.first].d[e.second][1]) <= kAgree) ++friends;
                const float score = float(list.size() - size_t(friends)) * 1000.0f + matches[e.first].cost;   // few friends, then large movement
                if (score > worstScore) { worstScore = score; worst = e.first; }
            }
            matches[worst].alive = false;
            dropped = true;
        }
        if (!dropped) break;
    }

    // ---- one movement per corner (the votes of all surviving matches around it agree, so any of them will do)
    std::unordered_map<uint64_t, std::array<float, 2>> field;
    for (const Match& m : matches) {
        if (!m.alive) continue;
        ++local.matched;
        const GpuCmd& c = cur.cmds[m.curIdx];
        if (c.semi) ++local.semiMatched;
        for (int v = 0; v < 3; ++v) field[vertexKey(c.x[v] - float(cur.ox), c.y[v] - float(cur.oy))] = {m.d[v][0], m.d[v][1]};
    }
    std::vector<uint8_t> isMatched(cur.cmds.size(), 0);
    for (const Match& m : matches) if (m.alive) isMatched[m.curIdx] = 1;
    // the matched triangles by place (centroid and mean movement), for the ones that found no partner and share no corner with a matched one
    constexpr float kCell = 32.0f;
    struct Near { float x, y, dx, dy; };
    auto cellKey = [](int gx, int gy) { return uint64_t(uint32_t(gx + 4096)) << 32 | uint32_t(gy + 4096); };
    std::unordered_map<uint64_t, std::vector<Near>> grid;
    for (const Match& m : matches) {
        if (!m.alive) continue;
        const GpuCmd& c = cur.cmds[m.curIdx];
        Near nb;
        nb.x = (c.x[0] + c.x[1] + c.x[2]) / 3.0f - float(cur.ox); nb.y = (c.y[0] + c.y[1] + c.y[2]) / 3.0f - float(cur.oy);
        nb.dx = (m.d[0][0] + m.d[1][0] + m.d[2][0]) / 3.0f; nb.dy = (m.d[0][1] + m.d[1][1] + m.d[2][1]) / 3.0f;
        grid[cellKey(int(std::floor(nb.x / kCell)), int(std::floor(nb.y / kCell)))].push_back(nb);
    }
    for (uint32_t i = 0; i < out.size(); ++i) {
        GpuCmd& o = out[i];
        if (o.kind != GpuCmd::Kind::Tri) continue;
        const GpuCmd& c = cur.cmds[i];
        float mx = 0, my = 0;
        int n = 0;
        float dv[3][2] = {};
        bool has[3] = {};
        for (int v = 0; v < 3; ++v) {
            const auto it = field.find(vertexKey(c.x[v] - float(cur.ox), c.y[v] - float(cur.oy)));
            if (it == field.end()) continue;
            dv[v][0] = it->second[0]; dv[v][1] = it->second[1]; has[v] = true;
            mx += dv[v][0]; my += dv[v][1]; ++n;
        }
        // See-through effects without a trustworthy partner stay where the newest frame has them, unless all three corners are corners of geometry that did move (the water,
        // ice and glass layers drawn over a floor reuse the floor's own vertices): then they move with it, or their edges would no longer line up with the floor under them.
        const bool followsGeometry = c.semi && !isMatched[i] && n == 3;
        if (c.semi && !isMatched[i] && !followsGeometry) continue;
        if (n == 0) {   // no corner in common with a matched triangle: take the movement of the matched ones around it, nearest first
            const float cx = (c.x[0] + c.x[1] + c.x[2]) / 3.0f - float(cur.ox), cy = (c.y[0] + c.y[1] + c.y[2]) / 3.0f - float(cur.oy);
            float sx = 0, sy = 0, sw = 0;
            const int gx = int(std::floor(cx / kCell)), gy = int(std::floor(cy / kCell));
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const auto it = grid.find(cellKey(gx + dx, gy + dy));
                    if (it == grid.end()) continue;
                    for (const Near& nb : it->second) {
                        const float dist = std::sqrt((nb.x - cx) * (nb.x - cx) + (nb.y - cy) * (nb.y - cy));
                        if (dist > 2.0f * kCell) continue;
                        const float w = 1.0f / (dist + 6.0f);
                        sx += nb.dx * w; sy += nb.dy * w; sw += w;
                    }
                }
            if (sw > 0) for (int v = 0; v < 3; ++v) { vdisp[i][size_t(2 * v)] = sx / sw; vdisp[i][size_t(2 * v + 1)] = sy / sw; }
            continue;
        }
        if (isMatched[i] || followsGeometry) {   // all three corners move on their own, the way the partner shows
            for (int v = 0; v < 3; ++v) { vdisp[i][size_t(2 * v)] = dv[v][0]; vdisp[i][size_t(2 * v + 1)] = dv[v][1]; }
        } else {              // no partner: move as a whole, with the average of the corners it shares with others, so its shape is kept
            for (int v = 0; v < 3; ++v) { vdisp[i][size_t(2 * v)] = mx / float(n); vdisp[i][size_t(2 * v + 1)] = my / float(n); }
        }
        vgrow[i] = 1;
    }
    cachedStats = local;
}

void FrameInterpolator::build(float t, std::vector<GpuCmd>& out, std::vector<std::string>& texts, Stats* stats) const {
    if (!prepared) prepare();
    out = cur.cmds;
    texts = cur.texts;
    for (const auto& m : textMoves) { GpuCmd& o = out[size_t(m[0])]; o.x[0] += m[1] * t; o.y[0] += m[2] * t; }
    for (size_t i = 0; i < out.size(); ++i) {
        GpuCmd& o = out[i];
        if (o.kind != GpuCmd::Kind::Tri) continue;
        const auto& d = vdisp[i];
        for (int v = 0; v < 3; ++v) { o.x[v] += d[size_t(2 * v)] * t; o.y[v] += d[size_t(2 * v + 1)] * t; }
        // Neighbours that moved by different amounts can leave a hairline between them (the real picture shows through as a dark thread): opaque triangles are
        // made a little bigger (grow) so they overlap by that much. Blended ones are left alone, they would be blended twice.
        if (vgrow[i] && grow > 0.0f && t > 0.0f && !o.semi && o.alpha == 255) {
            const float cx = (o.x[0] + o.x[1] + o.x[2]) / 3.0f, cy = (o.y[0] + o.y[1] + o.y[2]) / 3.0f;
            for (int v = 0; v < 3; ++v) {
                const float dx = o.x[v] - cx, dy = o.y[v] - cy, len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0f) { o.x[v] += dx / len * grow; o.y[v] += dy / len * grow; }
            }
        }
    }
    if (stats) *stats = cachedStats;
}

}  // namespace psx
