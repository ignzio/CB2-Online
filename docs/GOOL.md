# GOOL virtual machine notes (Crash Bandicoot 2, NTSC-U `SCUS_941.54`)

Everything here was checked against the game's own machine code (disassembled with REA + Rizin) and against all
453 GOOL entries in the shipped level files. Addresses are in the executable's RAM image (load address `0x80010000`).

## Entry layout (type 0x0B)
Items, in order: `0` header (24 bytes), `1` **bytecode**, `2` **constant pool**, `3` state map (`int16[]`),
`4` state descriptors (16 bytes each), `5` frame groups (animation/texture data).
Some community loaders name items 1 and 2 the other way round. The data settles it: item 1 is 94% valid opcodes
(773,316 words in total); item 2 starts with `entryCount` entry IDs, followed by constants.
Header: `id, class, format, heapBase, eventCount, entryCount` (six `int32`).

## Interpreter
- Function `0x8003A014`. Registers: `s0` process, `s1`/`s2` caller arguments, `s3`/`s4` pool bases, `s5` instruction
  pointer (saved to `proc+0xC0`), `s6` operand-stack pointer (`proc+0xBC`), `s7` frame base (`proc+0xC4`),
  `fp` = `0x1F800000` (scratchpad).
- Loop at `0x8003A06C`: `ins = *s5++; handler = table[ins >> 24]; jr handler`. The 79-entry table is at
  `0x8005C514` (opcodes 0-78) and its address is kept in scratchpad word `0x1F80005C`.
- Operand resolvers share the interpreter's registers instead of the C calling convention:
  value resolver `0x80039E80`, destination resolver `0x80039E10`, process-field helper `0x80039F74`.
- Operands are resolved **high field first**: for `ADD`/`SUB` the handler resolves bits 23..12, then bits 11..0,
  and computes `low - high` (`SUB`). This matters when both operands pop the stack.
- Results are pushed with `s6 += 4; *(s6 - 4) = value`. Handlers jump back to the loop with `bgez zero, 0x8003A06C`.

## Instruction word
`opcode = word >> 24`. The 24 operand bits are described by format strings that read **LSB-first** (the first
character is bit 0). Examples (bit ranges):

| Op | Fields |
|---|---|
| ADD, SUB, ... | `L` ref bits 0-11, `R` ref bits 12-23 |
| SETF | `D` dest bits 0-11, `S` ref bits 12-23 |
| BRA / BNEZ / BEQZ | offset `I` signed bits 0-9, pop count `V` bits 10-13, process field `R` bits 14-19 |

Branch target = index of the next instruction + `I`. `BNEZ`/`BEQZ` read process field `R` (31 = pop the stack) and
pop `V` values from the operand stack when the branch is taken. Verified: 46,046 branches in the bytecode (native
blocks excluded), none outside their script.

## 12-bit value ref (resolver `0x80039E80`)
| bits 11..9 | meaning |
|---|---|
| `0 b10` | pool word `pool[ref & 0x3FF]`; bit 10 selects the linked entry (`s3`) or the object's own entry (`s4`) |
| `1 0 0` | `signext9(ref) << 8` |
| `1 0 1`, bit 8 = 0 | `signext8(ref) << 4` |
| `1 0 1 1 0` | stack/argument slot `*(frame + signext7(ref) * 4)` |
| `1 0 1 1 1` | 0 or 1 taken from bit 4 |
| `1 1 0` | `link[(ref >> 6) & 7]->field[ref & 0x3F]`, 0 when the link is null |
| `1 1 1` | own field `proc[0x40 + 4 * (ref & 0x1FF)]`; `0xE1F` pops the operand stack |

Destination refs (`0x80039E10`) use bits 0-10 only: stack slot, linked field, own field, or push (`field 0x1F`).

