# Modding

A mod is a set of changes to a level's entries (maps, models, textures, scripts, entities). The tools read the
player's own NSF/NSD files, apply the changes and write a new pair. Nothing from the game is stored in this repository.

## What works today (verified)
| Capability | Tool / API | Verification |
| --- | --- | --- |
| Read any level, expand compressed chunks | `loadPages`, `readEntryPage` | 38 files, 2,548 chunks |
| Edit entries and repack chunks | `packEntryPage` | 1,759 entry pages repack byte-identical |
| Write a valid NSF (checksums included) | `writeNsf`, `finalizePage` | all 2,548 checksums match; file round trip identical |
| Regenerate the NSD index after edits | `rebuildNsd` | 38 NSDs rebuilt byte-identical |
| View the result | `cb2_viewer <modded NSF>` | demo mod (swapped vertex colours) loads and renders |
| Play the result | `cb2_play <game folder> --mod <mod folder>` | the swapped-colour demo mod of `S1\S000001C.NSF` runs in the real game (mushrooms and rocks turn blue) |

`cb2_roundtrip <folder>` runs all of the checks above and exits non-zero on any mismatch.
`cb2_mod_demo <level.NSF> <out folder>` is a complete example (edit entries, repack, write NSF, rebuild NSD).

## Installing a mod
Put the modded files in a folder that mirrors the disc layout and pass it with `--mod`:

```
mods\myhack\S1\S000001C.NSF
mods\myhack\S1\S000001C.NSD
build\Release\cb2_play.exe <game folder> --mod mods\myhack
```

The virtual disc is built at start-up from the original folder plus the overlays (later `--mod` folders win), so the
original files are never modified and sizes may change freely. A `SCUS_941.54` inside a mod folder replaces the
executable, which is how engine-level patches are delivered. The first level of a new game is `S1\S000001C`
(`cb2_boot --cd` prints every file the game reads).

Notes:
- Output NSFs store every page uncompressed (`0x1234`). The reader accepts both forms; the original PS1 loader
  has not been tested with them, which does not matter for this port because it loads the files itself.
- A page holds 64 KiB. `packEntryPage` throws `std::length_error` when entries do not fit, so a mod that grows
  an entry must move entries between pages (epackPage moves the overflow to new pages; see docs/EDITING.md).
- Spawn points and the loading bitmap live in the NSD tail and are carried over unchanged for now.

## What is not built yet
| Goal | What is missing |
| --- | --- |
| Custom maps | Authoring path: import a mesh (OBJ/glTF) into a scenery entry (12-bit vertex grid, 11-bit texture indices, colour table) and a texture atlas into PS1 texture pages. Collision and zone/entity placement entries must be authored too (their formats are not parsed yet). |
| Custom models | Same for animated model entries (type 2) and their animation frames. |
| Custom logic | Done for bytecode: cb2_gool assembler/patcher (docs/EDITING.md). Missing: authoring **new native code**, see below. |
| Playing the result | Done: `cb2_play --mod` runs modded files in the real engine. The recompiled, readable-C++ engine is still planned (see `ARCHITECTURE.md`). |

## Logic mods and native code
Script bytecode is portable, but 54% of the shipped script words are native MIPS blocks (817 unique). A mod that
adds logic may ship new blocks. Statically recompiled code cannot include code that did not exist at build time, so the
engine will have two paths for these blocks:

1. **Known blocks** (matched by hash of their words): the pre-translated C++ version.
2. **Unknown blocks:** an R3000 interpreter (`src/r3000.hpp` is its runtime) that runs the block as is.

The same interpreter doubles as the reference for differential tests of the translated code. Modders can also write
logic purely in GOOL bytecode, which needs no native code at all.

## Presentation improvements without touching game logic
Game state advances in fixed 30 Hz steps exactly as on the PS1. Things that only affect presentation can be
improved safely: interpolated rendering at higher frame rates, widescreen, higher resolution, filtering. Anything
that changes object behaviour (physics, collision, script timing) is kept faithful by default.
