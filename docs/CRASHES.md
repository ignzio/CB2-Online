# When the game crashes: logs, dumps and how to read them

The game keeps records so that a crash that happens "all of a sudden" can be understood, and so that players can send a useful report. Everything is written next to `Crash2.exe`.

| File | What it is | When |
| --- | --- | --- |
| `cb2-session.log` | The events of this session, one line each, written at once (it survives a killed process): version, system, graphics card, game folder, the phase (lobby / playing / menu), every level, other players joining or leaving, state loads, the freeze watchdog, GLFW errors, out-of-memory, and a line every 30 s with the memory use, handles and GDI objects (a leak shows as numbers that only grow). At most 1 MB. Anything the game prints on `stderr` goes into it too. | always |
| `cb2-session-previous.log` | The session before this one. | always |
| `crash.log` | The report of a crash: what failed and where, the game's state (frame, level, guest pc, settings, graphics), the call stack as `Crash2.exe+0x...`, registers, memory, **the loaded modules** (an antivirus or overlay that injected itself shows here), and the last events. | an exception, `abort`, an uncaught C++ exception, a stack overflow |
| `crash.dmp` | A minidump of the crashed process (open it in Visual Studio or WinDbg). About 1 MB. | with `crash.log` |
| `hang.dmp` + a stack in the session log | The game stopped responding: the main loop did not run for 20 s. The log says where the main thread was (it can also be a window being dragged or a dialog: the line says so). | a hang |
| `crash-last.log`, `crash-last-session.log`, `crash-last.dmp`, `hang-last.dmp` | The files of the last failed session, kept by the next start so that they are not overwritten. | the start after a failure |

**A crash that leaves no `crash.log`:** a fast-fail (heap corruption detected by the runtime), the process being killed (Task Manager, a security program, a driver reset, the system) leaves no handler time to write anything.
The next start still notices (a lock file `running-<pid>.lock` was left behind) and offers the report with the **last known state and the last events** from `cb2-session.log`. If the log ends with a normal line and no `session ended cleanly`,
something outside the game ended the process; the module list and the graphics card at the top of the log are the first things to look at.

## For players: sending a report

The next start after a crash asks *"The game closed unexpectedly last time. Open a bug report?"*. Say yes: the browser opens an issue with the version, system, graphics card, settings and the last events filled in, and Explorer opens the game folder with
the crash files. Zip `crash-last.log`, `crash-last-session.log` and `crash-last.dmp` (or `hang-last.dmp`) and drag the zip into the issue. Nothing is sent until you press *Submit new issue*. Esc > *Report a bug* does the same at any time.

## For developers: reading a report

Each release has a **link map** of the exact build: `Crash2-v<version>.map` is attached to the release (and `tools\make_release.ps1` keeps a copy in `dist\symbols\v<version>\Crash2.map`). It turns the stack addresses into functions:

```
python tools\resolve_crash.py crash-last.log [Crash2-v0.8.4.map]
  Crash2.exe+0x1ed36   <- main +0xc286
```

Without the map argument the script looks for `dist\symbols\v<version>\Crash2.map` (the version is read from the report), then `build\Release\cb2_game.map`. C++ names are demangled with `undname.exe` when Visual Studio is installed. The map names functions
and offsets, which is enough to find the line; for locals and arguments open `crash-last.dmp` in Visual Studio with a PDB of the same build (build with `/Zi` and `/DEBUG` if you need one).

The module list is worth reading before the stack: injected DLLs of security programs and overlays (Kaspersky, Discord, MSI Afterburner, RTSS, Steam overlay, GPU recording tools) are a common cause of crashes inside the graphics driver
(the stack then ends in `nvoglv64.dll`, `atio6axx.dll` or `ig*.dll`: a driver problem; update it).

## Trying it

`CB2_TESTCRASH=<frame>[:kind]` crashes a visible game on purpose: `null` (default: a write through a null pointer), `throw`, `abort`, `fastfail`, `exit` (the process ends silently), `hang` (the main loop stops), `oom`.
`tools` has no test runner for it; start `Crash2.exe --no-lobby` with the variable set and look at the files, then start it again to see the report question.
