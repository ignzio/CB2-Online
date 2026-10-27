#include "savestate.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <type_traits>

namespace psx {
namespace {

constexpr char kMagic[8] = {'C', 'B', '2', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t kVersion = 1;
constexpr uint64_t kMaxBlob = 64u << 20;   // sanity limit for any one array in a file

struct Writer {
    static constexpr bool kReading = false;
    std::vector<uint8_t> out;
    bool ok = true;
    void bytes(void* p, size_t n) { const uint8_t* c = static_cast<const uint8_t*>(p); out.insert(out.end(), c, c + n); }
};

struct Reader {
    static constexpr bool kReading = true;
    const uint8_t* data = nullptr;
    size_t size = 0, pos = 0;
    bool ok = true;
    void bytes(void* p, size_t n) {
        if (!ok || n > size - pos) { ok = false; std::memset(p, 0, n); return; }
        std::memcpy(p, data + pos, n);
        pos += n;
    }
};

// Feeds a hash instead of a buffer (stateDigest).
struct Hasher {
    static constexpr bool kReading = false;
    uint64_t h = 1469598103934665603ull;
    bool ok = true;
    void bytes(void* p, size_t n) {
        const uint8_t* c = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ull; }
    }
};

template <class A, class T> void pod(A& a, T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "plain data only");
    a.bytes(&v, sizeof(T));
}

// A vector of plain data whose length is stored.
template <class A, class T> void vec(A& a, std::vector<T>& v) {
    uint64_t n = v.size();
    pod(a, n);
    if (A::kReading) {
        if (!a.ok || n > kMaxBlob) { a.ok = false; return; }
        v.resize(size_t(n));
    }
    if (n) a.bytes(v.data(), size_t(n) * sizeof(T));
}

// A vector that must keep its size (RAM, VRAM, SPU RAM): a file made with another size does not fit.
template <class A, class T> void fixed(A& a, std::vector<T>& v) {
    uint64_t n = v.size();
    pod(a, n);
    if (A::kReading && n != v.size()) { a.ok = false; return; }
    if (n) a.bytes(v.data(), size_t(n) * sizeof(T));
}

template <class A> void str(A& a, std::string& s) {
    uint64_t n = s.size();
    pod(a, n);
    if (A::kReading) {
        if (!a.ok || n > 4096) { a.ok = false; return; }
        s.resize(size_t(n));
    }
    if (n) a.bytes(s.data(), size_t(n));
}

}  // namespace

struct StateAccess {
    template <class A> static void cpu(A& a, r3000::Cpu& c) {
        pod(a, c.r); pod(a, c.hi); pod(a, c.lo); pod(a, c.pc); pod(a, c.nextPc);
        pod(a, c.cop0); pod(a, c.cop2d); pod(a, c.cop2c); pod(a, c.scratch);
        pod(a, c.instructions); pod(a, c.fault); pod(a, c.faultAddr); pod(a, c.pendReg); pod(a, c.pendVal);
    }

    template <class A> static void gpu(A& a, Gpu& g) {
        fixed(a, g.vram);
        pod(a, g.displayEnabled); pod(a, g.displayX); pod(a, g.displayY); pod(a, g.hres); pod(a, g.vres);
        pod(a, g.color24); pod(a, g.interlaced); pod(a, g.pal);
        pod(a, g.drawCalls); pod(a, g.primitives); pod(a, g.unknownCommands);
        vec(a, g.fifo); pod(a, g.need); pod(a, g.polyLine);
        pod(a, g.xfer); pod(a, g.xX); pod(a, g.xY); pod(a, g.xW); pod(a, g.xH); pod(a, g.xPos); pod(a, g.readLatch);
        pod(a, g.tpX); pod(a, g.tpY); pod(a, g.semi); pod(a, g.depth);
        pod(a, g.dither); pod(a, g.drawToDisplay); pod(a, g.texDisable); pod(a, g.rectFlipX); pod(a, g.rectFlipY);
        pod(a, g.twMaskX); pod(a, g.twMaskY); pod(a, g.twOffX); pod(a, g.twOffY);
        pod(a, g.areaX0); pod(a, g.areaY0); pod(a, g.areaX1); pod(a, g.areaY1); pod(a, g.offX); pod(a, g.offY);
        pod(a, g.setMask); pod(a, g.checkMask); pod(a, g.dir); pod(a, g.field); pod(a, g.infoLatch);
        vec(a, g.fbRects);
    }

