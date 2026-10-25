# Platform layer

The game runs as the original program: `SCUS_941.54` is loaded at `0x80010000` and executed by the R3000 core
(`src/r3000.*`); everything the program touches outside RAM is provided by `src/psx.*` and the device models it owns.
Because the engine, the GOOL VM and the scripts' embedded native code are the originals, game logic is exact by
construction; the work is in the devices.

```
 SCUS_941.54 + NSF/NSD  --->  R3000 core (+ GTE)  --->  Platform
                                                        |- interrupt controller, 3 root counters, DMA 0-6
                                                        |- GPU  (gpu.*)   GP0/GP1 + software rasteriser -> VRAM
                                                        |- CD   (cdrom.*) controller registers, INT1-5, data FIFO
                                                        |   `- VirtualDisc (disc.*) ISO9660 built from loose files
                                                        |- SPU  (spu.*)   24 ADPCM voices, ADSR, RAM transfers
                                                        |- memory card (memcard.*) + BIOS card/file API
                                                        `- BIOS kernel replacement (HLE)
 cb2_play: window + OpenGL texture of the display area, miniaudio output, keyboard/gamepad -> pad buffer
```

## What is emulated at the register level (the game's own library code runs on top)
- **Interrupts**: `I_STAT`/`I_MASK`, vertical blank at 59.94 Hz, CD, DMA, root counters. Exceptions enter the library's
  handler through the buffer registered with `HookEntryInt` and return through `ReturnFromException`.
- **Root counters** with clock sources (system, system/8, hblank, dot clock), reset-on-target, one-shot/repeat IRQs.
- **DMA**: GPU linked lists and blocks, CD to RAM, SPU RAM, ordering-table clear (OTC).
- **CD-ROM**: Getstat, Setloc, ReadN/ReadS, Pause, Init, Mute/Demute, Setmode, GetID, GetTN/GetTD, SeekL/P, Test.
  Sectors are served at double speed as raw mode-2 data. The disc is synthesised from the host folder (ISO9660 volume
  descriptor, path tables, directory records), so `CdSearchFile` in the game's libcd finds the NSF/NSD files; mod
  overlays are folded in when the disc is built.
- **GPU**: all drawing commands used by the game, VRAM transfers, texture windows, mask bit, dithering, draw area/offset,
  GPUSTAT. The rasteriser is software (pixel-per-pixel into a 1024x512 VRAM), so any renderer that reads VRAM is exact
  by construction; an upscaling OpenGL backend is future work.
- **SPU**: ADPCM decode, ADSR (attack/decay/sustain/release, linear and exponential), pitch, volumes, key on/off, loop
  flags and ENDX (the game polls these; without them the intro cutscene hangs), transfer address and DMA, IRQ address.
  Reverb is modelled (src/spu.cpp, original formula at 22.05 kHz; the game does not enable it). Not modelled: noise, pitch modulation, CD audio (the game uses none of these in the scenes tested).
- **GTE**: all 22 commands plus register file behaviour, verified against 1,150 real-hardware captures
  (JaCzekanski/ps1-tests `gte/test-all`, converted by `tools/gte_tests_convert.py`, run by `cb2_gtetest`).

## What is replaced (high-level BIOS)
The BIOS ROM is not available, so `Platform::biosCall` implements the functions the game links (the list is produced by
`tools/bios_stubs.py`): libc string/memory functions, `printf`, heap, `setjmp`/`longjmp`, events
(`OpenEvent`/`EnableEvent`/`TestEvent`/`WaitEvent`/`DeliverEvent` including callback events), `Enter/ExitCriticalSection`,
`InitPad`/`StartPad` (the pad buffer is refreshed every vblank), exception hooks, memory card low level
(`_card_info`/`_card_load`/`_card_read`/`_card_write`/`_new_card`) and the card file API (`open`, `read`, `write`,
`close`, `firstfile`, `nextfile`, `erase`, `format` on `bu00:`).
The kernel function tables at `0x200`, `0x674`, `0x874` hold pointers to stub addresses that dispatch back into the same
HLE, because the pad library patches and reads those tables. Library code that reaches into BIOS internals at computed
addresses is overridden by address (`Platform::overrides`). Kernel-side interrupt work (root-counter events, vblank
event) runs before the library hook, as in the BIOS.

Calls the platform does not implement are reported with the first caller by `cb2_boot` (`NOT IMPLEMENTED`), and
execution entering the kernel area raises an error instead of running garbage.

## Memory card
`MemCard` keeps a standard 128 KiB image (frame 0 header, 15 directory frames with XOR checksums, 15 data blocks),
so `.mcr` files from other emulators work. It is created formatted and empty if the file is missing. In-game
verification so far: card detection and the Load Game screen. The write path is exercised only by `cb2_memtest`.

## Verification summary
| Part | Method |
| --- | --- |
| R3000 | 7,500 random programs vs Unicorn, directed tests |
| GTE | 1,150 real-hardware captures, all pass |
| GOOL VM | 38,000 random programs vs the game's interpreter running on the R3000 core |
| NSF/NSD writer | byte-exact repack of every page and every NSD |
| Memory card | `cb2_memtest` |
| Whole system | headless runs: boot, title menu, intro cutscene, gameplay with scripted input and screenshots |

Known fidelity gaps: GPU rasterisation has not been compared with hardware pixel by pixel (rare thin slivers on some
models are visible), no reverb, the CD never reports errors, timing uses 2 cycles per instruction.

## Graphics roadmap (modern hardware)
Today the GPU is a software rasteriser into a native 1024x512 VRAM, shown through an OpenGL texture with selectable
filtering. A resolution upgrade, widescreen and texture packs need an OpenGL backend that receives the game's polygons
instead of pixels: hook `Gpu::execute` to emit each primitive (screen-space vertices, UVs, CLUT, blend mode) to a
renderer that draws into an N-times framebuffer, keeps VRAM only for texture and CLUT lookups, and applies
perspective-correct texturing, PGXP-style precise vertex positions (the GTE results before rounding are available in
`gte.cpp`) and a wider projection (the engine's camera scale constant is a data value). VRAM readbacks and
VRAM-to-VRAM copies stay on the native copy. None of this changes game logic, so the exactness of the engine is kept.
Not implemented yet.

## Running in tests
`cb2_boot <folder> 9000000000 --frames 3600 --press 2310-2314:start --shot 3590:out.ppm --wav out.wav` boots, presses
Start at frame 2310 and saves the display and the audio of the run.

GTE hardware captures (not stored in this repository):
```
mkdir analysis\gte
curl -L -o analysis\gte\tests.c https://raw.githubusercontent.com/JaCzekanski/ps1-tests/master/gte/test-all/tests.c
python tools\gte_tests_convert.py analysis\gte\tests.c analysis\gte\tests.bin
build\Release\cb2_gtetest.exe analysis\gte\tests.bin
```
