# Development

For people who want to build, test, mod or extend CB2-Online. Players do not need any of this: see the [README](../README.md).

## Build (Windows, Visual Studio 2022 + CMake)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

GLFW and GLM are fetched by CMake. glad, `stb_image_write.h` and `miniaudio.h` are in `third_party/`. The targets that matter:

| Target | What it is |
| --- | --- |
| `cb2_game` | the game as one windowed program (`Crash2.exe`, crate icon, no console); `tools\make_exe.ps1` appends the game data for the single-file variant |
| `cb2_play` | the same with a console and every command-line option (below) |
| `cb2_server` | the rendezvous server (`Crash2-Server.exe`); `server/` has a Linux build |
| `cb2_*test` | tests: `cb2_statetest`, `cb2_runtest`, `cb2_rostertest`, `cb2_interptest`, `cb2_gtetest`, `cb2_memtest`, `cb2_vmdiff`; the network test is `tools\net_test.ps1` |

Releases are made with `tools\make_release.ps1` (see [RELEASING.md](RELEASING.md)); `tools\make_folder.ps1` assembles a ready-to-run folder (`dist\Crash2`).

## Command line of `cb2_play`

```
cb2_play [game folder] [--game <folder>] [--mod <folder>]... [--card file.mcr] [--ram-mb 2..16] [--scale 1..8] [--native]
         [--online | --room NAME | --rendezvous host:port | --directory URL | --name NAME | --no-lobby | --lobby]
         [--load-state file | --save-state-at FRAME:file | --rel] [--press FROM-TO:button[+button]] [--capture FRAME:file.png]
         [--smooth | --textures sharp|smooth | --record DIR:EVERY:COUNT:START] [--fake-player N] [--fake-request FRAME:HEXLEVEL] [--poke FRAME:ADDR:VALUE]
```

`--mod <folder>` overlays files onto the disc: any file in the mod folder (same relative path, e.g. `S1\S000001C.NSF`) replaces the original, new files are
added, several `--mod` options stack. `--ram-mb 8|16` gives the engine heap more memory than the retail console's 2 MiB.
`--native` runs the statically translated engine code (identical output; save states and the online features need the default interpreter).
Settings can live in `cb2.cfg` (see `cb2.cfg.example`); command-line options override the file.

Environment switches for diagnosing: `CB2_PERF`, `CB2_PCHIST`, `CB2_FRAMELOG` (speed, see [PERFORMANCE.md](PERFORMANCE.md)), `CB2_SMOOTHLOG` (60 fps smoothing),
`CB2_RUNLOG` (level timer), `CB2_NOIDLESKIP`, `CB2_NOPRECISE`, `CB2_SOFTRASTER`, `CB2_DISCSPEED` (disc speed-up, overrides `disc_speed`), `CB2_WAV` (record the sound), `CB2_HWSUMMARY` (at exit: which CD and SPU features the game really used - XA, CD-DA, reverb, noise, pitch modulation).

## Other tools

```
build\Release\cb2_viewer.exe <folder-with-S0-S3 | one .NSF> [--level N] [--screenshot out.png]   # scenery viewer
build\Release\cb2_boot.exe   <folder> [instr] [--frames N] [--press 2310-2314:start] [--shot 2400:f.ppm] [--wav f.wav]
build\Release\cb2_check.exe  <folder>      # parses every NSF, exits non-zero on any failure
build\Release\cb2_roundtrip.exe <folder>   # NSF/NSD writer round trip, exits non-zero on any mismatch
build\Release\cb2_gtetest.exe analysis\gte\tests.bin    # GTE vs real-hardware captures (tools\gte_tests_convert.py)
build\Release\cb2_memtest.exe              # memory card filesystem self-test
build\Release\cb2_vmdiff.exe <SCUS_941.54> [cases] [seed]   # GOOL VM vs the game's own interpreter
build\Release\cb2_mod_demo.exe <level.NSF> <out folder>     # example mod (see MODDING.md)
build\Release\cb2_gool.exe  list|dump|patch|test ...        # GOOL assembler / script patcher (EDITING.md)
build\Release\cb2_scene.exe list|export|import|test ...     # scenery <-> OBJ editor pipeline (EDITING.md)
build\Release\cb2_models.exe <level.NSF>                 # models and animations in a level
build\Release\cb2_entities.exe <level.NSF>               # zones, spawn points and entities of a level
build\Release\cb2_crashtrace.exe <folder> --press ... --follow <object> --from F --to F   # records an object of the running game frame by frame
build\Release\cb2_scan.exe / cb2_cardfind.exe / cb2_discbench.exe                        # finding game variables, memory-card data, disc read speed
```