    template <class A> static void cd(A& a, CdRom& c) {
        pod(a, c.irqLine);
        pod(a, c.index); pod(a, c.enable); pod(a, c.flags); pod(a, c.mode); pod(a, c.status); pod(a, c.request);
        std::vector<uint8_t> params(c.params.begin(), c.params.end()), response(c.response.begin(), c.response.end());
        vec(a, params); vec(a, response);
        if (A::kReading) { c.params.assign(params.begin(), params.end()); c.response.assign(response.begin(), response.end()); }
        vec(a, c.data); pod(a, c.dataPos);
        pod(a, c.lba); pod(a, c.seekLba); pod(a, c.reading); pod(a, c.filterFile); pod(a, c.filterChannel);
        uint64_t n = c.pending.size();
        pod(a, n);
        if (A::kReading) {
            if (!a.ok || n > 4096) { a.ok = false; return; }
            c.pending.assign(size_t(n), CdRom::Pending{});
        }
        for (CdRom::Pending& q : c.pending) { pod(a, q.due); pod(a, q.irq); vec(a, q.resp); pod(a, q.sectorLba); }
        vec(a, c.sectorBuf); pod(a, c.now); pod(a, c.nextSector);
    }

    template <class A> static void spu(A& a, Spu& s) {
        fixed(a, s.ram);
        pod(a, s.irq); pod(a, s.framesProduced);
        pod(a, s.v); pod(a, s.regs); pod(a, s.transferAddr); pod(a, s.irqAddr); pod(a, s.endx);
    }

    // The card's file handles and scan position, but not its image (see savestate.hpp).
    template <class A> static void card(A& a, MemCard& m) {
        pod(a, m.newCard); pod(a, m.handles); pod(a, m.scanBlock);
        str(a, m.scanPattern);
    }

    template <class A> static void platform(A& a, Platform& p) {
        fixed(a, p.ram);
        cpu(a, p.cpu);
        gpu(a, p.gpu);
        cd(a, p.cd);
        spu(a, p.spu);
        card(a, p.card);
        pod(a, p.istat); pod(a, p.imask); pod(a, p.irqCount); pod(a, p.cycles); pod(a, p.frames); pod(a, p.padButtons);
        pod(a, p.saved); pod(a, p.hookBuf);
        pod(a, p.timers);
        pod(a, p.dmaMadr); pod(a, p.dmaBcr); pod(a, p.dmaChcr); pod(a, p.dpcr); pod(a, p.dicr);
        pod(a, p.spuCycles); pod(a, p.spuIrqSeen); pod(a, p.cdPrevIrq); pod(a, p.lastService); pod(a, p.nextVblank);
        vec(a, p.events);
        pod(a, p.padBuf1); pod(a, p.padBuf2); pod(a, p.padLen1); pod(a, p.padLen2); pod(a, p.padActive);
        pod(a, p.heapStart); pod(a, p.heapEnd); pod(a, p.heapPtr); pod(a, p.randState);
    }

    // What a loaded state must not leave behind: output the renderer had not consumed yet.
    static void afterLoad(Platform& p) {
        p.gpu.cmds.clear();
        p.gpu.cmdPool.clear();
        p.gpu.textPool.clear();
        p.gpu.pendingTags.clear();
        p.cpu.fault = false;
        p.inHook = false;
        p.lastError.clear();
    }

    static bool usable(Platform& p, std::string& error) {
        if (p.useNative) { error = "save states need the interpreter (not --native)"; return false; }
        if (p.shadow) { error = "not available in shadow mode"; return false; }
        return true;
    }
};

uint64_t stateDigest(Platform& p) {
    Hasher h;
    StateAccess::platform(h, p);
    return h.h;
}

