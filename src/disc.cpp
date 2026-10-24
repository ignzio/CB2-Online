#include "disc.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace psx {
namespace fs = std::filesystem;
namespace {

void put32both(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) { p[i] = uint8_t(v >> (8 * i)); p[7 - i] = uint8_t(v >> (8 * i)); }
}
void put16both(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint8_t bcd(uint32_t v) { return uint8_t((v / 10) << 4 | (v % 10)); }
std::string upper(std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }

std::vector<uint8_t> dirRecord(const std::string& name, bool special, uint8_t specialByte, uint32_t lba, uint32_t size, bool dir) {
    const size_t nameLen = special ? 1 : name.size();
    size_t len = 33 + nameLen;
    if (len & 1) ++len;
    std::vector<uint8_t> r(len, 0);
    r[0] = uint8_t(len);
    put32both(&r[2], lba);
    put32both(&r[10], size);
    r[18] = 96; r[19] = 1; r[20] = 1;  // 1996-01-01
    r[25] = dir ? 2 : 0;
    put16both(&r[28], 1);
    r[32] = uint8_t(nameLen);
    if (special) r[33] = specialByte; else std::memcpy(&r[33], name.data(), name.size());
    return r;
}

}  // namespace

bool VirtualDisc::build(const std::string& rootFolder, const std::vector<std::string>& overlays, const std::string& volumeId) {
    if (!fs::is_directory(rootFolder)) return false;
    // relative path (upper case, '\' separated) -> host file; later folders replace earlier ones
    std::map<std::string, FileRef> files;
    auto scan = [&](auto&& self, const fs::path& folder, const std::string& rel, int depth, bool isRoot) -> void {
        for (auto& e : fs::directory_iterator(folder)) {
            const std::string n = upper(e.path().filename().string());
            if (e.is_directory()) {
                if (depth >= 7 || n[0] == '.' || (isRoot && n == "CB2-VIEWER")) continue;
                self(self, e.path(), rel + n + "\\", depth + 1, false);
            } else if (e.is_regular_file()) {
                FileRef r;
                r.host = e.path().string();
                r.size = uint64_t(fs::file_size(e.path()));
                files[rel + n] = r;
            }
        }
    };
    scan(scan, rootFolder, "", 0, true);
    for (const auto& o : overlays) if (fs::is_directory(o)) scan(scan, o, "", 0, false);
    bundlePath.clear();
    return finish(files, volumeId);
}

bool VirtualDisc::buildFromBundle(const std::string& bundleFile, const std::vector<BundledFile>& list, const std::vector<std::string>& overlays,
                                  const std::string& volumeId) {
    std::map<std::string, FileRef> files;
    for (const BundledFile& b : list) {
        FileRef r;
        r.host = "bundle:" + b.path;
        r.size = b.size;
        r.offset = b.offset;
        r.bundled = true;
        files[b.path] = r;
    }
    for (const auto& o : overlays) {
        if (!fs::is_directory(o)) continue;
        for (auto& e : fs::recursive_directory_iterator(o)) {
            if (!e.is_regular_file()) continue;
            FileRef r;
            r.host = e.path().string();
            r.size = uint64_t(fs::file_size(e.path()));
            files[upper(fs::relative(e.path(), o).string())] = r;
        }
    }
    bundlePath = bundleFile;
    bundleStream.close();
    bundleStream.clear();
    bundleStream.open(bundleFile, std::ios::binary);
    if (!bundleStream) return false;
    return finish(files, volumeId);
}

