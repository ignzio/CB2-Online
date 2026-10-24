#pragma once
// GOOL instruction decoding. Bit layouts come from the community opcode formats; the 12-bit operand "ref"
// encoding was verified against the game's own operand resolvers (value resolver at 0x80039E80).
#include <cstdint>
#include <string>
#include <vector>

namespace cb2 {

constexpr int kGoolOpcodeCount = 79;   // size of the executable's handler table
constexpr uint32_t kStackPopRef = 0xE1F;  // value ref that pops the operand stack

enum class FieldKind : uint8_t { Bits, Ref, Dest, Field, Signed };

struct FieldSpec {
    char name;
    FieldKind kind;
    uint8_t shift;
    uint8_t width;
};

struct OpSpec {
    int opcode;
    const char* name;
    std::vector<FieldSpec> fields;
};

// nullptr for opcodes with no known operand layout (74-77 exist in the executable but are unnamed).
const OpSpec* opSpec(int opcode);

inline int opcodeOf(uint32_t word) { return int(word >> 24); }

// Raw (unsigned) field value, or sign-extended for FieldKind::Signed.
int32_t fieldValue(uint32_t word, const FieldSpec& f);

enum class RefKind : uint8_t {
    LocalPool,     // pool constant from the object's own GOOL entry (s4)
    ExternalPool,  // pool constant from the linked GOOL entry (s3)
    Imm9,          // signed 9-bit immediate, scaled << 8
    Imm8,          // signed 8-bit immediate, scaled << 4
    StackSlot,     // *(frame + signext7 * 4); negative = argument
    SmallConst,    // 0 or 1
    LinkField,     // linked process field: link[(ref >> 6) & 7]->field[ref & 0x3F]
    SelfField,     // own process field: proc[0x40 + 4 * (ref & 0x1FF)]
    StackPop,      // pop the operand stack
};

struct RefInfo {
    RefKind kind;
    int32_t index;  // pool index, immediate value, slot, constant, or field number
    int link;       // link number for LinkField
};

RefInfo decodeRef(uint32_t ref);

// Destination refs (store targets) use only the low 11 bits; verified against the resolver at 0x80039E10.
RefInfo decodeDest(uint32_t ref12);

std::string refText(uint32_t ref12);
std::string disassemble(uint32_t word);

}  // namespace cb2
