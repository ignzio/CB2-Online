#include "gool_asm.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "gool_decode.hpp"

namespace cb2 {
namespace {

constexpr uint32_t kMipsWord = 0x49BE0BE0u;
constexpr uint32_t kBlockEnd = 0x03E0A809u;  // `jalr s5, ra`; the following word is its delay slot

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

std::string destText(uint32_t ref12) {
    const RefInfo r = decodeDest(ref12);
    char buf[48];
    switch (r.kind) {
    case RefKind::StackSlot: std::snprintf(buf, sizeof buf, r.index >= 0 ? "stack[%d]" : "arg[%d]", r.index >= 0 ? r.index : -r.index - 1); break;
    case RefKind::LinkField: std::snprintf(buf, sizeof buf, "link%d.f%d", r.link, r.index); break;
    case RefKind::SelfField: std::snprintf(buf, sizeof buf, "f%d", r.index); break;
    default: std::snprintf(buf, sizeof buf, "[sp]"); break;
    }
    return buf;
}

int64_t parseInt(const std::string& s) {
    if (s.empty()) fail("empty number");
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 0);
    if (*end) fail("bad number '" + s + "'");
    return v;
}

// Operand text -> 12-bit ref (canonical encoding).
uint32_t parseRef(const std::string& t, bool dest) {
    auto idx = [&](const char* prefix) -> int64_t {
        const std::string p = prefix;
        if (t.size() > p.size() + 1 && t.compare(0, p.size(), p) == 0 && t.back() == ']') return parseInt(t.substr(p.size(), t.size() - p.size() - 1));
        return INT64_MIN;
    };
    if (t == "[sp]") return 0xE1F;
    int64_t v;
    if ((v = idx("EL[")) != INT64_MIN) { if (dest || v < 0 || v > 0x3FF) fail("bad operand " + t); return 0x400u | uint32_t(v); }
    if ((v = idx("L[")) != INT64_MIN) { if (dest || v < 0 || v > 0x3FF) fail("bad operand " + t); return uint32_t(v); }
    if ((v = idx("stack[")) != INT64_MIN) { if (v < 0 || v > 63) fail("bad operand " + t); return 0xB00u | uint32_t(v); }
    if ((v = idx("arg[")) != INT64_MIN) { if (v < 0 || v > 64) fail("bad operand " + t); return 0xB00u | uint32_t(-v - 1) & 0x7Fu; }
    if (t.size() > 1 && t[0] == '#') {
        if (dest) fail("immediate is not a destination: " + t);
        const int64_t x = parseInt(t.substr(1));
        if (x % 256 == 0 && x / 256 >= -256 && x / 256 <= 255) return 0x800u | uint32_t((x / 256) & 0x1FF);
        if (x % 16 == 0 && x / 16 >= -128 && x / 16 <= 127) return 0xA00u | uint32_t((x / 16) & 0xFF);
        fail("immediate " + t + " is not representable (use a constant-pool entry)");
    }
    if (t == "0" || t == "1") { if (dest) fail("constant is not a destination"); return 0xB80u | (t == "1" ? 0x10u : 0u); }
    int link = 0, field = 0;
    if (std::sscanf(t.c_str(), "link%d.f%d", &link, &field) == 2) {
        if (link < 0 || link > 7 || field < 0 || field > 63) fail("bad operand " + t);
        return 0xC00u | uint32_t(link << 6) | uint32_t(field);
    }
    if (t.size() > 1 && t[0] == 'f' && std::isdigit(static_cast<unsigned char>(t[1]))) {
        field = int(parseInt(t.substr(1)));
        if (field < 0 || field > 0x1FF) fail("bad operand " + t);
        return 0xE00u | uint32_t(field);
    }
    fail("unknown operand '" + t + "'");
}

const OpSpec* byName(const std::string& name) {
    static const std::map<std::string, const OpSpec*> m = [] {
        std::map<std::string, const OpSpec*> r;
        for (int op = 0; op < 256; ++op) if (const OpSpec* s = opSpec(op)) r[s->name] = s;
        return r;
    }();
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second;
}

bool isBranch(int op) { return op == 50 || op == 51 || op == 52; }

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    for (std::string t; in >> t;) out.push_back(t);
    return out;
}