bool VirtualDisc::finish(const std::map<std::string, FileRef>& files, const std::string& volumeId) {
    nodes.clear(); meta.clear(); byPath.clear();
    nodes.push_back(Node{"", "", true, 0, 0, 0, {}, 0});
    std::map<std::string, int> dirIndex{{"", 0}};
    for (const auto& [rel, ref] : files) {
        int parent = 0;
        std::string path;
        size_t start = 0, slash;
        while ((slash = rel.find('\\', start)) != std::string::npos) {
            const std::string part = rel.substr(start, slash - start);
            path += part + "\\";
            auto it = dirIndex.find(path);
            if (it == dirIndex.end()) {
                Node nd; nd.name = part; nd.dir = true; nd.parent = parent; nodes.push_back(nd);
                it = dirIndex.emplace(path, int(nodes.size()) - 1).first;
                nodes[size_t(parent)].kids.push_back(it->second);
            }
            parent = it->second;
            start = slash + 1;
        }
        Node nd; nd.name = rel.substr(start) + ";1"; nd.host = ref.host; nd.size = uint32_t(ref.size); nd.parent = parent;
        nd.bundled = ref.bundled; nd.offset = ref.offset;
        nodes.push_back(nd);
        nodes[size_t(parent)].kids.push_back(int(nodes.size()) - 1);
    }
    layout();
    writeVolumeDescriptors(volumeId);
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].dir) writeDirectory(int(i));
    return true;
}

void VirtualDisc::layout() {
    // 16 PVD, 17 terminator, 18 L path table, 19 M path table, then directories, then files.
    // Directory numbering for the path table is breadth-first.
    std::vector<int> order{0};
    for (size_t i = 0; i < order.size(); ++i)
        for (int k : nodes[size_t(order[i])].kids) if (nodes[size_t(k)].dir) order.push_back(k);
    for (size_t i = 0; i < order.size(); ++i) nodes[size_t(order[i])].pathIndex = int(i) + 1;
    uint32_t lba = 20;
    for (int d : order) {
        auto& n = nodes[size_t(d)];
        // directory extent size: records never straddle a sector
        std::vector<size_t> sizes;
        size_t used = 0, sectors = 1;
        auto add = [&](size_t len) { if (used + len > 2048) { ++sectors; used = 0; } used += len; };
        add(34); add(34);
        for (int k : n.kids) add(33 + nodes[size_t(k)].name.size() + ((33 + nodes[size_t(k)].name.size()) & 1));
        n.lba = lba; n.size = uint32_t(sectors * 2048);
        lba += uint32_t(sectors);
    }
    for (auto& n : nodes) {
        if (n.dir) continue;
        n.lba = lba;
        lba += (n.size + 2047) / 2048;
        std::string path;
        for (int p = n.parent; p != 0; p = nodes[size_t(p)].parent) path = "\\" + nodes[size_t(p)].name + path;
        byPath[path + "\\" + n.name.substr(0, n.name.find(';'))] = n.lba;
    }
    total = lba + 150;
}

void VirtualDisc::writeDirectory(int dirIndex) {
    const Node& n = nodes[size_t(dirIndex)];
    std::vector<uint8_t> data(n.size, 0);
    size_t pos = 0;
    auto emit = [&](const std::vector<uint8_t>& rec) {
        if ((pos % 2048) + rec.size() > 2048) pos = (pos / 2048 + 1) * 2048;
        std::memcpy(&data[pos], rec.data(), rec.size());
        pos += rec.size();
    };
    emit(dirRecord("", true, 0, n.lba, n.size, true));
    const Node& par = nodes[size_t(n.parent)];
    emit(dirRecord("", true, 1, par.lba, par.size, true));
    std::vector<int> kids = n.kids;
    std::sort(kids.begin(), kids.end(), [&](int a, int b) { return nodes[size_t(a)].name < nodes[size_t(b)].name; });
    for (int k : kids) {
        const Node& c = nodes[size_t(k)];
        emit(dirRecord(c.name, false, 0, c.lba, c.size, c.dir));
    }
    for (size_t s = 0; s * 2048 < data.size(); ++s)
        meta[n.lba + uint32_t(s)].assign(data.begin() + long(s * 2048), data.begin() + long((s + 1) * 2048));
}

