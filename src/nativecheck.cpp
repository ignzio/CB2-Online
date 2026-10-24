// Differential checker for the translated engine code (tools/recomp.py output).
//
// The game runs on the interpreter. At sampled entries of translated functions the complete machine state (registers and
// RAM) is cloned into a shadow platform, and the function is executed twice from that identical state: once on the
// interpreter (the reference) and once as translated C++. Final registers, hi/lo, GTE registers, all of RAM and the
// device/BIOS traffic must be identical. Every function is checked several times, then sampled.
//   cb2_nativecheck <game folder> [frames=1500] [sample-every=400] [min-checks-per-function=4]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "psx.hpp"
#include "recomp_rt.hpp"

namespace {

std::atomic<int> gPhase{0};          // 1 load, 2 reference, 3 translated, 4 compare, 5 main loop
std::atomic<uint32_t> gAddr{0};
std::atomic<uint64_t> gProgress{0};

struct Snapshot {
    uint32_t r[32], hi, lo, pc, cop2d[32], cop2c[32], cop0[32];
    std::vector<uint8_t> ram;
    std::vector<uint64_t> log;
    bool fault = false, limited = false;
    std::string error;
    uint64_t instructions = 0;
};

struct Failure {
    uint64_t count = 0;
    std::vector<std::string> details;
    uint32_t caller = 0;
};

void load(psx::Platform& s, const psx::Platform& m) {
    auto& c = s.cpu;
    std::memcpy(c.r, m.cpu.r, sizeof c.r);
    c.hi = m.cpu.hi; c.lo = m.cpu.lo;
    std::memcpy(c.cop0, m.cpu.cop0, sizeof c.cop0);
    std::memcpy(c.cop2d, m.cpu.cop2d, sizeof c.cop2d);
    std::memcpy(c.cop2c, m.cpu.cop2c, sizeof c.cop2c);
    c.pendReg = m.cpu.pendReg; c.pendVal = m.cpu.pendVal;
    c.fault = false; c.faultAddr = 0;
    std::memcpy(s.ram.data(), m.ram.data(), m.ram.size());
    std::memcpy(c.scratch, m.cpu.scratch, sizeof c.scratch);
    c.instructions = 0;
}

void capture(psx::Platform& s, Snapshot& out, std::vector<uint64_t>& log) {
    auto& c = s.cpu;
    if (c.pendReg) { c.r[c.pendReg] = c.pendVal; c.pendReg = 0; }
    std::memcpy(out.r, c.r, sizeof out.r);
    out.hi = c.hi; out.lo = c.lo; out.pc = c.pc;
    std::memcpy(out.cop2d, c.cop2d, sizeof out.cop2d);
    std::memcpy(out.cop2c, c.cop2c, sizeof out.cop2c);
    std::memcpy(out.cop0, c.cop0, sizeof out.cop0);
    out.ram = s.ram;
    out.log = log;
    out.instructions = c.instructions;
}

std::vector<uint32_t>* gRefTrace = nullptr;   // when set, the reference run records which translated basic blocks it visits

bool isLeader(uint32_t pc) {
    const uint32_t* b = recomp::kLeaders;
    return std::binary_search(b, b + recomp::kLeaderCount, pc);
}

void runReference(psx::Platform& s, uint32_t addr, uint32_t ra, Snapshot& out, std::vector<uint64_t>& log) {
    auto& c = s.cpu;
    c.pc = addr; c.nextPc = addr + 4;
    try {
        uint64_t steps = 0;
        const uint32_t sp0 = c.r[29];
        while (!(c.pc == ra && c.nextPc == c.pc + 4 && c.r[29] == sp0)) {
            if (gRefTrace && c.nextPc == c.pc + 4 && isLeader(c.pc)) gRefTrace->push_back(c.pc);
            c.step();
            if (++steps > 100000) { out.limited = true; break; }
        }
    } catch (const std::exception& e) {
        out.fault = true;
        out.error = e.what();
    }
    capture(s, out, log);
}

void runTranslated(psx::Platform& s, uint32_t addr, Snapshot& out, std::vector<uint64_t>& log) {
    auto& c = s.cpu;
    if (c.pendReg) { c.r[c.pendReg] = c.pendVal; c.pendReg = 0; }
    c.pc = addr; c.nextPc = addr + 4;
    s.pollAt = 600000;
    try {
        s.call(addr);
    } catch (const psx::Platform::ShadowLimit&) {
        out.limited = true;
    } catch (const std::exception& e) {
        out.fault = true;
        out.error = e.what();
    }
    capture(s, out, log);
}

// Re-runs one function from the saved state with block tracing and names the first block where the two executions part ways.
std::string explain(psx::Platform& s, const psx::Platform& m, uint32_t addr, uint32_t ra, std::vector<uint64_t>& log) {
    std::vector<uint32_t> ref, nat;
    Snapshot a, b;
    load(s, m); log.clear();
    gRefTrace = &ref;
    runReference(s, addr, ra, a, log);
    gRefTrace = nullptr;
    load(s, m); log.clear();
    s.blockTrace = &nat;
    s.traceLeader = isLeader;
    runTranslated(s, addr, b, log);
    s.blockTrace = nullptr;
    s.traceLeader = nullptr;
    size_t i = 0;
    while (i < ref.size() && i < nat.size() && ref[i] == nat[i]) ++i;
    char buf[300];
    if (i == ref.size() && i == nat.size()) return "same block sequence (" + std::to_string(i) + " blocks); the difference is inside a block";
    std::string out = "block sequences diverge at block #" + std::to_string(i) + " of " + std::to_string(ref.size()) + "/" + std::to_string(nat.size());
    std::snprintf(buf, sizeof buf, ": after %08x the interpreter went to %08x, translated went to %08x", i ? ref[i - 1] : 0, i < ref.size() ? ref[i] : 0, i < nat.size() ? nat[i] : 0);
    out += buf;
    auto list = [&](const char* name, const std::vector<uint32_t>& v) {
        out += std::string(" | ") + name + ":";
        for (size_t k = i > 3 ? i - 3 : 0; k < v.size() && k < i + 4; ++k) { std::snprintf(buf, sizeof buf, " %08x", v[k]); out += buf; }
    };
    list("interp", ref);
    list("native", nat);
    std::snprintf(buf, sizeof buf, " | entry a0..a3 = %08x %08x %08x %08x", m.cpu.r[4], m.cpu.r[5], m.cpu.r[6], m.cpu.r[7]);
    return out + buf;
}

std::string compare(const Snapshot& a, const Snapshot& b) {
    char buf[200];
    for (int i = 1; i < 32; ++i)
        if (a.r[i] != b.r[i]) { std::snprintf(buf, sizeof buf, "r%d: interpreter %08x, translated %08x", i, a.r[i], b.r[i]); return buf; }
    if (a.hi != b.hi || a.lo != b.lo) { std::snprintf(buf, sizeof buf, "hi/lo: interpreter %08x:%08x, translated %08x:%08x", a.hi, a.lo, b.hi, b.lo); return buf; }
    for (int i = 0; i < 32; ++i)
        if (a.cop2d[i] != b.cop2d[i] || a.cop2c[i] != b.cop2c[i]) { std::snprintf(buf, sizeof buf, "GTE register %d differs", i); return buf; }
    if (a.cop0[12] != b.cop0[12]) { std::snprintf(buf, sizeof buf, "cop0 status: interpreter %08x, translated %08x", a.cop0[12], b.cop0[12]); return buf; }
    if (a.log != b.log) {
        size_t i = 0;
        while (i < a.log.size() && i < b.log.size() && a.log[i] == b.log[i]) ++i;
        std::snprintf(buf, sizeof buf, "device/BIOS traffic differs at item %zu (%zu vs %zu items)", i, a.log.size(), b.log.size());
        return buf;
    }
    if (a.ram != b.ram) {
        for (size_t i = 0; i < a.ram.size(); ++i)
            if (a.ram[i] != b.ram[i]) {
                std::snprintf(buf, sizeof buf, "RAM %08zx: interpreter %02x, translated %02x", 0x80000000 + i, a.ram[i], b.ram[i]);
                return buf;
            }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_nativecheck <game folder> [frames] [sample-every] [min-checks]\n"); return 2; }
    const std::string folder = argv[1];
    const uint64_t frames = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1500;
    const uint64_t every = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 400;
    const uint64_t minChecks = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 4;

    psx::Platform m;
    std::unique_ptr<psx::Platform> sp(every ? new psx::Platform : nullptr);
    psx::Platform& s = sp ? *sp : m;
    m.echoPrintf = std::getenv("CB2_ECHO") != nullptr;
    if (!std::getenv("CB2_NOTABLE") && !m.loadNativeTable()) { std::fprintf(stderr, "no translated code linked in\n"); return 2; }
    if (sp) s.loadNativeTable();
    if (!m.loadExe(folder + "\\SCUS_941.54") || !m.mountDisc(folder)) { std::fprintf(stderr, "cannot load the game\n"); return 2; }
    std::thread([&] {   // watchdog: reports where the run is stuck instead of hanging the session
        uint64_t last = ~0ull;
        for (int stale = 0;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const uint64_t now = gProgress.load();
            stale = now == last ? stale + 1 : 0;
            last = now;
            if (stale >= 2) {
                std::fprintf(stderr, "[watchdog] no progress: phase %d (1 load, 2 reference, 3 translated, 4 compare, 5 main) in function %08x; main pc=%08x instr=%llu frames=%llu\n", gPhase.load(), gAddr.load(), m.cpu.pc, (unsigned long long)m.cpu.instructions, (unsigned long long)m.frames);
                std::fflush(stderr);
                std::_Exit(3);
            }
        }
    }).detach();
    std::vector<uint64_t> shadowLog;
    if (sp) s.configureShadow(&shadowLog);

    std::map<uint32_t, uint64_t> entries, checked;
    std::map<uint32_t, Failure> failures;
    uint64_t total = 0, compared = 0, skipped = 0;
    uint64_t counter = 0;
    double tLoad = 0, tRef = 0, tNat = 0;
    uint64_t nextSlot = 0;
    std::set<uint32_t> spinners;   // functions that never finish with stubbed devices (wait loops): not checkable this way
    Snapshot A, B;

    if (every) m.entryHook = [&](uint32_t addr) {
        gPhase = 10; gAddr = addr;
        struct Done { ~Done() { gPhase = 11; ++gProgress; } } done_;
        ++total;
        const uint64_t n = ++entries[addr];
        if (spinners.count(addr)) return;
        if (m.cpu.instructions < nextSlot) return;  // at most one check per slot, so spin loops cannot dominate the run
        if (checked[addr] >= minChecks && (++counter % every) != 0) return;
        nextSlot = m.cpu.instructions + 20000;
        if (std::getenv("CB2_VERBOSE")) { std::fprintf(stderr, "[enter] %08x ra=%08x instr=%llu\n", addr, m.cpu.r[31], (unsigned long long)m.cpu.instructions); std::fflush(stderr); }
        ++checked[addr];
        const uint32_t ra = m.cpu.r[31];
        A = Snapshot{}; B = Snapshot{};
        gAddr = addr; gPhase = 1; ++gProgress;
        load(s, m);
        shadowLog.clear();
        gPhase = 2;
        runReference(s, addr, ra, A, shadowLog);
        gPhase = 1;
        if (std::getenv("CB2_VERBOSE")) { std::fprintf(stderr, "   ref: %llu instr limited=%d fault=%d %s\n", (unsigned long long)A.instructions, A.limited, A.fault, A.error.c_str()); std::fflush(stderr); }
        load(s, m);
        shadowLog.clear();
        gPhase = 3;
        runTranslated(s, addr, B, shadowLog);
        gPhase = 4;
        if (std::getenv("CB2_VERBOSE")) { std::fprintf(stderr, "   translated: %llu instr limited=%d fault=%d %s\n", (unsigned long long)B.instructions, B.limited, B.fault, B.error.c_str()); std::fflush(stderr); }
        if (A.fault || A.limited || (B.limited && !A.limited)) {
            if (A.fault != B.fault || (!A.limited && B.limited)) {
                Failure& f = failures[addr];
                ++f.count; f.caller = ra;
                if (f.details.size() < 3) f.details.push_back("  -> " + explain(s, m, addr, ra, shadowLog));
                if (f.details.size() < 3) f.details.push_back(std::string("outcome differs: interpreter ") + (A.fault ? "fault " + A.error : A.limited ? "limit" : "ok") + ", translated " + (B.fault ? "fault " + B.error : B.limited ? "limit" : "ok"));
            } else {
                ++skipped;
            }
            return;
        }
        if (B.fault || B.limited) {
            Failure& f = failures[addr];
            ++f.count; f.caller = ra;
            if (f.details.size() < 3) {
                f.details.push_back(std::string("translated: ") + (B.fault ? B.error : "did not finish"));
                f.details.push_back("  -> " + explain(s, m, addr, ra, shadowLog));
            }
            return;
        }
        ++compared;
        if (compared % 50 == 0) { std::fprintf(stderr, "[check] %llu compared, last %08x, frame %llu, load %.0f ms, reference %.0f ms, translated %.0f ms\n", (unsigned long long)compared, addr, (unsigned long long)m.frames, tLoad, tRef, tNat); std::fflush(stderr); }
        const std::string d = compare(A, B);
        if (!d.empty()) {
            Failure& f = failures[addr];
            ++f.count; f.caller = ra;
            if (f.details.size() < 3) {
                f.details.push_back(d);
                f.details.push_back("  -> " + explain(s, m, addr, ra, shadowLog));
            }
        }
    };

    // Scripted pad: Start at the title screen, then Start/Cross presses that walk through the intro into the first level.
    m.onVblank = [&] {
        const uint64_t f = m.frames;
        uint16_t pad = 0xFFFF;
        if (f >= 2310 && f <= 2314) pad &= uint16_t(~(1u << 3));
        if (f >= 2500) {
            if ((f % 100) < 4) pad &= uint16_t(~(1u << 3));
            if ((f % 100) >= 50 && (f % 100) < 54) pad &= uint16_t(~(1u << 14));
        }
        if (f >= 11000) pad &= uint16_t(~(1u << (4 + ((f / 40) % 4))));   // then walk around: up/right/down/left
        m.padButtons = pad;
    };
    uint64_t done = 0;
    while (m.frames < frames) {
        if (m.cpu.instructions > 4190000 && std::getenv("CB2_DBG")) { std::fprintf(stderr, "[dbg] pc=%08x instr=%llu\n", m.cpu.pc, (unsigned long long)m.cpu.instructions); std::fflush(stderr); }
        gPhase = 5; ++gProgress;
        const std::string err = m.run(std::getenv("CB2_DBG") ? 1 << 8 : 1 << 15);
        done += 1 << 15;
        if ((done >> 15) % 512 == 0) { std::fprintf(stderr, "[main %.2fs] pc=%08x instr=%llu frames=%llu\n", double(std::clock()) / CLOCKS_PER_SEC, m.cpu.pc, (unsigned long long)m.cpu.instructions, (unsigned long long)m.frames); std::fflush(stderr); }
        if (!err.empty()) { std::fprintf(stderr, "main platform stopped: %s\n", err.c_str()); break; }
        if (done % (1 << 24) == 0) std::fprintf(stderr, "frame %llu, %llu checks, %zu failing functions\n", (unsigned long long)m.frames, (unsigned long long)compared, failures.size());
    }

    size_t covered = 0;
    for (auto& [a, n] : entries) (void)n, ++covered;
    std::printf("frames %llu: %llu function entries, %zu distinct translated functions executed\n", (unsigned long long)m.frames, (unsigned long long)total, covered);
    std::printf("compared %llu runs (skipped %llu that hit limits/faults on both sides); failing functions: %zu\n", (unsigned long long)compared, (unsigned long long)skipped, failures.size());
    for (auto& [a, f] : failures) {
        std::printf("  FAIL %08x  x%llu  (entered %llu times, first caller %08x)\n", a, (unsigned long long)f.count, (unsigned long long)entries[a], f.caller);
        for (auto& d : f.details) std::printf("       %s\n", d.c_str());
    }
    return failures.empty() ? 0 : 1;
}
