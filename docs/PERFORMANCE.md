# Speed, smoothness and 60 fps

## 60 fps smoothing (*Esc → Settings → Display → 60 fps smoothing*, `smooth = on` in `cb2.cfg`)

Crash Bandicoot 2 draws a new picture every second vertical blank, about 30 per second. With smoothing on, the picture in between is drawn too, so
the screen changes 60 times a second.

How it works (`src/interp.cpp`): the host renderer replays what the game told the GPU to draw. The recorded commands of a frame are kept, and when the
game flips to a new frame the previous and the new frame are compared: every triangle of the new one is looked up in the old one (same texture,
texel coordinates, palette and colour - a polygon keeps those from frame to frame, and it must have a similar size and shape) and its corners are
moved half way towards where the old one had them. The result is drawn into a second image (started from the real picture), shown for one vertical
blank, and then the real frame follows.

Two things keep it free of artefacts:

- **One movement per corner.** Every screen position that is a corner of a matched triangle gets the *median* of the movements the triangles around it
  found, and triangles that share that corner all use it. Neighbouring triangles of a surface therefore always move together: no cracks open between
  them (they showed as black wedges along platform edges), a triangle matched to a wrong partner is outvoted, and a triangle without a partner follows
  the corners it has in common with matched ones. Triangles that jumped far (a camera cut, a fade) are not matched and stay where the new frame has them.
- **Exact timing.** Each display flip is stamped with the number of the vertical blank it happened after, and the main loop keeps running until the flip
  after the current vertical blank has happened (it comes 3,000 to 23,000 instructions after it). Before this, a pass could end just before the flip,
  the flip was seen one pass late and that frame lost its in-between picture: about a third of the frames were shown without one, which looked like
  stutter. Now 99% of the flips get one (`CB2_SMOOTHLOG=1` lists those that do not, with the reason).

Frames that contain framebuffer uploads (videos, loading pictures) are never interpolated, and neither are scenes that already run at 60 fps.

- **Cost:** about 0.2 ms per frame on top of the normal rendering, and **one vertical blank (16.7 ms) of extra delay**, because the in-between picture
  needs the new frame to exist. It is off by default.
- **Looks:** the game's own timing and physics are untouched (the game still thinks at 30 fps); only the picture is smoother. Things that appear or
  disappear are drawn in the frame they exist in; a polygon that turns edge-on can look thin for a moment.
- **Check it yourself:** `cb2_play --capture-mid --smooth-t 0.5 --capture FRAME:out.png` saves an in-between picture (`--smooth-t 0` is the newest
  real frame: the program then also reports how many pixels differ from the real picture, which must be 0; `1` is the previous frame).
  `CB2_SMOOTHLOG=1` prints how many triangles were matched for each in-between picture. `cb2_interptest` tests the matching.

## High refresh rate monitors (160 Hz and the like)

With 60 fps smoothing on and a monitor of 90 Hz or more (`refresh_rate = 0` asks the monitor; or set it by hand), every refresh of the monitor shows its own picture:
the game still thinks at 30 fps and the emulation runs at 59.94 Hz, but the renderer draws, for each refresh, the picture for that exact moment between the two newest real frames
(the motion of every polygon is scaled by how far along the moment is). On a 160 Hz monitor that is 160 distinct pictures a second instead of 60 with repeats, so motion has no
judder. The matching of the two frames is made once per real frame and reused by every picture in between, and only the visible part of the target is touched, which is what makes
160 Hz affordable (about 3 ms of rendering per game frame). Cost: the picture runs about one game frame (17 ms) behind. `CB2_PERF=1` prints how many pictures are presented per
second; `CB2_FAKEHZ=160` pretends to have such a monitor, `CB2_CONTINUOUS=1` forces the mode.

## Perspective-correct textures and widescreen (Esc -> Graphics)

- **Perspective** (`perspective = on`): the console interpolates texture coordinates linearly across the screen, so textures swim and warp on big floors and walls. The GTE hands over the depth
  of every projected vertex; the renderer passes it on as the clip-space `w`, and the hardware then interpolates the texture coordinates as for a real 3D surface. Polygons without a
  known depth (2D, sprites) are untouched. This is the part of DuckStation's PGXP that matters for textures; the geometry precision of PGXP (unrounded vertices) was already there.
- **Widescreen** (`widescreen = on`): the picture is shown at the window's aspect ratio (up to 21:9). 3D polygons are squeezed horizontally around the middle of the screen by 4:3 / aspect, and the
  wider display stretches them back to their true shape, so the view gets more room at the sides; the HUD is squeezed the same way but pinned towards its edge so it keeps its shape. Limit: the game
  does not draw what lies beyond its own 4:3 view (it culls it), so the new margins show only what the engine happened to draw there: some edges are empty or end in cut-off geometry.
  A true widescreen mode needs the engine's culling widened, which is a hack into the game code per level (not done).
- **PGXP depth buffer** is not implemented: the game depends on painter's-algorithm order, and a depth buffer from the GTE depth would change what is drawn on top.

## Native world (Esc → Settings → Experimental, `native_world = on`, **unstable**)

