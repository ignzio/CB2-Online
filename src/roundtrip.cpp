// Round-trip test for the NSF/NSD writers, run over every level in a folder:
//  1. each page's stored checksum equals pageChecksum()
//  2. each entry page, re-packed from its parsed entries, is byte-identical to the original
//  3. the pages written as an uncompressed NSF and loaded back are identical to the originals
//  4. the NSD regenerated from the pages is byte-identical to the shipped NSD
// Exit status is non-zero on any mismatch.
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "nsd.hpp"
#include "nsf.hpp"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path root = argc > 1 ? argv[1] : ".";
    const fs::path tmp = fs::temp_directory_path() / "cb2_roundtrip.nsf";
    size_t files = 0, pages = 0, entryPages = 0, nsds = 0, bad = 0;

    for (const auto& de : fs::recursive_directory_iterator(root)) {
        std::string ext = de.path().extension().string();
        for (auto& c : ext) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (!de.is_regular_file() || ext != ".NSF") continue;
        ++files;
        try {
            const std::vector<cb2::Bytes> original = cb2::loadPages(de.path().string());
            for (size_t i = 0; i < original.size(); ++i) {
                const cb2::Bytes& page = original[i];
                ++pages;
                uint32_t stored;
                std::memcpy(&stored, page.data() + 12, 4);
                if (cb2::pageChecksum(page.data()) != stored) {
                    ++bad;
                    std::fprintf(stderr, "checksum mismatch %s page %zu\n", de.path().string().c_str(), i);
                }
                uint16_t family;
                std::memcpy(&family, page.data() + 2, 2);
                if (family != 0) continue;
                ++entryPages;
                uint32_t id;
                std::memcpy(&id, page.data() + 4, 4);
                const cb2::Bytes rebuilt = cb2::packEntryPage(id, cb2::readEntryPage(page));
                if (rebuilt != page) {
                    ++bad;
                    std::fprintf(stderr, "repack mismatch %s page %zu\n", de.path().string().c_str(), i);
                }
            }
            cb2::saveFile(tmp.string(), cb2::writeNsf(original));
            if (cb2::loadPages(tmp.string()) != original) {
                ++bad;
                std::fprintf(stderr, "file round trip mismatch %s\n", de.path().string().c_str());
            }

            fs::path nsdPath = de.path();
            nsdPath.replace_extension(fs::exists(fs::path(de.path()).replace_extension(".NSD")) ? ".NSD" : ".nsd");
            const cb2::Bytes shipped = cb2::loadFile(nsdPath.string());
            ++nsds;
            if (cb2::rebuildNsd(shipped, original) != shipped) {
                ++bad;
                std::fprintf(stderr, "NSD rebuild mismatch %s\n", nsdPath.string().c_str());
            }
        } catch (const std::exception& ex) {
            ++bad;
            std::fprintf(stderr, "FAIL %s: %s\n", de.path().string().c_str(), ex.what());
        }
    }
    fs::remove(tmp);
    std::printf("files=%zu pages=%zu entry pages repacked=%zu NSDs rebuilt=%zu failures=%zu\n", files, pages, entryPages,
                nsds, bad);
    return (files == 0 || bad) ? 1 : 0;
}