## Embedded native code
The `MIPS` instruction (op 73, `0x49BE0BE0`) executes `jalr s5`, i.e. it calls the MIPS code that follows the marker
with the interpreter's registers live. The block ends with `jalr s5, ra` (`0x03E0A809`) plus its delay slot: that
returns to the interpreter and links the address after the block into `s5`, so the bytecode resumes right there.

Measured on the shipped data: 7,265 native blocks hold 416,538 of 773,316 script words (54%), but only **817 are
unique** (median 17 words, longest 2,076). They are leaf routines on the process state (`s0`), the operand stack
(`s6`) and the frame (`s7`): relative branches only, no calls into the executable (one `jal` target in total) and
one GTE instruction in total. With this block boundary every remaining script word decodes as a valid opcode
(0 unknown words) and all 46,046 bytecode branches land inside their script.
The routines are position-independent, so each unique block can be translated once.

## Opcode handler map
Opcode names are the community names. "native" handlers are 3-instruction trampolines that call an engine routine.

| Op | Name | Handler | | Op | Name | Handler |
|---|---|---|---|---|---|---|
| 0 | ADD | 8003A0D4 | | 40 | EFLR | 8003A8F8 native |
| 1 | SUB | 8003A0F8 | | 41 | EFLV | 8003A904 native |
| 2 | MUL | 8003A11C | | 42 | ARRL | 8003A910 |
| 3 | DIV | 8003A144 | | 43 | SIN | 8003A970 |
| 4 | EQL | 8003A200 | | 44 | COS | 8003A994 |
| 5 | AND | 8003A1B4 | | 45 | ATAN | 8003AA14 |
| 6 | OR | 8003A1DC (same as 8) | | 46 | UNK46 | 8003ADD8 |
| 7 | ANDL | 8003A2E0 | | 47 | NOP | 8003A06C |
| 8 | ORL | 8003A1DC | | 48 | DBG | 8003AA74 native |
| 9 | LT | 8003A228 | | 49 | RET | 8003ABE8 |
| 10 | LTE | 8003A24C | | 50 | BRA | 8003AC98 |
| 11 | GT | 8003A274 | | 51 | BNEZ | 8003ACB4 |
| 12 | GTE | 8003A298 | | 52 | BEQZ | 8003ACE4 |
| 13 | MOD | 8003A16C | | 53 | GO | 8003AD08 native |
| 14 | XORL | 8003A304 | | 54 | GNEZ | 8003AD14 |
| 15 | BITS | 8003A328 | | 55 | GEQZ | 8003AD3C |
| 16 | RAND | 8003A354 | | 56 | ANIM | 8003AD68 native |
| 17 | SETF | 8003A3C0 | | 57 | ANIF | 8003AD74 native |
| 18 | NOT | 8003A194 | | 58 | VEC | 8003AA80 native |
| 19 | LOOP | 8003A7B8 | | 59 | CALL | 8003AAEC |
| 20 | LEA | 8003A3DC | | 60 | SEVT | 8003AD80 native |
| 21 | ASH | 8003A3FC | | 61 | RJEV | 8003ADA4 (same as 62) |
| 22 | PUSH | 8003A42C | | 62 | ACEV | 8003ADA4 |
| 23 | NOTL | 8003A2C0 | | 63 | SPWN | 8003AA98 native (same as 70) |
| 24 | SETC | 8003A4A4 | | 64 | CHNK | 8003AAC8 native |
| 25 | ABS | 8003A4E8 | | 65 | SNDP | 8003AAA4 native |
| 26 | CPAD | 8003A510 | | 66 | SNDA | 8003AAB0 native |
| 27 | VEL | 8003A530 | | 67 | COLL | 8003AA8C native |
| 28 | MISC | 8003A564 | | 68 | BEVT | 8003AD98 native |
| 29 | PSIN | 8003A58C | | 69 | CEVT | 8003AD8C native |
| 30 | SYNC | 8003A5E8 | | 70 | SPWF | 8003AA98 native |
| 31 | GVAR | 8003A618 | | 71 | CALR | 8003AB64 |
| 32 | GVAW | 8003A640 | | 72 | UNK | 8003AABC native |
| 33 | DEGD | 8003AA44 | | 73 | MIPS | 8003AAD4 |
| 34 | SEEK | 8003A66C | | 74-77 | (unnamed) | 8003AE24 / 30 / 3C / 48 native |
| 35 | COLR | 8003A870 | | 78 | ARRS | 8003A940 |
| 36 | COLW | 8003A89C | | | | |
| 37 | DSEK | 8003A704 | | | | |
| 38 | PSHP | 8003A46C | | | | |
| 39 | ANIS | 8003A8C8 | | | | |