The level scenery is drawn from the level's .NSF file instead of from the game's projected polygons (`src/nativeworld.cpp`). It does not make the game faster (the GTE costs 0.2-0.4 ms of a
3-4 ms frame); what it gives is quality and flexibility.

- **How it works.** The game projects every scenery section with the camera matrix alone: a vertex is `16 * vertex + section offset - camera position`, rotated by the matrix and divided by its
  depth (`H = 288`). The renderer does that in a vertex shader with a real depth buffer, so textures are perspective-correct. The camera position is read from the game's memory (0x80060768);
  the memory value is up to a few units off, so the exact one is found by searching for the position where the vertices the GTE projected land on vertices of the level (they lie on a grid of 16).
- **Which polygons.** The game's own visibility culling is kept: the triangles it sent to the GPU are matched to the level's polygons through their corners, and exactly those are drawn natively
  (the game's copies are skipped). Polygons the game draws see-through (water, ice, glass) are left to the game, as are all models, sprites and the HUD.
- **The game's own scenery is not drawn at all.** Every triangle the game made from scenery vertices is dropped, including the polygons it splits when they come close to the camera (their made-up corners are
  not in the level file). The parent polygons of the sections the game drew from are drawn natively instead, under the picture with a depth buffer, so nothing is left blank. Only see-through scenery
  (portals, water, ice) is still the game's.
- **Culling.** The game picks the sections it draws from its visibility data and culls what lies outside its 4:3 view, so with its own polygons the world pops in at the sides when you move. With native world every section
  within the game's draw distance (the farthest depth it projected, plus 15 %) is drawn, so the widened view is filled and nothing pops in. Polygons the game draws see-through (a textured polygon with a blend mode in the level file:
  portals, glows, water) stay the game's; they are told apart by the level file, not by matching triangles.
- **Order, not depth.** The game's painter's algorithm lets a polygon that is nearer be covered by one that is farther (a mushroom overlapping a stone block is drawn over it even though the block is
  in front in 3D). A depth buffer shows the geometry as it really is, which looks broken; so each scenery polygon is drawn natively *at the place of the game's triangle in the command list*, with
  no depth test, and the game's other polygons (models, near pieces of subdivided polygons) come in between exactly as they do on the console. Only the extra widescreen polygons use a depth buffer, under the picture.
- **Widescreen.** With widescreen on, the whole sections the visible polygons belong to are drawn, so the side margins show real geometry instead of the cut-off 4:3 view.
- **60 fps and high refresh rates.** The in-between pictures draw the scenery from the camera position and matrix interpolated between the two frames, instead of morphing the game's polygons.
- **Limits.** Texture animation (flowing water, flickering lights) is not played natively yet; the nearest section is chosen by what the game drew, so an area seen only through widescreen margins
  shows whole sections (some far parts the game would have culled); collision, entities and custom levels are not part of this stage.
- **Tools.** `CB2_NWLOG=1` prints the camera search and how many of the game's triangles were found in the level; `CB2_SESSION=file` records the scenery projections of a play session
  (`tools\record_session.ps1`), and `cb2_worldprobe` matches such data against level files.


The pauses when the game changes zone or level are the game waiting for its disc. The emulated drive used to read at the console's real double speed
(150 sectors a second) with a 12 ms spin-up per read, so a level load took 2.5 to 5 seconds, as on a real PlayStation. Now the emulated drive is faster
(`disc_speed = 16` in `cb2.cfg`; 1 = authentic, maximum 24). The game still reads one sector at a time through its own interrupts; only the waits are shorter.
Measured from the moment the game opens a level to the moment Crash can move (`CB2_RUNLOG=1`, frames at 59.94 per second):

| Load | `disc_speed` 1 | 4 | 8 | 16 | 24 |
| --- | --- | --- | --- | --- | --- |
| intro level (0x3C) | 300 frames (5.0 s) | 75 | 38 | 20 (0.33 s) | 14 |
| level 0x1C | 148 frames (2.5 s) | 37 | - | 10 (0.17 s) | 7 |
| warp room (0x02) | 212 frames (3.5 s) | 54 | 26 | 15 (0.25 s) | 11 |

A whole level cannot simply be kept in memory: the console has 2 MiB of RAM and the engine streams level pages through it by design, so a level never fits. What
can be removed is the waiting. What is left of a load is the game's own CPU work, which no disc setting changes. (At 32x the boot sequence was delayed, so the
limit is 24.) The host side helps too: the first time the game reads a file, a helper thread reads the whole file once, so the operating system's cache holds it
before the game gets there (a cold disk or a virus scanner had made the first read of each area stall for half a second).
## Textures (*Esc → Settings → Display → Textures*, `textures = smooth` in `cb2.cfg`)

*Sharp* reads every texel as the console does (blocky when enlarged, the original look). *Smooth* filters the textures bilinearly in the renderer's
shader (`src/glrender.cpp`): the four texels around each sample are blended, texels that are transparent do not take part (so the edges of cut-out
sprites stay clean), and the texel indices stay inside the polygon's own texture coordinates, so nothing bleeds in from the neighbouring picture in the
texture page. Palettes, the texture window and the semi-transparency passes work as before. `cb2_play --textures smooth` forces it for one run.

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
