# Online play

Several players walk around the same level of the original game. Each player's PC runs its own copy of the game (the original
code on the emulated PlayStation); the other players show up as extra Crashes, drawn by the game itself with the real model,
animations, lighting and shadow, and the player's name floats over the head. It is the same idea as the CTR online mod.

**There is no host.** Everybody who types the same *room* name meets, and every player sends their state to every other player
directly (a mesh). Nobody has to type an address, nobody's departure ends the game for the rest, and a player who quits (or crashes)
can come back at any time and is simply picked up again. A rendezvous server is only needed to find each other across the internet.

## The lobby

`Crash2.exe` opens an online lobby before the game starts (the PlayStation game itself only begins after you leave it):

- **Play offline** - straight to the game's own title screen and New Game / Load Game.
- **Play online** - opens the **room list**: every room that exists, with its number of players, most populated first (it refreshes by
  itself every 2 seconds). Pick one with Up/Down and Enter, or choose *+ Create a room* at the top and name it (a random name is
  suggested). The list contains the rooms the rendezvous server knows about and the rooms of players on this PC / network. Then the
  **waiting room** shows who is in it (name and level) and whether the server answers; Enter starts the game, Esc goes back to the list.
  Everybody then picks New Game or Load Game as usual and meets in the same level.
- **Name** - shown over your Crash (default: your Windows user name).
- **Server** - the rendezvous server (see below). Empty = only players on this PC or local network.
- Name and Server are edited in place (Enter, type, Enter) and remembered in `cb2.cfg` (`name`, `room`, `rendezvous`; the last room
  you entered is remembered as `room`). Keys: Up/Down, Enter, Esc, Backspace; a game pad works too (D-pad, A, B).
- The menu is drawn in the *Crash-a-Like* font by Aryel Filipe (dafont.com, "100% free"), embedded in the executable
  (`assets/fonts/`, `tools/embed_font.py`); the stone-and-planks look follows the warp room's palette.

`lobby = off` in `cb2.cfg`, `--no-lobby`, `Play offline (no lobby).bat`, or any of the command-line options below skip the lobby.

Inside the game, `Esc` opens a pause menu (keys, volume, picture, name tags, fullscreen) whose **Main menu** entry leaves the room
and returns to this lobby (it starts a fresh copy of the game with `--lobby`). While it is open the others see your Crash stand
still; the network keeps running so nobody drops you. Hide the name tags there if you prefer.

## The online lobby service (optional)

**Play online** lists every open room and dedicated server from everywhere, and joining needs no typing and no port forwarding. That
comes from a small HTTP lobby service whose address is built into the executable (`local/directory.txt`, git-ignored, read by CMake; see
`CMakeLists.txt`). The service is **not part of this repository**: the maintainer runs a private one. A build without the file has no
online list: **Play local** (this PC / local network) and the relay server below still work. If you want your own lobby service, the
protocol is in `docs/LOBBY_PROTOCOL.md`; put your service's address in your own `local/directory.txt` and rebuild.

Dedicated servers list themselves: a `Crash2 Server.exe` built from this source tree with `local/directory.txt` announces itself to that service **by default**
(`--no-announce` keeps it private; `--announce <service address>` and `--name "My server"` override; on Linux, `DIRECTORY=<service address> NAME="My server" sudo -E bash server/install.sh`).
The server then shows up in everybody's lobby list within a minute, **provided its UDP port (7777 unless you chose another) is open to the internet**: an ingress rule on a cloud VM,
or a port forward on a home router (`--public-port` if the outside port differs). A server announces itself only; it needs no game files and serves both releases. Players never need to open ports: both sides send to each other at the same moment, which opens the path
through their routers (hole punching, with the public address learned from STUN). Pairs that still cannot connect need a relay: a
`Crash2 Server.exe` whose address is built in as `local/rendezvous.txt` (or `rendezvous =` in `cb2.cfg`) forwards their packets.

## How players find each other

1. **Same PC or same network: automatic.** Each copy listens on the first free UDP port from 7777 to 7786 and probes those ports on this
   PC and a broadcast to the local network, once a second. Two copies in the same room find each other with no setup at all.
2. **Across the internet: the rendezvous server** (`Crash2 Server.exe`, `src/server.cpp`; below). Each player tells it their room, and it
   answers with the other members' public and private addresses. The players then send packets to each other's addresses at the
   same moment, which opens a path through both NATs ("hole punching"); players on the same network use the private address.
3. **When no direct path exists** (some mobile and carrier networks, strict NATs) the server forwards that pair's packets (relay) after
   about 3 seconds. This costs the server a little traffic: it is about 70 bytes x 30 per second per player and receiver.

Once two players are connected the server is not needed: if it goes away they keep seeing each other (tested). New players cannot be
introduced until it is back, and a relayed pair loses its path.

## Rooms with a password

*Create a room* asks for a name and then an optional password. A locked room shows a padlock in the list and asks for the password when you join. Wrong or missing
password: the lobby returns with **WRONG PASSWORD** (or, if you were creating the room and the name is taken on the server with another password, a hint to pick another).