Opcodes 74-77 are implemented in the executable but have no community name; they appear 16 times in the data.

## Handler semantics read from the executable (differs from community names)
- Operands of two-operand ops are resolved **high field first** (`R`, bits 12-23), then `L` (bits 0-11).
- Op 18 (`NOT`) is a *logical* not (`!x`); op 23 (`NOTL`) is a *bitwise* not (`~x`).
- Op 5 (`AND`) returns `L` when `R != 0`, else 0 (logical); op 7 (`ANDL`) is the bitwise AND. Ops 6 and 8 share one handler.
- `LT/LTE/GT/GTE` are signed compares computed with `slt` (`LTE` = `!(R < L)`, `GTE` = `!(L < R)`); `EQL` is `L == R`.
- Source refs and destination refs encode process fields differently: a source field is `0xE00 | n`, a destination field is
  `0x600 | n` (destinations ignore bit 11, so `0x605` as a *source* is a constant-pool reference, not field 5).
  Value ref `0xE1F` pops the operand stack; destination field `0x1F` pushes.
- The interpreter keeps its state in registers: `s1` = status flags (also returned through `v0`), `s5` = instruction
  pointer, `s6` = operand stack pointer, `s7` = frame base. The operand stack and call frames live **inside the process
  object** (`proc + 0x40 ...`). `CALL` pushes a 12-byte frame `{flags, return ip, packed stack/frame offsets}`; `RET`
  pops it, and at the outermost frame (`[s7 + 8] == 0`) it returns `v0 = -0x19` to the caller.
- Scratchpad words used by handlers: `0x1F80005C` opcode table, `0x1F800050/54/58` engine variables (angle
  tables, RAND state, global array for `GVAR`/`GVAW`).

## Oracle: running the game's own interpreter
`cb2_vmrun <SCUS_941.54>` loads the executable into the R3000 core (`src/r3000.cpp`), builds a synthetic process
(own and linked constant pools at `proc+0x10/0x14` -> `entry+0x18`, code pointer `proc+0xC0`, stack `proc+0xBC`, frame
`proc+0xC4`), points `ra` at a sentinel and runs function `0x8003A014`. Verified: a small program computing
`f6 = f5 + 0x30` through the operand stack returns `f6 = 0x80`, `v0 = -0x19`. This is the reference for
differential tests of the decompiler's output and of any C++ reimplementation of a handler.
The core itself is verified by `tools/cpu_diff.py` (7,500 random programs against Unicorn, 0 mismatches; an injected
`lwr` bug was caught) and `tools/cpu_directed.py` (load/branch delay slots, lwl/lwr bypass, divide edge cases,
isolated-cache stores, all passing).

## Reference VM (`src/gool_vm.*`) and its verification
`cb2::goolRun` is a C++ implementation of the whole interpreter, written from the handlers above. It uses the game's own
memory model (32-bit addresses, process objects, operand stack and frames inside the process), so pointer semantics are
exact. Anything that needs the rest of the engine goes through `cb2::Services`, one call per original routine:

