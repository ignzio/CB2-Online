# Architecture: an OpenGOAL-style Crash Bandicoot 2

Goal: a native, playable port that reproduces the original game's logic exactly, reading the player's own
game files at runtime (nothing from the game is stored in this repository).

## Why it works like OpenGOAL
Naughty Dog built Crash 2 in two layers, the same split OpenGOAL relies on for Jak and Daxter:

1. **Engine (MIPS code in `SCUS_941.54`)**: GOOL virtual machine, object/process scheduler, collision,
   camera, renderer, level and chunk streaming, memory management, audio, input.
2. **Game logic (GOOL bytecode in the NSF files)**: every game object (Crash, crates, enemies, bosses, camera
   control, HUD, menus) is a GOOL process. There are 453 scripts in the shipped levels (46,268 instruction words).

Exact behaviour therefore needs two things: a bit-accurate GOOL VM, and bit-accurate reimplementations of the
native engine services the scripts call.

## Current state (supersedes the plan below)
The game now runs end to end through the platform layer described in `docs/PLATFORM.md`: the original executable and
scripts execute on the R3000 core with an exact GTE, and the platform provides GPU, SPU, CD-ROM, memory card and a BIOS
replacement. Boot, menus, cutscenes and gameplay work with audio and input. The sections below describe the longer
term plan (static recompilation, readable C++ scripts) that sits on top of this working baseline; the platform layer is
also the reference the recompiled build is compared against, frame by frame.

## Components
| Layer | Module | State |
| --- | --- | --- |
| Data | `nsf.*` chunk/entry reader, LZSS decoder | done, 38/38 files |
| Data | `scenery.*` world geometry and textures | done |
| Data | `gool.*` script container (header, pool, code, states) | done, 453/453 scripts |
| Logic | GOOL decoder (`gool_decode.*`) | done: 356,778 bytecode instructions decoded, 46,046 branches all in range, no unknown words, see `docs/GOOL.md` |
| Logic | GOOL reference VM (`gool_vm.*`, `gool_native.*`) | done, exact against the game's interpreter (38,000 random programs) |
| Logic | Original engine + scripts | running unmodified on the R3000 core (`r3000.*`, `gte.*`) |
| Platform | GPU, SPU, CD-ROM, memory card, BIOS HLE, interrupts, DMA, timers | working, see `docs/PLATFORM.md` |
| Render | Software GPU to VRAM shown through OpenGL; scenery viewer | working; OpenGL GPU backend with upscaling is next |
| Platform | Input, audio output, save data | keyboard/gamepad, miniaudio, `.mcr` card image |
| Modding | NSF/NSD repack, disc overlay (`--mod`) | working |
| Logic | GOOL decompiler to readable C++, static recompilation | planned

## Fidelity strategy
- The original code is the specification. Every engine function that is reimplemented is checked against the
  disassembly of `SCUS_941.54` (function address recorded in a comment).
- Integer and fixed-point arithmetic is reproduced as the PS1 does it. No floats in game logic; floats only at
  the rendering boundary.
- Frame stepping is locked to the original update order and 30 Hz logic rate.
- Verification: a deterministic trace (object positions, state, RNG) is compared frame by frame against a
  reference recorded from the original game running in an emulator.

## Graphics API
OpenGL 3.3 is enough for the PS1 renderer (indexed textures are decoded in the fragment shader already).
The renderer sits behind a small interface, so Vulkan or D3D11 can replace it later without touching game logic.

## Finding: scripts embed native MIPS
The GOOL `MIPS` instruction calls machine code stored inside the script. 416,538 of 773,316 script words (54%) are
such code, in 7,265 blocks of which only 817 are unique. They are position-independent leaf routines (see
`docs/GOOL.md`), so they translate once each. Roughly 20 other opcodes are thin trampolines into native engine
routines (animation, events, spawning, collision, sound, chunk loading). So "exact logic" means three layers, not
two: bytecode, native routines inside scripts, and the native engine.

## Static recompilation plan (chosen approach)
The native layers (the 758 functions of `SCUS_941.54` and the 817 unique script routines) are translated to C++ by
a lifter that runs locally on the user's own files; the generated code is a build artifact and is never committed.
Measured scope of the executable: 62,858 instruction words in 758 functions, 11 functions use the GTE (275 GTE
instructions plus 279 `lwc2`/`swc2`), 5 load-delay hazards that read the old register value, 44 `lwl`/`lwr` and 32
`swl`/`swr`, 928 indirect jumps. Steps:
1. Lifter for the integer ISA with exact branch- and load-delay behaviour, plus a runtime (PS1 memory map,
   register file, GTE).
2. Differential tests of lifted functions against an independent emulator (Unicorn) on random machine states.
3. HLE of the Sony library layer (GPU/CD/SPU/pad/memory-card calls) onto OpenGL, audio and input.
4. Port the GOOL interpreter and scheduler to C++ on top of the lifted code, then replace lifted functions with
   readable C++ one by one, keeping the lifted version as the reference for tests.

## Script pipeline (decompile and build)
```
GOOL bytecode  ->  IR (control-flow graph, stack slots to variables)  ->  readable C++ (states, events)
                                                                              |
engine MIPS  ->  hand port guided by Ghidra/REA decompilation  ->  runtime library  ->  native PC build
```
The verified reference VM (`gool_vm.*`) already runs real GOOL bytecode and is exact against the original interpreter,
so it is both the first runtime and the oracle for the decompiler: generated C++ is checked against it on identical
process state with the same service-call log. Readability passes (structured control flow, expression folding, naming
from the CrashEdit field table in `gool_fields.inc`) are re-checked against the same oracle. Mods are written as C++ against the
runtime library, so no interpreter is needed for new native code.

Ordering decision: because the VM is exact and the 817 native blocks run correctly on the R3000 core today, the next
engine step is porting the 25 services the scripts call, not the decompiler. The decompiler is built on top afterwards,
where it improves modding and speed rather than blocking gameplay.

## Tooling for analysis
- `tools/psx_xref.py`: Capstone-based MIPS cross-references for PS-X EXE files.
- Rizin 0.9.1 for disassembly and auto-analysis of the executable.
- REA works on the executable through the `rea-wsl` MCP server (Ubuntu 24.04 in WSL with Node and Rizin). On native
  Windows its providers refuse to run (`ERR_REA_WINDOWS_NATIVE`).
- Analysis output lives in `analysis/` and is not committed (it is derived from the game executable).
