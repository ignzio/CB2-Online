# Editing levels and scripts

Everything here works on the player's own files and writes a mod folder that `cb2_play --mod <folder>` overlays onto the
disc. Originals are never touched.

## Scripts (GOOL) - `cb2_gool`
```
cb2_gool list  <level.NSF>                       scripts in a level
cb2_gool dump  <level.NSF> <eid> [out.asm]       lossless text form
cb2_gool patch <level.NSF> <eid> <in.asm> <out>  assemble and write the modded NSF + NSD
cb2_gool test  <folder>                          assemble(disassemble(x)) == x for every script
```
The text form (`src/gool_asm.*`) names every instruction (`ADD L=f12 R=#256`, `BNEZ ->L0020 V=0 R=31`, `CALL ->L0100`),
turns branch and `CALL` targets into labels, lists the constant pool (`.const 0x...`), and keeps native blocks as
`MIPS` + `.word`. Words the assembler could not re-encode canonically carry the leftover bits in `x=0x...`, so the round
trip is exact. Verified on all 453 shipped scripts (773,316 words, 38,545 labels, 0 round-trip failures). A no-op patch
of Crash's script (`Will.C`) plays pixel-identical to the original.

Rules: a patched script may only grow (the state tables point into the code, so code is appended, labels keep targets
valid); refs are canonical (`#N` immediates must fit 9 bits << 8 or 8 bits << 4, otherwise use a pool constant); new
native (`MIPS`) code cannot be authored yet, but existing blocks can be reused. The page planner (`repackPage`) moves
entries that no longer fit in a 64 KiB chunk to new pages and the NSD is rebuilt.

Not yet done: a high-level language on top of the assembler (the decompiler to readable C++ is the planned front end),
and a symbol file naming pool constants and fields in the text.

## Scenery - `cb2_scene`
```
cb2_scene list   <level.NSF>
cb2_scene export <level.NSF> <eid> <out.obj>
cb2_scene import <level.NSF> <eid> <in.obj> <out folder>
cb2_scene test   <folder>                        entry -> OBJ -> entry is byte-identical, all worlds
```
OBJ conventions (`src/scenery_edit.hpp`): `v x y z r g b` (position / 400, colour 1.0 = 128), `usemtl tN` / `aN` selects
texture description N or animated texture N, `f` is a triangle or a quad (a-b-c, a-d-c). Positions can be moved, faces
added or removed, colours changed in Blender or any tool. Limits of the format: 4,096 vertices and 1,024 distinct colours
per world, 12-bit coordinates (-2048..2047 in 1/16 world-offset units), 11-bit texture indices. Big levels use several
worlds, so a level can still be as large as you like; each new world is a new entry.
Verified on all 696 shipped worlds (1.73 M vertices); stretching every world of the first level by 1.5 in height shows up
in the running game.

Not done yet: authoring new texture descriptions and texture pages (faces reuse existing ones), collision meshes, zone and
entity placement (where objects spawn), models and animation. Those formats exist in CrashEdit and are the next parsers.
Until collision is editable, geometry edits are visual only.

## Limits that matter
| Question | Answer (measured) |
| --- | --- |
| 2 MiB RAM | A property of the original engine's heap, not of the data. `cb2_play --ram-mb 8` / `16` hands the extra memory to the engine heap (the start-up code reads the RAM-size word at `0x8005F188`); the game plays identically at 2, 8 and 16 MiB. 32 MiB breaks the GPU ordering table (24-bit links). |
| Level size | 64 KiB chunks, 12-bit vertex indices; levels are collections of worlds, so size is bounded by memory, not by the format. The game's loader streams the level pages (the original used up to ~1.9 MiB of pages). |
| Graphics | The software GPU reproduces the PS1 picture exactly in structure; display filtering (nearest / bilinear / sharp-bilinear, `F1`), fullscreen (`F11`) and fast-forward (`Tab`) are in the player. True high resolution, widescreen and texture replacement need a GPU-side renderer fed with the game's primitives; see `docs/PLATFORM.md`. |
