# Crash Bandicoot 2 native PC port (C++)

Runs the original game on PC from your own game files: the original engine code and GOOL scripts execute unmodified on a
cycle-approximate PlayStation platform layer (R3000 + exact GTE, GPU, SPU, CD-ROM, memory card, BIOS replacement),
with an OpenGL window, audio and keyboard/gamepad input. Game data is read at runtime and never copied into this
project; point it at your own files. Includes a level viewer, an NSF/NSD read/write library for modding, and a verified
GOOL VM for the decompiler work. Architecture: `docs/PLATFORM.md`, `docs/ARCHITECTURE.md`.

## Install (from a release)

1. Download `CB2-Online-v<version>.zip` from the [Releases](../../releases) page and unzip it.
2. Run `Play.bat` (or `Crash2.exe`). The first time, the game asks for **your own disc image** of Crash Bandicoot 2 (NTSC-U,
   `SCUS_941.54`): pick the `.cue` (or `.bin` / `.iso`) file and it copies the files it needs into the `data` folder, once. The
   release does not contain the game (it is copyrighted, so it can't be shared here). You can also drop the `.cue`/`.iso`
   on `Crash2.exe`, or run `Crash2.exe --import "path\to\disc.cue"`. Compressed formats (`.chd`, `.pbp`, `.ecm`) are not supported.
   If you already have the files extracted, copy `SCUS_941.54`, `SYSTEM.CNF` and `S0`..`S3` into `data` yourself.
3. The game keeps itself up to date from then on (see Updates below).

## Build (Windows, Visual Studio 2022 + CMake)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

GLFW and GLM are fetched by CMake. glad, `stb_image_write.h` and `miniaudio.h` are in `third_party/`.

## The game folder (`dist\Crash2`)

`powershell -File tools\make_folder.ps1` (after building the `cb2_game` target) makes a folder you can double-click:

```
Crash2.exe            the game: the original code on the emulated PlayStation layer, drawn by OpenGL (crate icon, no console)
data\                 your own game files (SCUS_941.54, SYSTEM.CNF, S0..S3), found automatically; edit or swap them freely
cb2.cfg               settings (name, server, scale ...)
Crash2 Server.exe     rendezvous server (no game): introduces players to each other across the internet; also builds for Linux (`server/`)
*.bat                 shortcuts: play, play offline (no lobby), start the rendezvous server
```

Saves (`memcard0.mcr`) are kept next to the exe. The folder holds your own game files, so keep it for yourself.
**This repository contains no game data**; you need your own copy of the game (see `NOTICE.md`). Source code: MIT (`LICENSE`).
`tools\make_exe.ps1` builds the alternative single-file `Crash2.exe` with the data embedded (`cb2_pack`).

**Online play**: `Crash2.exe` starts with a lobby (play offline or online, room, name, server). There is no host and nobody types an
address: players in the same room find each other on the same PC / network automatically, or through a rendezvous server on a cloud
machine; they can leave and come back at any time. Other players appear inside the original game as extra Crashes with their name
floating over the head. `--online --room NAME --rendezvous host` skip the lobby. An optional lobby service (not in this repo, protocol in
`docs/LOBBY_PROTOCOL.md`) adds a public list of open servers and rooms. See `docs/ONLINE.md`.

## Play

```
build\Release\cb2_play.exe <folder with SCUS_941.54 and S0-S3> [--mod <folder>]... [--card file.mcr]
```

The folder is your extracted disc: `SCUS_941.54`, `SYSTEM.CNF`, `S0`..`S3`. The game boots to the title screen on its own.
Default keys (rebindable in the `Esc` menu): arrows = D-pad, `Z` Cross (jump), `X` Circle, `A` Square (spin), `S` Triangle,
`Q`/`W` L1/R1, `E`/`R` L2/R2, `Enter` Start, Right Shift Select, `F12` screenshot. A gamepad (XInput/SDL mapping) works as well. Saves go to `memcard0.mcr`
(standard 128 KiB PS1 card image).

`--mod <folder>` overlays files onto the disc: any file in the mod folder (same relative path, e.g. `S1\S000001C.NSF`)
replaces the original, and new files are added; several `--mod` options stack. `--ram-mb 8|16` gives the engine heap
more memory than the 2 MiB of the retail console. See `docs/MODDING.md` and `docs/EDITING.md`.

Player keys: `Esc` opens the in-game menu (see below), `F1` cycles the display filter (nearest / bilinear / sharp
bilinear), `F11` fullscreen, hold `Tab` to fast-forward, `F2` toggles the OpenGL renderer.

**In-game menu (`Esc`).** Pauses the game and offers Resume, Controls, Volume, Picture (the `F1` filter), Fullscreen,
Name tags (online only), Main menu and Quit. *Controls* rebinds all 14 PS1 buttons: pick a row, press Enter, press the new
key (a key already in use swaps with the one you pick; `Del` clears a binding; `Esc`, `Tab`, `F1`, `F2`, `F11` and `F12`
are reserved). *Main menu* starts a fresh copy of the game at the lobby, so you can pick another mode or room; unsaved
progress is lost. A game pad navigates it too (D-pad, A = choose, B = back). Online, the others see you stand still while
the menu is open. Everything you change is remembered in `cb2.cfg` (`volume`, `filter`, `nametags`, `key_<button>`).

The picture is drawn by an OpenGL renderer at `--scale N` (default 4) times the original resolution. Projected
vertices keep their unrounded GTE positions, so geometry does not wobble at high resolution (`CB2_NOPRECISE=1`
disables this). `--native` runs the statically translated engine code (identical output to the interpreter).

Settings can live in `cb2.cfg` (see `cb2.cfg.example`): game folder, mods, memory card, scale and RAM size. With it,
`cb2_play.exe` needs no arguments; the game data stays in its own folder, separate from the engine.

**Updates.** When the lobby opens, the game checks the GitHub Releases of this project for a newer version (one request, in
the background). If there is one, the first row of the lobby becomes *UPDATE TO Vx.y.z*; choosing it downloads the new
`Crash2.exe`, verifies its SHA-256, and restarts the game. Your game files, settings and saves are not touched.
`update = off` in `cb2.cfg` disables the check. How releases are made: `docs/RELEASING.md`.

## Other tools

```
build\Release\cb2_viewer.exe <folder-with-S0-S3 | one .NSF> [--level N] [--screenshot out.png]   # scenery viewer
build\Release\cb2_boot.exe   <folder> [instr] [--frames N] [--press 2310-2314:start] [--shot 2400:f.ppm] [--wav f.wav]
build\Release\cb2_check.exe  <folder>      # parses every NSF, exits non-zero on any failure
build\Release\cb2_roundtrip.exe <folder>   # NSF/NSD writer round trip, exits non-zero on any mismatch
build\Release\cb2_gtetest.exe analysis\gte\tests.bin    # GTE vs real-hardware captures (tools\gte_tests_convert.py)
build\Release\cb2_memtest.exe              # memory card filesystem self-test
build\Release\cb2_vmdiff.exe <SCUS_941.54> [cases] [seed]   # GOOL VM vs the game's own interpreter
build\Release\cb2_mod_demo.exe <level.NSF> <out folder>     # example mod (see docs/MODDING.md)
build\Release\cb2_gool.exe  list|dump|patch|test ...        # GOOL assembler / script patcher (docs/EDITING.md)
build\Release\cb2_scene.exe list|export|import|test ...     # scenery <-> OBJ editor pipeline (docs/EDITING.md)
build\Release\cb2_models.exe <level.NSF>                 # models and animations in a level
build\Release\cb2_entities.exe <level.NSF>               # zones, spawn points and entities of a level
build\Release\cb2_crashtrace.exe <folder> --press ... --follow <object> --from F --to F   # records an object of the running game frame by frame
build\Release\cb2_mpspike.exe <folder> --press ... --spawn F --type T --subtype S   # research tool: creates an object in the running game
```

`cb2_boot` is the headless harness: it runs the game for N instructions/frames, applies scripted pad input, saves
frames and audio, and reports which BIOS calls and hardware registers the game used.

Viewer controls: hold right mouse to look, `WASD` move, `Q`/`E` down/up, `Shift` fast,
`Tab` / `Shift+Tab` next/previous level, `F` reframe, `Esc` quit.

## Status

| Milestone | State |
| --- | --- |
| Platform layer | Working: the game boots, plays the intro, and reaches gameplay with controls, audio and rendering. Details in `docs/PLATFORM.md` |
| R3000 + GTE | R3000 verified against Unicorn; GTE passes all 1,150 captures taken from real hardware (22 commands) |
| GPU | Software rasteriser into a 1024x512 VRAM (polygons, sprites, lines, blending, dithering, texture windows); not yet pixel-exact against hardware |
| SPU | ADPCM voices, ADSR, loops, key on/off, ENDX, RAM transfers; no reverb, noise or pitch modulation yet |
| CD-ROM | Controller registers, ReadN/Setloc/GetID etc. over an ISO9660 volume synthesised from the loose files (with mod overlay) |
| Memory card | BIOS card API and file API on a native-layout card image; card detection and an empty-card Load Game screen verified in-game, save path covered by `cb2_memtest` only |
| M1 NSF format + C++ parser | Done. 39 files, 2,762 chunks, 696 scenery worlds parse with no errors |
| M2 OpenGL scenery viewer | Working: textured world geometry, 4/8/15-bit CLUT textures, flipbook animation, sky |
| GOOL VM | Reference VM covering all 79 opcodes, including native blocks via the R3000 core, verified against the game's own interpreter on 38,000 random programs with 0 mismatches (`cb2_vmdiff`) |
| Online play | Extra Crashes for the other players inside the original game, with name tags (`docs/ONLINE.md`). Tested with two instances on one PC and a test ghost; the others are shown only, no interaction |
| M4 Modding pipeline | NSF/NSD writer verified byte-exact on all levels; modded NSFs load into the running game through `--mod`. GOOL assembler (all 453 scripts round-trip exactly) and scenery-to-OBJ editor (all 696 worlds round-trip exactly) are in; see `docs/EDITING.md`. Not written: new native code, textures/collision/entity/model authoring |

Not done yet: SPU reverb, XA/CD audio, an OpenGL GPU backend with upscaling, a GOOL-to-C++ decompiler, a static
recompiler, pixel-exact GPU comparison against hardware. The interpreter plus software GPU currently runs the game at
roughly 1.5x real time on the development machine, so slower CPUs will need the recompiler or an optimised core.

`tools/nsf_probe.py` is the Python prototype used to validate the format before the C++ port.
See `docs/FORMAT.md` for the layout notes.
