# Props: things that are not in the game but live in it (experimental)

**Pause menu > Settings > Experimental > Props.** The first prop is a wooden box in the warp room. It is not in the game's data: the game does not know it exists. You can walk up to it (it is a wall at the sides) and jump on it (it is a spring on top), and
Crash goes up like on a bounce crate. Switch it off and nothing of it is left. It works on both releases (NTSC-U and PAL) and with native world, widescreen and 60 fps smoothing on or off.

## How it works

Two halves, both in `src/props.cpp`:

1. **Drawing.** Every frame the game sends its polygons for the picture to the GPU. `Props::onDisplayList` takes the camera the game used for the level (the matrix and the offsets of the last scenery projection the GTE ran, plus the camera position the game keeps in
   memory), projects the corners of the box with it, and inserts the box's triangles into the frame's polygon list **at the place its depth belongs to**. The game paints far to near (the painter's algorithm), so Crash and everything else
   that is nearer than the box is drawn over it, and what is farther is drawn under it. The triangles carry the depth of their corners, so widescreen (which stretches the picture from the polygons' depth) and the in-between pictures of 60 fps (which match polygons
   between two pictures) treat the box like any other 3D polygon. Faces that look away are not drawn, and a box that has a corner nearer than the camera's near plane is not drawn (there is no clipping).
2. **Interaction.** The game's object update is hooked (the same hook the multiplayer code uses). After Crash's own update `Props::interact` reads his fields (`x y z`, `velx vely velz`: field numbers 8-10 and 17-19 of the object, see `gool_fields.inc`) and writes them back:
   * Crash's feet inside the height of the box and closer than the box's half size plus Crash's radius: he is pushed out along the shortest way and the speed in that direction is zeroed (a wall);
   * Crash's feet cross the top while falling: his position is set to the top and `vely` to the box's bounce speed (a normal jump starts at 936025; the box gives 1200000). The game's own jump code carries on from there, because it integrates `vely` by itself: the arc, the
     animation (the jump and fall poses) and the landing are the game's.

It writes only what a GOOL script would write to the same object, so the game cannot tell the difference between this and a crate that bounces him.

Nothing is sent over the network. Crash's bounce is seen by the other players as Crash moving, like every jump. The box itself is drawn only for players who switched Props on (a ghost Crash of another player walks through it on your screen if they do not have it).

## Your own boxes: `props.txt`

Put a file `props.txt` next to the game (`Crash2.exe`). One box per line, in world units, `#` starts a comment:

```
# box  <level>  <x>   <z>   <floor y>  <half size>  [<bounce speed>]
box    02       1166  471   112.5       250
box    02       600   900   112.5       150          1800000
```

`level` is the number the game uses, in hex (`2` = the warp room; `CB2_PROPLOG=1` prints the level of a started game). `x z` is where the middle of the box is; `floor y` is the height of the floor it stands on (the warp room's is 112.5); `half size` is half of the edge (40 to 3000);
the bounce speed is optional. When the file has at least one valid line it replaces the built-in box. The positions of a level can be read in the game with `CB2_PROPLOG=1 CB2_PROPTRACE=1` (it prints Crash's position every frame).

## Why not a GOOL script?

The game's own scripts are GOOL ("Game Object Oriented Lisp"), the language the original objects are written in (`docs/GOOL.md`). Three levels of doing this were considered:

| | What it is | Status |
| --- | --- | --- |
| **Host props** (this page) | The prop is drawn and decided by the program around the emulated game; it talks to the game only by reading and writing the player object's fields and by adding polygons to the frame. | done |
| **A game object driven from outside** | A real game object (made with the game's own creation routine, like the other players' Crashes in `multiplayer.cpp`) whose script is switched off and whose position, rotation and animation the program writes. The game then draws it with its own models and sorts it itself; it can use any model that the level's data has loaded. | the machinery exists (multiplayer puppets); a prop with it is possible for models that are in the level |
| **A real GOOL entry** | A new script, assembled with the tools in `gool_asm.cpp`, put into the game's memory and registered in the level's entry table, so that the game runs it in its own object loop (collisions with Crash through the game's own box and "collider" mechanism, events, `statetime`...) and the script can be shared as a file. | not done: the engine looks entries up in the page table of the level it loaded, so a new entry needs space in that table and a model/animation to point at; this is the road to custom levels and custom objects |

The host prop is the quick way and is enough for boxes, springs, platforms and switches whose look is made of coloured polygons. For things that need the game's models (a crate with the real "?" texture, an enemy, a fruit) the second row is the next step; the third is what a level editor
and custom levels need, and is where this goes (see the roadmap on the project page).

## Limits

* The box is flat-coloured polygons (no texture yet). The look is "a wooden crate with a green top and an arrow".
* The collision knows boxes only (axis-aligned, no rotation, no moving ones). Crash's own collision with the level (the floor, the walls) is the game's: a box placed inside a wall can push Crash into it.
* Boxes are per level number: the language copies of the PAL warp room (0x2D-0x30) count as the warp room.
* It needs the interpreter and the hi-res renderer (the default), like the multiplayer.

## For developers

`CB2_PROPS=1` switches Props on without the menu. `CB2_PROPLOG=1` prints what it does (`[prop]` lines); with `CB2_PROPTRACE=1` also Crash's position and speed at every update; `CB2_PROPDROP=<frames>` puts Crash above the first box that many frames after the start,
which makes the bounce a test that needs no pad (`cb2_play --load-state <warp room state> --rel --capture 80:file.png`).
