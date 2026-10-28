# Skipping the intro

A new game of Crash Bandicoot 2 starts with the logos, the title, a long story cutscene and then the first visit to the warp room. **Settings > Skip the intro** (on by default; `skip_intro = off` in `cb2.cfg`, or `CB2_NOSKIP=1`) starts a new game **in the warp room**.

## How it works: the boot level, not a save state

The game's `main()` ends with a call that opens its first level, and the number it passes is a constant in the program: `ori a0, zero, 0x3C` at `0x800117E4` (the same address in both releases), the level of the logos and the title. A modified executable
(the `SCUS_941.54` / `SCES_009.67` file of the disc) that has `0x02` there starts a new game in level 2, the warp room: the test ISO `c2_custom_models_test` that was supplied for this work differs from the retail PAL executable in exactly that one byte, and starts the same way.
The program makes the same change **in memory**, when the game is about to run that instruction (a hook one instruction earlier), so no file of the disc, the game folder or the memory card is touched. The profile of each release keeps the address (`bootLevelInsn` in `game_profile.hpp`).

There is no save state, no waiting for a first run, and nothing is stored on disk.

## What you see: the warp room's own scene

The warp room is opened the way the game opens it for a new game, and its first scene plays: Dr. Neo Cortex talks to Crash and Crash is dropped into the room (about 35 seconds, as in the game; the pad's Start only pauses it). When Crash can be controlled, you are in the warp room.
`Tab` runs the scene at full speed.

The scene is played by the game's own scripts, which wait for it to end. Leaving it out is not possible without leaving Crash's camera in the scene (tried: skipping the creation of the scene's objects removes Cortex, but the camera of the scene never ends). Starting the warp room from the
"returning from a level" spawn point, which has no scene, is the way to get rid of it; that spawn is chosen by game variables that have not been found yet (see the end).

## When it does not happen

* The memory card already has a Crash 2 save: you have the title screen's *Continue*, which is where you want to be, and a game that did not go through it must not write a new save over yours.
* A save state, a scripted press, `--autostart` or `--native` decide where the game starts.
* You switch it off in the menu (from the next start) or in `cb2.cfg`.

`CB2_FORCESKIP=1` makes a scripted run (a capture or a hidden window) skip the intro too; that is how it is tested.

## Not done yet

* The warp room's opening scene is still played. The way out is to find the variable the game uses to choose the spawn point of the warp room (the NSD of level 2 has 32 spawn points; the first is the new game's, the second is the usual entrance of the room), and set it with the same hook.
