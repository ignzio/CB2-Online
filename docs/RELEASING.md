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

## Code signing

Windows Defender SmartScreen and some virus scanners warn about, or block, programs that are **not signed** and that nobody has run before; a new open-source game that talks to the
network and replaces its own `.exe` when it updates looks exactly like what they are cautious about. A signature makes the publisher visible and lets Windows build a reputation for it;
it does not make the warning disappear on day one (that needs downloads), and Microsoft no longer gives even EV certificates an instant pass. Signing is built into the release script;
what is needed is a certificate **issued to you after an identity check** (nobody can create one for you):

| Option | Cost | Notes |
| --- | --- | --- |
| **Azure Artifact Signing** (formerly Trusted Signing) | about 10 USD a month plus an Azure subscription | no key to keep: Microsoft holds it; individuals only in the USA and Canada, organisations in more countries; identity validation takes days |
| **SignPath Foundation** | free | for public open-source projects only (the repository would have to be public, with an OSI licence; apply at signpath.org/foundation) |
| A certificate authority (Sectigo, DigiCert, SSL.com ...) | about 100-400 USD a year | OV or EV; since 2023 the key must be on a hardware token or in a cloud HSM |

Once you have one, set **one** of these in the environment before running `tools\make_release.ps1` (nothing secret goes into the repository):

```
CB2_SIGN_PFX=C:\path\cert.pfx   CB2_SIGN_PFX_PASSWORD=...          a certificate file
CB2_SIGN_THUMBPRINT=<sha1>                                         a certificate in the Windows store (hardware token, EV)
CB2_SIGN_AZURE_DLIB=<...>\Azure.CodeSigning.Dlib.dll  CB2_SIGN_AZURE_METADATA=<...>\metadata.json     Azure Artifact Signing
```

The script signs `Crash2.exe` and `Crash2-Server.exe` (SHA-256, timestamped, so the signature stays valid after the certificate expires), checks the signature, and only then builds the zip and uploads;
the updater's SHA-256 check is made against the signed files. `-RequireSign` stops the release when nothing is configured; without it the script only warns. Do not sign or append data to
the exe afterwards (`tools\make_exe.ps1` appends game data to the exe for personal single-file builds: that breaks a signature, and such builds must never be published).

Both programs carry version information (company, description, version from `VERSION`, licence, source address; `assets/version.rc.in`), which also helps with scanners.

**While the programs are unsigned**, for a player: SmartScreen's "Windows protected your PC" has *More info -> Run anyway*; a Defender detection can be checked by comparing the file's SHA-256
(`Get-FileHash Crash2.exe`) with the value GitHub shows under the release asset, and the file can be restored from quarantine and excluded (Windows Security -> Virus & threat protection -> Manage settings ->
Exclusions). For you as the publisher: submit the file as a *false positive* to Microsoft (https://www.microsoft.com/wdsi/filesubmission, "Software developer", with the detection name that Defender
shows); the verdict is usually updated within days, and each new release has to be submitted again until it is signed.

## Making a release

1. Bump `VERSION` (for example `0.2.0`), commit, push to `main`.
1a. (Recommended) Have a code-signing certificate configured, see *Code signing* above.
2. Build from a tree that has `local/directory.txt` (the lobby service address is baked into the exe from it):
   `cmake -S . -B build; cmake --build build --config Release --target cb2_game cb2_server`
3. `powershell -File tools\make_release.ps1 -Publish -Notes "what changed"`

The script creates the tag `v<VERSION>` on `main`, and uploads:

| Asset | Used for |
| --- | --- |
| `CB2-Online-v<VERSION>.zip` | first install: the game without data; the game asks for the player's own disc image on first start and fills `data\` |
| `Crash2.exe` | what the updater downloads; replaces the running executable, whatever it is called |
| `Crash2-Server.exe` | replaces `Crash2 Server.exe` when the player has one |

Players on different versions can not play together if the network protocol changed (`src/netproto.hpp`): bump `VERSION`
when you change it, so everybody is offered the update.

## Testing the updater

`CB2_FAKE_VERSION=0.0.1` makes a build pretend to be that version, so a copy of the current release offers the update.
