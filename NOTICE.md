# Notice

## Game data and trademarks

This project is an independent, unofficial hobby project and is not affiliated with or endorsed by Activision, Sony Interactive
Entertainment, Naughty Dog or any other rights holder. "Crash Bandicoot" and related names and characters are trademarks of their owners.

**No game data is included or distributed here.** The program reads the files of your own legally obtained copy of the game
(`SCUS_941.54`, `SYSTEM.CNF` and the `S0`..`S3` folders) at run time and never copies them into the project. Do not commit, upload or share
those files, or an executable folder that contains them (`.gitignore` excludes `dist/`, `data/` and memory cards for this reason).

## Third-party components

| Component | Where | Licence |
|---|---|---|
| GLFW (fetched by CMake) | build | zlib/libpng |
| GLM (fetched by CMake) | build | MIT |
| glad (OpenGL loader) | `third_party/glad` | public domain / MIT (generated code) |
| stb_image_write, stb_truetype | `third_party/` | public domain (or MIT) |
| miniaudio | `third_party/miniaudio.h` | public domain / MIT-0 |
| Crash-a-Like font by Aryel Filipe | `assets/fonts/`, `src/crash_font.inc` | listed on dafont.com as "100% Free"; see https://www.dafont.com/crash-a-like.font |

File-format knowledge comes from the community: the CrashEdit project (https://github.com/cbhacks/CrashEdit) and its wiki
(https://wiki.cbhacks.com). Thank you.

## Contributing

Pull requests are welcome. Never include game files, memory cards, built executables or the address of a private service in a contribution.
