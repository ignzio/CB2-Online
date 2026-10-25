# Speed, smoothness and 60 fps

## 60 fps smoothing (*Esc → Sound and picture → 60 fps smoothing*, `smooth = on` in `cb2.cfg`)

Crash Bandicoot 2 draws a new picture every second vertical blank, about 30 per second. With smoothing on, the picture in between is drawn too, so
the screen changes 60 times a second.

How it works (`src/interp.cpp`): the host renderer replays what the game told the GPU to draw. The recorded commands of a frame are kept, and when the
game flips to a new frame the previous and the new frame are compared: every triangle of the new one is looked up in the old one (same texture,
texel coordinates, palette and colour - a polygon keeps those from frame to frame) and drawn half way between the two screen positions. The result is
drawn into a second image, shown for one vertical blank, and then the real frame follows. Triangles with no partner, or that moved too far (a camera
cut, a fade), are drawn where the new frame has them, so a cut never smears. Frames that contain framebuffer uploads (videos, loading pictures) are
never interpolated, and neither are scenes that already run at 60 fps.

- **Cost:** about 0.2 ms per frame on top of the normal rendering, and **one vertical blank (16.7 ms) of extra delay**, because the in-between picture
  needs the new frame to exist. It is off by default.
- **Looks:** the game's own timing and physics are untouched (the game still thinks at 30 fps); only the picture is smoother. Things that appear or
  disappear are drawn in the frame they exist in; a polygon that turns edge-on can look thin for a moment.
- **Check it yourself:** `cb2_play --capture-mid --smooth-t 0.5 --capture FRAME:out.png` saves an in-between picture (`--smooth-t 0` is the newest
  real frame: the program then also reports how many pixels differ from the real picture, which must be 0; `1` is the previous frame).
  `CB2_SMOOTHLOG=1` prints how many triangles were matched for each in-between picture. `cb2_interptest` tests the matching.

## Frame pacing and vsync

The game is paced against the wall clock at the console's 59.94 Hz. Windows sleeps in steps of about 15 ms unless asked for 1 ms, which made the
rhythm uneven (a frame sometimes came twice as late and the next one early): the timer resolution is now set to 1 ms and the last couple of
milliseconds before a frame are spent waiting actively. *Vsync* (on by default, `vsync = off` in `cb2.cfg`) makes each picture wait for the monitor's
refresh, so there is no tearing; it is not used while fast-forwarding (`Tab`).

## Emulation speed

The PlayStation CPU is interpreted. The engine spends most of its time in the `VSync()` wait: a 14-instruction polling loop that waits for the next
vertical blank. The interpreter now recognises that loop (checks that the code is the expected one, that no interrupt is pending and that no
device - vertical blank, CD controller, timers, SPU - has anything due) and accounts for its passes in one step: the same machine state, the same
guest time, bit-identical pictures (`CB2_NOIDLESKIP=1` turns it off to compare). Emulating a frame went from about 5.3 ms to about 2.8 ms on the
development PC, which matters most on slow computers and during level loading, when the game does a lot of real work in a frame.

Measuring: `CB2_PERF=1` prints the average time per frame spent emulating, rendering and swapping every 600 frames; `CB2_PCHIST=1` adds the
addresses the guest code spends its time at; `CB2_FRAMELOG=1` reports slow frames and disc reads (see ONLINE.md).
