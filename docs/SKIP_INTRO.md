# Skipping the intro

A new game of Crash Bandicoot 2 starts with a long intro: the logos, the title, the story cutscene and Crash walking into the warp room. **Settings > Skip the intro** (on by default; `skip_intro = off` in `cb2.cfg`) starts a new game in the warp room instead. This is what makes a multiplayer room start together in the warp room, and a speedrun practice start at once.

## How it works

The game is not changed. The first time (per release: the European and the American game each have their own):

1. The program runs the game unthrottled from the boot, pressing Start now and then, on a **blank memory card kept in memory** (your real card is not read or written meanwhile, and its image is put back afterwards), until Crash stands in the warp room of a new game. It takes 20 to 60 seconds, and the window title says so.
2. It saves the whole console there as a save state: `states\warproom.cb2state` (`states\pal-warproom.cb2state` for the European release).

Every later start loads that state (about two seconds). Your saved games are not touched: save states do not contain the memory card.

## When it does not happen

* The memory card already has a Crash 2 save: you have the title screen's Continue, which is where you want to be.
* A save state, a scripted press, `--autostart` or `--native` decide where the game starts.
* You switch it off in the menu or in `cb2.cfg` (from the next start), or set `CB2_NOSKIP=1`.
* The saved warp room cannot be loaded (a state of another version of the program): it is deleted and made again.

If the game does not reach the warp room within 40,000 frames, it gives up, puts your card back and plays the intro as usual; `cb2-session.log` says why.

`CB2_FORCESKIP=1` makes a scripted run (a capture or a hidden window) skip the intro too; that is how it is tested.
