# The two releases: European (PAL) and American (NTSC-U)

CB2-Online runs both releases of Crash Bandicoot 2 from the same program. A **release** is identified by its executable:

| | American (NTSC-U) | European (PAL) |
| --- | --- | --- |
| Executable on the disc | `SCUS_941.54` | `SCES_009.67` |
| Game folder | `data` | `data-pal` |
| Video | 59.94 Hz | 50 Hz |
| Records | `records.txt` | `records-pal.txt` |
| Save states | `states\slotN.cb2state` | `states\pal-slotN.cb2state` |
| Online | rooms of the American release | rooms of the European release |

The console emulation is the same; what differs is the game's own code. Both executables have the same size and load address, but the engine's functions sit at different
addresses (and moved by different amounts in different parts of the file), so every address the program uses to look inside the game is kept per release in
[`src/game_profile.cpp`](../src/game_profile.cpp): the object routines the multiplayer module hooks, the scenery projection routines of the native world renderer, the player object, the
camera, the vertical-blank wait, the memory-card stubs and so on. The profile also carries the frame length (`564480` cycles NTSC, `677376` PAL) so that the emulated time runs at the
release's own speed; the level timer, the pacing of the window and the 60 fps smoothing follow from it.

## Choosing a release

`cb2.cfg`: `game = data` (American folder), `game_pal = data-pal` (European folder), `region = auto | pal | ntsc`. With `auto` the European release is played when it is installed.
The disc importer accepts either disc and puts the files in the matching folder, so you can import both. `--game <folder>` plays exactly that folder.

Save states record the release they were made on and refuse to load on the other one.

## What differs in practice

- **Speed:** the translated ("native") engine code is a translation of the American executable, so the European release runs on the interpreter (3-4 ms a frame on a normal PC, about a tenth of
  the frame budget). `--native` is ignored for it.
- **Online:** players of the two releases never share a room (`docs/ONLINE.md`).
- **Native world renderer, widescreen, 60 fps smoothing, level timer, records:** work on both.

## Adding the addresses of a release (or checking a new dump)

`tools/pal_map.py` finds, for an address in the American executable, the address of the same function in another executable by searching for the function's code with
the instruction fields that hold addresses masked out:

```
python tools\pal_map.py SCUS_941.54 SCES_009.67 80019800 8001C718
```

Check every result by disassembling both (`python tools\psxdis.py <exe> <hexaddr> <count>`), then run the game with the new profile: the multiplayer test ghost
(`--fake-player`), `CB2_NWLOG=1` for the native world (the camera it finds must equal the game's), and the timer. When the game hangs at start-up, `CB2_STEPDUMP` in `cb2_boot` shows what the
CPU is doing. The first bug of the European port was an NTSC memory-card stub address that landed in the middle of another European routine and made the CPU loop on it.