uint32_t assembleLine(const std::string& line, uint32_t index, const std::map<std::string, uint32_t>& labels) {
    const auto toks = tokens(line);
    if (toks.empty()) fail("empty instruction");
    const OpSpec* s = byName(toks[0]);
    if (!s) fail("unknown mnemonic '" + toks[0] + "'");
    if (s->opcode == 73) return kMipsWord;
    uint32_t word = uint32_t(s->opcode) << 24;
    std::map<std::string, std::string> kv;
    std::string arrow;
    uint32_t extra = 0;  // `x=0x...`: bits the original compiler left in fields the opcode does not define
    for (size_t i = 1; i < toks.size(); ++i) {
        if (toks[i].compare(0, 2, "->") == 0) { arrow = toks[i].substr(2); continue; }
        const size_t eq = toks[i].find('=');
        if (eq == std::string::npos) fail("expected NAME=value, got '" + toks[i] + "'");
        if (toks[i].substr(0, eq) == "x") { extra = uint32_t(parseInt(toks[i].substr(eq + 1))) & 0xFFFFFFu; continue; }
        kv[toks[i].substr(0, eq)] = toks[i].substr(eq + 1);
    }
    for (const FieldSpec& f : s->fields) {
        const std::string name(1, f.name);
        const auto it = kv.find(name);
        const bool isTarget = !arrow.empty() && ((isBranch(s->opcode) && f.kind == FieldKind::Signed) || (s->opcode == 59 && f.name == 'I'));
        int64_t v = 0;
        if (isTarget) {
            const auto lab = labels.find(arrow);
            if (lab == labels.end()) fail("unknown label '" + arrow + "'");
            v = s->opcode == 59 ? int64_t(lab->second) : int64_t(lab->second) - int64_t(index) - 1;
        } else if (it == kv.end()) {
            if (f.kind == FieldKind::Ref || f.kind == FieldKind::Dest) fail(std::string("missing operand ") + f.name + " for " + s->name);
        } else if (f.kind == FieldKind::Ref || f.kind == FieldKind::Dest) {
            v = parseRef(it->second, f.kind == FieldKind::Dest);
        } else {
            v = parseInt(it->second);
        }
        const int64_t lo = f.kind == FieldKind::Signed ? -(int64_t(1) << (f.width - 1)) : 0;
        const int64_t hi = f.kind == FieldKind::Signed ? (int64_t(1) << (f.width - 1)) - 1 : (int64_t(1) << f.width) - 1;
        if (v < lo || v > hi) fail(std::string("field ") + f.name + " of " + s->name + " out of range");
        word |= (uint32_t(v) & ((1u << f.width) - 1u)) << f.shift;
    }
    return word ^ extra;
}

std::string labelName(uint32_t index) {
    char b[16];
    std::snprintf(b, sizeof b, "L%04X", index);
    return b;
}

// Text of one instruction, or empty when it cannot be written losslessly.
std::string instructionText(uint32_t word, uint32_t index, size_t codeSize, std::set<uint32_t>* targets,
                            const std::vector<uint8_t>& isNative) {
    const int op = int(word >> 24);
    const OpSpec* s = opSpec(op);
    if (!s || op == 73) return {};
    std::string out = s->name;
    uint32_t target = UINT32_MAX;
    for (const FieldSpec& f : s->fields) {
        const int32_t v = fieldValue(word, f);
        char buf[48];
        if (isBranch(op) && f.kind == FieldKind::Signed) {
            const int64_t t = int64_t(index) + 1 + v;
            if (t < 0 || t >= int64_t(codeSize) || isNative[size_t(t)]) return {};
            target = uint32_t(t);
            out += " ->" + labelName(target);
        } else if (op == 59 && f.name == 'I' && !((word >> 14) & 1) && uint32_t(v) < codeSize && !isNative[uint32_t(v)]) {
            target = uint32_t(v);
            out += " ->" + labelName(target);
        } else if (f.kind == FieldKind::Ref) {
            out += std::string(" ") + f.name + "=" + refText(uint32_t(v));
        } else if (f.kind == FieldKind::Dest) {
            out += std::string(" ") + f.name + "=" + destText(uint32_t(v));
        } else {
            std::snprintf(buf, sizeof buf, " %c=%d", f.name, v);
            out += buf;
        }
    }
    // Whatever the canonical encoding does not reproduce (unused bits, alternative operand encodings) goes into x=.
    std::map<std::string, uint32_t> labels;
    if (target != UINT32_MAX) labels[labelName(target)] = target;
    try {
        const uint32_t canonical = assembleLine(out, index, labels);
        const uint32_t diff = (canonical ^ word) & 0xFFFFFFu;
        if ((canonical ^ word) >> 24) return {};
        if (diff) {
            char b[24];
            std::snprintf(b, sizeof b, " x=0x%X", diff);
            out += b;
        }
        if (assembleLine(out, index, labels) != word) return {};
    } catch (const std::exception&) {
        return {};
    }
    if (targets && target != UINT32_MAX) targets->insert(target);
    return out;
}

}  // namespace