void VirtualDisc::writeVolumeDescriptors(const std::string& volumeId) {
    std::vector<int> order{0};
    for (size_t i = 0; i < order.size(); ++i)
        for (int k : nodes[size_t(order[i])].kids) if (nodes[size_t(k)].dir) order.push_back(k);
    std::vector<uint8_t> lt, mt;
    for (int d : order) {
        const Node& n = nodes[size_t(d)];
        const std::string nm = d == 0 ? std::string(1, '\0') : n.name;
        const uint16_t parent = d == 0 ? 1 : uint16_t(nodes[size_t(n.parent)].pathIndex);
        for (int be = 0; be < 2; ++be) {
            std::vector<uint8_t>& t = be ? mt : lt;
            const size_t start = t.size();
            t.push_back(uint8_t(nm.size())); t.push_back(0);
            for (int i = 0; i < 4; ++i) t.push_back(uint8_t(be ? n.lba >> (24 - 8 * i) : n.lba >> (8 * i)));
            if (be) { t.push_back(uint8_t(parent >> 8)); t.push_back(uint8_t(parent)); } else { t.push_back(uint8_t(parent)); t.push_back(uint8_t(parent >> 8)); }
            for (char ch : nm) t.push_back(uint8_t(ch));
            if ((t.size() - start) & 1) t.push_back(0);
        }
    }
    std::vector<uint8_t> pvd(2048, 0);
    pvd[0] = 1; std::memcpy(&pvd[1], "CD001", 5); pvd[6] = 1;
    std::memset(&pvd[8], ' ', 64);
    std::memcpy(&pvd[8], "PLAYSTATION", 11);
    std::memcpy(&pvd[40], volumeId.data(), std::min<size_t>(32, volumeId.size()));
    put32both(&pvd[80], total);
    put16both(&pvd[120], 1); put16both(&pvd[124], 1); put16both(&pvd[128], 2048);
    put32both(&pvd[132], uint32_t(lt.size()));
    const uint32_t l = 18, m = 19;
    for (int i = 0; i < 4; ++i) { pvd[140 + i] = uint8_t(l >> (8 * i)); pvd[148 + i] = uint8_t(m >> (24 - 8 * i)); }
    const auto root = dirRecord("", true, 0, nodes[0].lba, nodes[0].size, true);
    std::memcpy(&pvd[156], root.data(), 34);
    pvd[881] = 1;
    meta[16] = pvd;
    std::vector<uint8_t> term(2048, 0);
    term[0] = 255; std::memcpy(&term[1], "CD001", 5); term[6] = 1;
    meta[17] = term;
    lt.resize(2048); mt.resize(2048);
    meta[18] = lt; meta[19] = mt;
}

uint32_t VirtualDisc::fileLba(const std::string& path) const {
    auto it = byPath.find(path);
    return it == byPath.end() ? 0 : it->second;
}

std::string VirtualDisc::fileAt(uint32_t lba) const {
    for (const auto& n : nodes)
        if (!n.dir && lba >= n.lba && lba < n.lba + (n.size + 2047) / 2048) return n.host;
    return {};
}

bool VirtualDisc::readSector(uint32_t lba, uint8_t* out) const {
    if (lba >= total) return false;
    std::memset(out, 0, 2352);
    out[0] = 0; std::memset(out + 1, 0xFF, 10); out[11] = 0;
    const uint32_t abs = lba + 150;
    out[12] = bcd(abs / 75 / 60); out[13] = bcd(abs / 75 % 60); out[14] = bcd(abs % 75); out[15] = 2;
    out[18] = out[22] = 0x08;  // submode: data
    uint8_t* data = out + 24;
    auto m = meta.find(lba);
    if (m != meta.end()) { std::memcpy(data, m->second.data(), 2048); return true; }
    for (const auto& n : nodes) {
        if (n.dir || lba < n.lba || lba >= n.lba + (n.size + 2047) / 2048) continue;
        if (n.bundled) {
            bundleStream.clear();
            bundleStream.seekg(std::streamoff(n.offset + uint64_t(lba - n.lba) * 2048));
            bundleStream.read(reinterpret_cast<char*>(data), 2048);
            return true;
        }
        std::ifstream f(n.host, std::ios::binary);
        if (!f) return true;
        f.seekg(std::streamoff(uint64_t(lba - n.lba) * 2048));
        f.read(reinterpret_cast<char*>(data), 2048);
        return true;
    }
    return true;
}

}  // namespace psx