What happens on the wire (`src/netproto.hpp`): the password never travels. Together with the room name (the salt) it becomes a 64-bit key through 20000 rounds of SHA-256 (so every
guess of a password is costly). Every state packet of a locked room carries a 32-bit tag, the SHA-256 of the key, the room, the sender and the **contents of the state**: a stranger
cannot make a packet that is accepted, and a captured tag is useless with other contents. Players drop packets with a wrong tag (and answer "denied", at most once every 2 s per address).
The rendezvous server is given a SHA-256 image of the key (the *verifier*) by whoever opens the room and only lets players in whose verifier matches; the key cannot be computed from it, and the tag
rides along in relayed packets. The lobby directory is only told *that* a room is locked, for the padlock.
This keeps unwanted players out of a room. It is **not encryption**: somebody who can read your network traffic can read your Crash's position and can replay an exact copy of a packet they captured,
and a short password can be guessed offline by anybody who captured a verifier or a tag. Use a password you do not use anywhere else. A server can also be occupied by a stranger who registers a room
name before you do (with another password): pick another name, or run your own server.

## Security notes

What the networking code does about hostile input (reviewed in v0.8.1; see `src/net.cpp`, `src/server.cpp`, `worker/src/directory.js`):

- **Decoders** check exact packet sizes and counts, names are cleaned; nothing from the network indexes memory unchecked. The state of other players is only ever written into the emulated console's
  memory, never the host's.
- **Forged players.** A player is tied to the address it first spoke from, so a packet that only uses somebody's id (ids are public) cannot steal them or redirect our packets; at most 32 players are
  accepted, so a flood of made-up ids cannot freeze the game.
- **Aiming our traffic at somebody (reflection).** The server drops the "private address" a registrant claims unless it really is a private address, and players only send to a private address on
  their own network and to public addresses that are plausible (never 0.x, multicast, or loopback unless the server is on this PC). The server answers an address at most 24 times a second and refuses
  a registration for the id of a living member from another address.
- **Directory.** Servers are listed oldest first (the player count they claim is not trusted), a caller (an IPv6 /64 counts as one) may have 16 rooms and 2 servers, the per-caller rate limit applies per /64.
  The address a client reports is only used for callers that reach the directory over IPv6; it cannot be verified there, which limits what a malicious caller can do to the few packets a join generates.
- **Relay server choice.** With no `rendezvous` set, the game relays through the directory's longest-listed server (this is what lets players on phone hotspots and other strict networks connect). The server
  sees the players' addresses and relayed positions, like the directory does; set `rendezvous =` to a server you trust to avoid it.
- **Updates** come over HTTPS only and are checked against the SHA-256 GitHub lists for the release; there is no code signature, so a compromised release account is a compromised update.

## The two releases

Players of the European (PAL) and the American (NTSC-U) release never share a room: the two games have different code and data, so the extra Crash of one cannot be put into the
other. Every packet carries a one-byte release number and ignores the other release's packets; the rendezvous server keeps one list of rooms per release; the lobby directory
takes a `region` field (`u` or `p`) and lists only the rooms of the asker's release. The release is the one the game is running (`region` in `cb2.cfg`). Addresses inside the
game that the multiplayer module uses are per release: `src/game_profile.cpp`, [REGIONS.md](REGIONS.md).

## Rendezvous server (for example on an Oracle Cloud VM)

`cb2_server [port]` (built from `src/server.cpp`; Windows and Linux, no dependencies) only keeps a list of who is in which room right now
(members that are silent for 6 s disappear, empty rooms with them) and forwards relayed packets. It stores nothing and needs no game
files. It also answers the lobby's "which rooms exist?" question (the request is padded to the size of the answer, so it cannot be used to
flood somebody else). Limits: 16 players per room, 2000 rooms, 16 players per public IP.

On the VM (Ubuntu), from your PC:

```
ssh ubuntu@VM 'mkdir -p ~/cb2/src ~/cb2/server'
scp src/server.cpp src/netproto.hpp ubuntu@VM:~/cb2/src/
scp server/install.sh server/cb2-server.service ubuntu@VM:~/cb2/server/
ssh ubuntu@VM 'cd ~/cb2 && sudo bash server/install.sh'
```

`install.sh` compiles the server, installs it as the systemd service `cb2-server` (starts at boot, restarts if it ever dies, runs as an
unprivileged user) and opens the UDP port in the VM's firewall. Oracle also has its own firewall in front of the VM: in the console add
an **ingress rule** to the VCN's security list (or network security group), source `0.0.0.0/0`, protocol **UDP**, destination port
**7777**. Check with `systemctl status cb2-server` and `journalctl -u cb2-server -f`. Then every player puts the VM's public IP (or name)
in the lobby's *Server* line, or in `cb2.cfg`:

```
rendezvous = 203.0.113.10
room = MYGANG
```

`Crash2 Server.exe` and `Start rendezvous server.bat` in the game folder run the same server on a Windows PC.

## Command line

```
Crash2.exe --online [--room NAME] [--rendezvous host[:port]] [--name Bob]     straight into the room, without the lobby
Crash2.exe --fake-player [1|2]                                                a test ghost that copies your Crash (2: it comes and goes)
```

