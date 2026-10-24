#include "gool_decode.hpp"

#include <cstdio>
#include <map>

namespace cb2 {
namespace {

const FieldKind kBits = FieldKind::Bits, kRef = FieldKind::Ref, kDest = FieldKind::Dest, kField = FieldKind::Field,
                kSigned = FieldKind::Signed;

const std::vector<OpSpec>& specs() {
    static const std::vector<OpSpec> table = {
#include "gool_ops.inc"
    };
    return table;
}

int32_t signExtend(uint32_t v, int bits) {
    const uint32_t m = 1u << (bits - 1);
    return int32_t((v ^ m) - m);
}

}  // namespace

const OpSpec* opSpec(int opcode) {
    static const std::map<int, const OpSpec*> index = [] {
        std::map<int, const OpSpec*> m;
        for (const OpSpec& s : specs()) m[s.opcode] = &s;
        return m;
    }();
    const auto it = index.find(opcode);
    return it == index.end() ? nullptr : it->second;
}

int32_t fieldValue(uint32_t word, const FieldSpec& f) {
    const uint32_t v = (word >> f.shift) & ((1u << f.width) - 1u);
    return f.kind == FieldKind::Signed ? signExtend(v, f.width) : int32_t(v);
}

RefInfo decodeRef(uint32_t ref) {
    ref &= 0xFFF;
    if (!(ref & 0x800)) {
        const int32_t idx = int32_t(ref & 0x3FF);
        return {(ref & 0x400) ? RefKind::ExternalPool : RefKind::LocalPool, idx, 0};
    }
    if (!(ref & 0x400)) {
        if (!(ref & 0x200)) return {RefKind::Imm9, signExtend(ref & 0x1FF, 9) * 256, 0};
        if (!(ref & 0x100)) return {RefKind::Imm8, signExtend(ref & 0xFF, 8) * 16, 0};
        if (!(ref & 0x80)) return {RefKind::StackSlot, signExtend(ref & 0x7F, 7), 0};
        return {RefKind::SmallConst, int32_t((ref >> 4) & 1), 0};
    }
    if (!(ref & 0x200)) return {RefKind::LinkField, int32_t(ref & 0x3F), int((ref >> 6) & 7)};
    if (ref == kStackPopRef) return {RefKind::StackPop, 0, 0};
    return {RefKind::SelfField, int32_t(ref & 0x1FF), 0};
}

RefInfo decodeDest(uint32_t ref) {
    ref &= 0x7FF;  // bit 11 is not examined by the original resolver
    if (!(ref & 0x400)) return {RefKind::StackSlot, signExtend(ref & 0x7F, 7), 0};
    if (!(ref & 0x200)) return {RefKind::LinkField, int32_t(ref & 0x3F), int((ref >> 6) & 7)};
    if ((ref & 0x1FF) == 0x1F) return {RefKind::StackPop, 0, 0};  // push to the operand stack
    return {RefKind::SelfField, int32_t(ref & 0x1FF), 0};
}

namespace {

std::string infoText(const RefInfo& r) {
    char buf[64];
    switch (r.kind) {
        case RefKind::LocalPool: std::snprintf(buf, sizeof buf, "L[%d]", r.index); break;
        case RefKind::ExternalPool: std::snprintf(buf, sizeof buf, "EL[%d]", r.index); break;
        case RefKind::Imm9:
        case RefKind::Imm8: std::snprintf(buf, sizeof buf, "#%d", r.index); break;
        case RefKind::StackSlot:
            std::snprintf(buf, sizeof buf, r.index >= 0 ? "stack[%d]" : "arg[%d]", r.index >= 0 ? r.index : -r.index - 1);
            break;
        case RefKind::SmallConst: std::snprintf(buf, sizeof buf, "%d", r.index); break;
        case RefKind::LinkField: std::snprintf(buf, sizeof buf, "link%d.f%d", r.link, r.index); break;
        case RefKind::SelfField: std::snprintf(buf, sizeof buf, "f%d", r.index); break;
        case RefKind::StackPop: std::snprintf(buf, sizeof buf, "[sp]"); break;
    }
    return buf;
}

}  // namespace

std::string refText(uint32_t ref12) { return infoText(decodeRef(ref12)); }

std::string disassemble(uint32_t word) {
    const int op = opcodeOf(word);
    const OpSpec* s = opSpec(op);
    char head[32];
    if (!s) {
        std::snprintf(head, sizeof head, "op%d", op);
        return std::string(head) + (op < kGoolOpcodeCount ? " ?" : " <data>");
    }
    std::string out = s->name;
    for (const FieldSpec& f : s->fields) {
        char buf[48];
        const int32_t v = fieldValue(word, f);
        if (f.kind == FieldKind::Ref) out += std::string(" ") + f.name + "=" + refText(uint32_t(v));
        else if (f.kind == FieldKind::Dest) out += std::string(" ") + f.name + "=" + infoText(decodeDest(uint32_t(v)));
        else { std::snprintf(buf, sizeof buf, " %c=%d", f.name, v); out += buf; }
    }
    return out;
}

}  // namespace cb2