`cb2_boot` is the headless harness: it runs the game for N instructions/frames, applies scripted pad input, saves frames and audio, and reports which BIOS
calls and hardware registers the game used. Viewer controls: hold right mouse to look, `WASD` move, `Q`/`E` down/up, `Shift` fast, `Tab` / `Shift+Tab`
next/previous level, `F` reframe, `Esc` quit.

## How the engine layer is built

| Part | State |
| --- | --- |
| Platform layer | The game boots, plays the intro and reaches gameplay with controls, audio and rendering. Details in [PLATFORM.md](PLATFORM.md), [ARCHITECTURE.md](ARCHITECTURE.md) |
| R3000 + GTE | R3000 verified against Unicorn; the GTE passes all 1,150 captures taken from real hardware (22 commands) |
| GPU | Software rasteriser into a 1024x512 VRAM keeps the console's exact VRAM; an OpenGL backend replays the same draw commands at up to 8x resolution with unrounded GTE vertex positions |
| SPU | ADPCM voices, ADSR, loops, key on/off, ENDX, RAM transfers, and the reverb (cb2_spureverbtest; the original formula at 22.05 kHz, without the hardware's input/output FIR filters). Crash 2 itself never enables reverb, noise or pitch modulation in any scene tested (CB2_HWSUMMARY), so these cannot be compared against the game's real sound; noise and pitch modulation are not implemented |
| CD-ROM | Controller registers, ReadN/Setloc/GetID over an ISO9660 volume synthesised from the loose files (with mod overlay) |
| Memory card | BIOS card API and file API on a native-layout card image |
| NSF format + C++ parser | 39 files, 2,762 chunks, 696 scenery worlds parse with no errors ([FORMAT.md](FORMAT.md)) |
| GOOL VM | Reference VM covering all 79 opcodes, verified against the game's own interpreter on 38,000 random programs with 0 mismatches ([GOOL.md](GOOL.md)) |
| Modding | NSF/NSD writer verified byte-exact on all levels; modded NSFs load into the running game through `--mod`; GOOL assembler (453 scripts round-trip) and scenery-to-OBJ editor (696 worlds round-trip). Not written: new native code, textures/collision/entity/model authoring |

Not done yet: a GOOL-to-C++ decompiler, pixel-exact GPU comparison against hardware (needs reference captures). The game uses no XA or CD-DA audio (no Setfilter, no CD-audio mode; the music is SPU samples), so none is implemented. CB2_WAV=file.wav records the SPU output; CB2_NOREVERB=1 / everb = off leave the reverb out.

`tools/nsf_probe.py` is the Python prototype used to validate the format before the C++ port.

## Documentation index

[ONLINE.md](ONLINE.md) (how the multiplayer works inside the game) · [LOBBY_PROTOCOL.md](LOBBY_PROTOCOL.md) · [SPEEDRUN.md](SPEEDRUN.md) (timer, records, leaderboard plans) ·
[PERFORMANCE.md](PERFORMANCE.md) (60 fps, textures, pacing, emulation speed) · [RELEASING.md](RELEASING.md) · [PLATFORM.md](PLATFORM.md) · [ARCHITECTURE.md](ARCHITECTURE.md) ·
[FORMAT.md](FORMAT.md) · [GOOL.md](GOOL.md) · [MODDING.md](MODDING.md) · [EDITING.md](EDITING.md)
