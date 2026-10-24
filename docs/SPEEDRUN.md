# Level times, records and the player list

CB2-Online times every run through a level of the real game, keeps your personal records in a text file, shows the times of the other
players in the room, and is built so that a leaderboard can be added later without changing the game.

## What is timed

- The clock starts the first frame **Crash can move** after a level has loaded (not while the level loads), and is shown in the top right
  corner (`F4` hides it, or *Sound and picture → Level timer*). It counts the game's own time at 59.94 frames per second, so the menu pausing
  the game stops it, and a slow PC does not change the result.
- A run ends when the game asks for a different level:
  - it asks for **the place you came from** (the warp room): the level is **finished**, the time is recorded;
  - it asks for the **Continue? screen** (level 0x3B, you ran out of lives): the run **failed**; it counts as an attempt, never as a record.
- Dying with lives left is a restart of the same level, not a new one: the clock keeps running (as in a full-game run).
- Time spent in the warp room is not timed; walking into a portal starts the next level's run.

Only the level numbers Snow Go, Crash Dash, Turtle Woods and the warp room have names so far; the others show as `LEVEL xx`.

**Honest status.** The "game asks for level N" signal is the game's own instruction (address `0x80037DC8`); it fires when you walk into a portal
(checked) and the hub is what the end of a level asks for (not yet checked on a real level end: please report if a finished level is not
counted, with a save state from just before the exit, `F5`).

## Records: `records.txt`

Next to `Crash2.exe`, plain text, one line per level, tab separated:

```
R <level, hex> <best ms> <finished runs> <attempts> <last ms> <date of the best>
R 1E            80100     3              4          80100      2026-10-26
```

It is written after every run (to a temporary name first, so a crash cannot leave a half file). *Esc → Records* shows it in the game. The file
survives updates; copy it to keep your times. A damaged line is skipped without losing the others.

## What the others see

Each player's state packet (protocol version 2, `src/netproto.hpp`) carries their **personal best for the level they are in**, and their
**latest run** (level, time, finished or not) with a counter that goes up with every run. In a room:

- the **player list** (right edge, `F3`) shows for each player the level, the time since they entered it, `PB m:ss.t` (their best there) and
  `DONE <level> <time>` (their latest finished run);
- a notice appears when somebody finishes a level (`ALICE FINISHED TURTLE WOODS 0:11.9`) or runs out of lives;
- runs made before you met are not announced.

The numbers come from the other player's own game, so they can be edited: they are for fun, not proof. A leaderboard that means something needs
the server to be able to check a run (see below).

## Leaderboards later

What a service needs is already there: a **player name**, a **level**, a **time in ms** and a **date**, in a format that is one line per
record. Two ways to go, from simple to strict:

1. **Trust-based board** (like the room directory): the client POSTs `{name, level, ms, version}` to the Cloudflare Worker after a finished run;
   the Worker keeps the best per name and level and lists the top N. Anyone can fake a time, but for friends that is enough.
2. **Verifiable board**: record the controller input of the run (the console is deterministic: `cb2_statetest` proves that the same input from
   the same state gives the same machine), upload `state at the start + inputs`, and let the server (or other players) replay it with `cb2_play`
   and check the time. This is the same technique as a speedrun replay file; the pieces that exist today are the save state format and the
   determinism test.

Nothing is uploaded today.