| Service | Routine | Opcodes |
|---|---|---|
| Go | `0x8003900C` | GO, GNEZ, GEQZ |
| Anim / Anif | `0x80039104` / `0x800391D8` | ANIM / ANIF |
| Sevt / Cevt / Bevt | `0x80039310` / `0x800394C4` / `0x80039684` | SEVT / CEVT / BEVT |
| Rjev | `0x8003981C` | RJEV, ACEV (flags are reloaded from `0x1F800048` afterwards) |
| Dbg / Eflr / Eflv | `0x80037440` / `0x80037448` / `0x8003756C` | DBG / EFLR / EFLV |
| Vec / Coll | `0x8003817C` / `0x80038700` | VEC / COLL |
| Spwn | `0x80038AFC` | SPWN, SPWF |
| Sndp / Snda | `0x80038C14` / `0x80038D08` | SNDP / SNDA |
| Chnk | `0x80038F00` | CHNK |
| Op72 | `0x80038E14` | opcode 72 |
| Op74-Op77 | `0x80039A14`, `0x80039AA4`, `0x80039B14`, `0x80039C08` | opcodes 74-77 |
| Misc | `0x80037698` | MISC (pool bases and the stack pointer are reloaded afterwards) |
| Cpad | `0x80036FAC` | CPAD (pad state) |
| Atan | `0x8004F078` | ATAN |
| Op46 | `0x8001EAD8` | opcode 46 (12 bytes are copied to `0x1F800020` first) |

Calls that return non-zero through `0x80039FE4` end the run. Details that are easy to get wrong and were confirmed
against the executable: a constant-0/1 ref used as an address yields the *value* 0 or 1, a null link yields address 0,
`CALL` frames are `{flags, return ip, packed offsets}` (locals start at slot 3, arguments are negative slots), ref
`0xBF0` in LOOP/SEEK/DSEK means "both operands come from the stack", and the sine table is at `0x8005BCB0`.

**Native blocks (opcode 73)** run through `Services::runNative`; `src/gool_native.*` implements it on the R3000 core.
`tools/block_livein.py` shows that all 817 distinct blocks in the shipped levels read only the interpreter's own
registers (`s0`-`s7`, `fp`, `sp`, `ra`), so those are the only registers the runner has to provide. A block ends with
`jalr s5, ra` and leaves the address after the block in `s5`.

**Verification** is `cb2_vmdiff <SCUS_941.54> [cases] [seed]`. It generates random programs (all 79 opcodes, forward
branches, `CALL`/`CALR`/`RET`, service calls, embedded native blocks), builds one memory image, and runs it through the
game's own interpreter on the R3000 core and through the C++ VM. Native engine routines are replaced by one
deterministic stub in both runs; the stub also edits process state so register-reload paths are exercised. The runs must
agree on the return value, the service-call log, the scratchpad and all memory the VM can touch.
Result: 38,000 compared cases over 12 fresh seeds (99-110; 30,551 with native blocks), **0 mismatches**, and every
opcode executed. Programs that overwrite their own code through a wild pointer are skipped, since the two cores read
stale registers differently by construction.
Three deliberately injected bugs (RAND operand order, DSEK wrap mask, native runner dropping `s6`) were each caught.
About 22% of random cases are skipped because the original faults on a wild pointer, which proves nothing either way.
When a case fails, the tool localises it: first differing native block, per-instruction trace of that block, lockstep
comparison at every bytecode instruction, and a watch on writes into the program's own code and link table. Every
failure seen so far was a generator artifact (programs overwriting their own code, frame header or link pointers),
found this way and fixed in the generator rather than the VM.

## Tools
`tools/dump_optable.py` (handler table), `tools/find_tables.py` (pointer-table scan), `tools/scratchpad_refs.py`,
`tools/show_refs.py`, `tools/psx_xref.py` (Capstone MIPS cross-references), `tools/extract_gool_ops.py` and
`tools/gen_gool_ops.py` (opcode layouts to `src/gool_ops.inc`), `cb2_check <dir> --dump Will.C` (script disassembly).