bool saveStateToMemory(Platform& p, const std::vector<uint8_t>& extra, std::vector<uint8_t>& out, std::string& error) {
    if (!StateAccess::usable(p, error)) return false;
    Writer payload;
    StateAccess::platform(payload, p);
    Writer file;
    file.bytes(const_cast<char*>(kMagic), sizeof kMagic);
    uint32_t version = kVersion | (uint32_t(p.profile->region) << 8), ramSize = uint32_t(p.ram.size()), extraSize = uint32_t(extra.size());   // the release is part of the version: a PAL state is not an NTSC-U one
    uint64_t payloadSize = payload.out.size();
    pod(file, version); pod(file, ramSize); pod(file, extraSize);
    if (extraSize) file.bytes(const_cast<uint8_t*>(extra.data()), extra.size());
    pod(file, payloadSize);
    file.out.insert(file.out.end(), payload.out.begin(), payload.out.end());
    Hasher h;
    h.bytes(payload.out.data(), payload.out.size());
    pod(file, h.h);
    out = std::move(file.out);
    return true;
}

bool saveState(Platform& p, const std::string& path, const std::vector<uint8_t>& extra, std::string& error) {
    Writer file;
    if (!saveStateToMemory(p, extra, file.out, error)) return false;

    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target = fs::u8path(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    fs::path tmp = target;
    tmp += ".part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(file.out.data()), std::streamsize(file.out.size()));
        if (!f) { error = "cannot write " + path; return false; }
    }
    fs::remove(target, ec);
    fs::rename(tmp, target, ec);
    if (ec) { error = "cannot write " + path; return false; }
    return true;
}

bool loadState(Platform& p, const std::string& path, std::vector<uint8_t>& extra, std::string& error) {
    if (!StateAccess::usable(p, error)) return false;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { error = "no saved state at " + path; return false; }
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return loadStateFromMemory(p, all, extra, error);
}

bool loadStateFromMemory(Platform& p, const std::vector<uint8_t>& all, std::vector<uint8_t>& extra, std::string& error) {
    if (!StateAccess::usable(p, error)) return false;
    Reader r;
    r.data = all.data();
    r.size = all.size();
    char magic[8];
    r.bytes(magic, sizeof magic);
    uint32_t version = 0, ramSize = 0, extraSize = 0;
    pod(r, version); pod(r, ramSize); pod(r, extraSize);
    if (!r.ok || std::memcmp(magic, kMagic, sizeof magic) != 0) { error = "this is not a save state file"; return false; }
    if ((version & 0xFF) != kVersion) { error = "this save state was made by another version of the game"; return false; }
    if (((version >> 8) & 0xFF) != uint32_t(p.profile->region)) { error = std::string("this save state belongs to the other release of the game (") + (p.profile->region == cb2game::Region::Pal ? "NTSC-U" : "PAL") + ")"; return false; }
    if (ramSize != p.ram.size()) { error = "this save state was made with a different amount of memory (ram_mb)"; return false; }
    if (extraSize > kMaxBlob) { error = "damaged save state"; return false; }
    std::vector<uint8_t> loadedExtra(extraSize);
    if (extraSize) r.bytes(loadedExtra.data(), extraSize);
    uint64_t payloadSize = 0;
    pod(r, payloadSize);
    if (!r.ok || payloadSize > r.size - r.pos || r.size - r.pos - payloadSize != sizeof(uint64_t)) { error = "damaged save state (wrong size)"; return false; }
    Hasher h;
    h.bytes(const_cast<uint8_t*>(r.data + r.pos), size_t(payloadSize));
    uint64_t stored = 0;
    std::memcpy(&stored, r.data + r.pos + payloadSize, sizeof stored);
    if (stored != h.h) { error = "damaged save state (checksum)"; return false; }

    // The file is whole and from this version: apply it.
    Reader body;
    body.data = r.data + r.pos;
    body.size = size_t(payloadSize);
    StateAccess::platform(body, p);
    if (!body.ok || body.pos != body.size) { error = "damaged save state (contents)"; return false; }
    StateAccess::afterLoad(p);
    extra = std::move(loadedExtra);
    return true;
}

}  // namespace psx