std::string disassembleScript(const GoolText& script, DisasmStats* stats) {
    const auto& code = script.code;
    DisasmStats local;
    DisasmStats& st = stats ? *stats : local;
    std::set<uint32_t> targets;
    std::vector<std::string> text(code.size());
    std::vector<uint8_t> isNative(code.size(), 0);
    for (size_t i = 0; i < code.size(); ++i) {
        if (isNative[i] || int(code[i] >> 24) != 73) continue;
        // MIPS marker: everything up to and including the delay slot of the closing `jalr s5, ra` is machine code.
        size_t j = i + 1;
        while (j < code.size() && code[j] != kBlockEnd) { isNative[j] = 1; ++j; }
        if (j < code.size()) { isNative[j] = 1; if (j + 1 < code.size()) isNative[j + 1] = 1; }
    }
    for (size_t i = 0; i < code.size(); ++i) {
        if (isNative[i]) continue;
        if (int(code[i] >> 24) == 73) text[i] = code[i] == kMipsWord ? "MIPS" : "";
        else text[i] = instructionText(code[i], uint32_t(i), code.size(), &targets, isNative);
    }
    std::string out = "; GOOL script: " + std::to_string(script.pool.size()) + " constants, " + std::to_string(code.size()) + " words\n.pool\n";
    char buf[64];
    for (uint32_t c : script.pool) { std::snprintf(buf, sizeof buf, ".const 0x%08X\n", c); out += buf; }
    out += ".code\n";
    for (size_t i = 0; i < code.size(); ++i) {
        if (targets.count(uint32_t(i)) && !isNative[i]) { out += labelName(uint32_t(i)) + ":\n"; ++st.labels; }
        std::snprintf(buf, sizeof buf, "  ; %04zX\n", i);
        if (isNative[i]) {
            std::snprintf(buf, sizeof buf, ".word 0x%08X\n", code[i]);
            out += buf;
            ++st.nativeWords;
        } else if (!text[i].empty()) {
            out += text[i];
            std::snprintf(buf, sizeof buf, "  ; %04zX\n", i);
            out += buf;
            ++st.instructions;
        } else {
            std::snprintf(buf, sizeof buf, ".word 0x%08X  ; %04zX %s\n", code[i], i, disassemble(code[i]).c_str());
            out += buf;
            ++st.rawWords;
        }
    }
    return out;
}

GoolText assembleScript(const std::string& textIn) {
    struct Line { size_t number; std::string body; };
    std::vector<Line> lines;
    std::map<std::string, uint32_t> labels;
    GoolText out;
    bool inPool = false;
    uint32_t count = 0;
    size_t number = 0;
    std::istringstream in(textIn);
    for (std::string raw; std::getline(in, raw);) {
        ++number;
        const size_t semi = raw.find(';');
        if (semi != std::string::npos) raw.resize(semi);
        size_t a = raw.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        std::string body = raw.substr(a, raw.find_last_not_of(" \t\r") - a + 1);
        try {
            if (body == ".pool") { inPool = true; continue; }
            if (body == ".code") { inPool = false; continue; }
            if (body.back() == ':') { labels[body.substr(0, body.size() - 1)] = count; continue; }
            if (inPool) {
                if (body.compare(0, 6, ".const") != 0) fail("expected .const in the pool section");
                out.pool.push_back(uint32_t(parseInt(body.substr(6))));
                continue;
            }
            lines.push_back({number, body});
            ++count;
        } catch (const std::exception& e) {
            fail("line " + std::to_string(number) + ": " + e.what());
        }
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        try {
            const std::string& b = lines[i].body;
            if (b.compare(0, 5, ".word") == 0) out.code.push_back(uint32_t(parseInt(b.substr(5))));
            else out.code.push_back(assembleLine(b, uint32_t(i), labels));
        } catch (const std::exception& e) {
            fail("line " + std::to_string(lines[i].number) + ": " + e.what());
        }
    }
    return out;
}

}  // namespace cb2
