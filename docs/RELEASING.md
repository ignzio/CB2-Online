# Releases and the in-game updater

The game updates itself from GitHub Releases. There is no update server: the client asks
`https://api.github.com/repos/ignzio/CB2-Online/releases/latest`, compares the tag with the version built into the
executable (the `VERSION` file) and, when the release is newer, the lobby shows a green **UPDATE TO Vx.y.z** row.

## What the player sees

1. The lobby opens, the check runs in the background (one request per launch).
2. If a newer release exists the first row of the lobby is *UPDATE TO V0.2.0*. Nothing happens until it is chosen.
3. Enter downloads the new `Crash2.exe` (and `Crash2 Server.exe` if the player has one next to it) with a percentage,
   then the game restarts into the lobby with the new version. Game data, `cb2.cfg` and saves are not touched.
4. If anything fails (no network, wrong checksum, folder not writable) the old version stays in place and the lobby shows why.

`update = off` in `cb2.cfg` disables the check.

## Safety

- The download URL must start with `https://github.com/ignzio/CB2-Online/releases/download/`; redirects to plain http are refused.
- Each file is checked against the SHA-256 `digest` GitHub publishes for the release asset (and must start with `MZ`).
  A release asset without a digest is not offered.
- The old executable is renamed to `Crash2.exe.old` (Windows allows renaming a running exe), the new one takes its place, and
  the old one is deleted at the next start. If the swap fails half way, the files are put back.

## Making a release

1. Bump `VERSION` (for example `0.2.0`), commit, push to `main`.
2. Build from a tree that has `local/directory.txt` (the lobby service address is baked into the exe from it):
   `cmake -S . -B build; cmake --build build --config Release --target cb2_game cb2_server`
3. `powershell -File tools\make_release.ps1 -Publish -Notes "what changed"`

The script creates the tag `v<VERSION>` on `main`, and uploads:

| Asset | Used for |
| --- | --- |
| `CB2-Online-v<VERSION>.zip` | first install: the game without data; the player copies their own game files into `data\` |
| `Crash2.exe` | what the updater downloads; replaces the running executable, whatever it is called |
| `Crash2-Server.exe` | replaces `Crash2 Server.exe` when the player has one |

Players on different versions can not play together if the network protocol changed (`src/netproto.hpp`): bump `VERSION`
when you change it, so everybody is offered the update.

## Testing the updater

`CB2_FAKE_VERSION=0.0.1` makes a build pretend to be that version, so a copy of the current release offers the update.