`--room` and `--rendezvous` also imply `--online`. Online play uses the interpreter (not `--native`) because it hooks the game's own
routines. `CB2_MPDEBUG=1` prints what the module does. Tests: `tools\net_test.ps1` (network: LAN discovery, server, relay, leave and
return, server going away; add the address of a Linux server with `cb2_nettest host:port` to test that one) and `tools\mp_test.ps1`
(two game windows in one room, skipping the cutscenes and walking into the Turtle Woods portal; `-Server host:port` goes through a
server).

## Getting in

Choose New Game in each window. The game opens with Cortex's intro cutscene; press Start to skip it, and press Start again to skip the
second cutscene in the warp room. Then you control Crash in the warp room, and other players there show up with their names. Walk
into a portal to enter a level (Turtle Woods is straight ahead); each player enters on their own, and you see each other as soon as
you are in the same level again. Both cutscenes also work if you let them play, but the extra Crash only appears once you
control Crash.

## How it works (`src/multiplayer.*`, `src/net.*`)

- **Which Crash is yours.** (Addresses in this section are the American release's; the European ones are in `src/game_profile.cpp`.) The game keeps a pointer to the current player object at `0x8006CB10`. An older Crash (the cutscene's) stays
  intact in memory after the engine drops it, so the module follows that pointer instead of scanning memory.
- **Local state.** When the game updates Crash's object (hook on `0x8001C718`) the module reads position, rotation, scale,
  animation (frame group relative to the object's entry, frame number) and sends it, 30 times a second, over UDP to every
  other player in the room (`net.cpp`, Winsock; packet format in `netproto.hpp`). A player says goodbye when leaving; one that is
  silent for 10 s (a crash, a cable; long enough that a slow level load on someone's PC does not make everyone delete and re-create
  their Crash) disappears anyway. While you are in a menu or a loading screen you still announce your name, so
  the others do not keep drawing your old position. The state packet also carries the run data of [SPEEDRUN.md](SPEEDRUN.md)
  (protocol version 4: older clients do not see this one).
- **Loading safety.** Creating or deleting a puppet while the game streams a level from the disc is postponed until the disc is idle
  (at most 5 s), every engine call the module makes has an instruction budget, and a call that fails is rolled back and retried later, so
  a problem with a puppet can no longer take the game down.
- **Remote Crashes.** For every remote player in the same level the module calls the game's own object-creation routine
  (`0x80019800`, type 0 / subtype 1 gives a second `Will.C` object) and hangs it from a list the object loop always walks. The
  object's script never runs (calls into the script interpreter, `0x8003A014`, for these objects are answered with "carry on"; a
  script left running eventually executes a `return` and the engine deletes the object, which made the extra Crash blink out
  after a few seconds), and before each update the module writes the received position (smoothed between
  packets), rotation, animation and frame into the object's fields. The game's update then draws it like any other object.
  Objects hold field `n` at `object + 0x40 + 4n` (`src/gool_fields.inc`).
- **Cutscenes and level changes.** The engine rebuilds its tables when a cutscene or level starts, which can leave an extra
  object with an unusable record (`object + 8`). The module only updates an extra Crash whose record is sound (otherwise it is
  skipped, and replaced after a while), refuses poses that are not plain animations, and keeps the frame number inside the
  animation, so that a cutscene pose from the other player cannot crash the guest. Objects the module has removed are left alone
  by the object loop.
- **Name tags.** The GTE projects every vertex the game draws; those are recorded (`Cpu::gteSamples`). Each extra Crash is drawn
  at a scale a fraction of a percent different from the others, which identifies its polygons on screen (every object's model
  matrix is the camera matrix times its own, so the object's column lengths survive). The tag is placed above the top of that
  group and drawn by the OpenGL renderer as an overlay (`Gpu::queueTag`, `src/font5x7.hpp`). Tags appear while the extra Crash is
  on screen.

## Security and limits

- There is no authentication or encryption: a room is as private as its name, so pick a name nobody guesses. The server checks that
  relayed packets come from a registered member's own address and rate-limits registrations, but it is a hobby server.
- Windows Firewall may ask once to allow the program. A VPN such as Tailscale or ZeroTier also works: put the VPN address in `Server`
  (or leave it empty, since players on the same virtual LAN are found by broadcast only if the VPN forwards broadcasts).
- Everybody in a room meets everybody (a full mesh), which is fine for a few players; the server caps a room at 16.

## Not there yet

The other Crashes are only shown: they do not collide with you, hit you or open boxes, and boxes and enemies are not shared (each
machine runs its own world). Real interaction needs a shared world state (boxes, enemies, collectibles); with no host that would need an owner chosen among the players.
It is also only tested with copies on one PC (two game windows, and the network layer with a Linux server in WSL), not over the internet
between two machines or through a real NAT, and the extra Crash is not drawn during cutscenes. Collisions with the extra Crash would be the next step: its object is
a real game object, so it can be given a hit box or a real pad (the game reads only pad 0, so that needs the pad swapped).
