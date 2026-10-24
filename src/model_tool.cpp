// cb2_models <NSF>: lists the models and animations of a level and checks that every frame decodes.
#include <cstdio>
#include <map>

#include "model.hpp"
#include "nsf.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_models <level.NSF>\n"); return 2; }
    try {
        const cb2::Nsf nsf = cb2::loadNsf(argv[1]);
        std::map<uint32_t, cb2::Model> models;
        for (const auto& e : nsf.entries)
            if (e.type == 2) {
                if (auto m = cb2::parseModel(e)) {
                    std::printf("model %s: %zu triangles, %d vertices%s, %zu textures, scale %d %d %d\n", cb2::eidName(e.eid).c_str(), m->tris.size(),
                                m->vertexCount, m->compressed ? " (compressed)" : "", m->textures.size(), m->scale[0], m->scale[1], m->scale[2]);
                    models.emplace(e.eid, std::move(*m));
                } else {
                    std::printf("model %s: not understood (%zu items)\n", cb2::eidName(e.eid).c_str(), e.items.size());
                }
            }
        int bad = 0, total = 0;
        for (const auto& e : nsf.entries)
            if (e.type == 1) {
                const auto a = cb2::parseAnimation(e);
                if (!a) { std::printf("anim %s: not understood\n", cb2::eidName(e.eid).c_str()); continue; }
                const auto mi = models.find(a->frames[0].modelEid);
                int ok = 0;
                std::vector<std::array<int, 3>> v;
                for (const auto& f : a->frames) {
                    const auto m = models.find(f.modelEid);
                    ++total;
                    if (m != models.end() && cb2::frameVertices(f, m->second, v)) ++ok; else ++bad;
                }
                std::printf("anim %s: %zu frames, model %s%s, %d decode\n", cb2::eidName(e.eid).c_str(), a->frames.size(),
                            cb2::eidName(a->frames[0].modelEid).c_str(), mi == models.end() ? " (missing)" : "", ok);
            }
        std::printf("frames decoded: %d ok, %d failed\n", total - bad, bad);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
