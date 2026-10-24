#pragma once
// GOOL script text form: a lossless disassembler and an assembler for the bytecode of one GOOL entry.
//
//   .pool                       ; constant pool, one `.const` per entry (entry IDs first)
//   .const 0x12345678
//   .code
//   L0010:                      ; labels are generated for branch / CALL targets
//   ADD L=L[3] R=#256           ; 0010
//   BNEZ ->L0020 V=0 R=31
//   CALL ->L0100 E=0 V=0
//   MIPS
//   .word 0x...                 ; native MIPS block and anything that does not decode losslessly
//
// Instructions that the assembler would not re-encode to the identical word are emitted as `.word`, so
// assembleScript(disassembleScript(s)) == s always holds. Authoring new native (MIPS) blocks is not supported.
#include <cstdint>
#include <string>
#include <vector>

namespace cb2 {

struct GoolText {
    std::vector<uint32_t> pool;
    std::vector<uint32_t> code;
};

struct DisasmStats {
    size_t instructions = 0, nativeWords = 0, rawWords = 0, labels = 0;
};

std::string disassembleScript(const GoolText& script, DisasmStats* stats = nullptr);

// Throws std::runtime_error("line N: ...") on a syntax error or an unknown label.
GoolText assembleScript(const std::string& text);

}  // namespace cb2
