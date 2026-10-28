# Props: things that are not in the game but live in it (experimental)

**Pause menu > Settings > Experimental > Props.** Two things: the game's own **"?" crate** in the warp room (a host prop, below) and the **game’s own crates in Turtle Woods**, placed anywhere and in any number (real game objects made while Crash is near, see Crates of the game’s own script). Its model, its size and its textures are the game's: they are read at the start from the Turtle Woods level file on your own disc (the crate is in that file, `Bp10.G` / `Bp10.V`, 12 polygons, 398 units on a side), so nothing was invented or drawn by hand, and nothing of the game is stored by this program. The crate is not in the warp room's data: the game does not know it exists there. You can walk up to it (it is a wall at the sides) and jump on it (it is a spring on top), and
Crash goes up like on a bounce crate. Switch it off and nothing of it is left. It works on both releases (NTSC-U and PAL) and with native world, widescreen and 60 fps smoothing on or off.

## How it works

Three parts: `src/propasset.cpp` (the look), `src/props.cpp` (the drawing and the interaction):

0. **The asset.** `loadAsset` finds the animation `Bp10.V` in the level file, takes its first frame and the model that frame shows, decodes the vertices the way the model viewer does (`cb2::frameVertices`, scale and offset of the frame), and cuts every texture the polygons use out of the level's texture pages with its palette applied
   (4, 8 or 15-bit, the same decode the console's GPU does) into a 256 x 256 atlas of 15-bit colours. The renderer (`glrender.cpp`) has a second texture for it, so the prop does not touch the console's video memory, which a level can fill completely. A model that is a closed solid
   (the crates are: every edge belongs to exactly two polygons) has its winding made consistent, so that the faces that look away can be left out.

1. **Drawing.** Every frame the game sends its polygons for the picture to the GPU. `Props::onDisplayList` takes the camera the game used for the level (the matrix and the offsets of the last scenery projection the GTE ran, plus the camera position the game keeps in
   memory), projects the corners of the model with it, and inserts its triangles, textured from the atlas, into the frame's polygon list **at the place its depth belongs to**. The game paints far to near (the painter's algorithm), so Crash and everything else
   that is nearer than the box is drawn over it, and what is farther is drawn under it. The triangles carry the depth of their corners, so widescreen (which stretches the picture from the polygons' depth) and the in-between pictures of 60 fps (which match polygons
   between two pictures) treat the box like any other 3D polygon. Faces that look away are not drawn, and a prop that has a corner nearer than the camera's near plane is not drawn (there is no clipping).
2. **Interaction.** The game's object update is hooked (the same hook the multiplayer code uses). After Crash's own update `Props::interact` reads his fields (`x y z`, `velx vely velz`: field numbers 8-10 and 17-19 of the object, see `gool_fields.inc`) and writes them back:
   * Crash's feet inside the height of the box and closer than the box's half size plus Crash's radius: he is pushed out along the shortest way and the speed in that direction is zeroed (a wall);
   * Crash's feet cross the top while falling: his position is set to the top and `vely` to the box's bounce speed (a normal jump starts at 936025; the box gives 1200000). The game's own jump code carries on from there, because it integrates `vely` by itself: the arc, the
     animation (the jump and fall poses) and the landing are the game's.

It writes only what a GOOL script would write to the same object, so the game cannot tell the difference between this and a crate that bounces him.

Nothing is sent over the network. Crash's bounce is seen by the other players as Crash moving, like every jump. The box itself is drawn only for players who switched Props on (a ghost Crash of another player walks through it on your screen if they do not have it).

## Your own props: `props.txt`

Put a file `props.txt` next to the game (`Crash2.exe`). One prop per line, in world units, `#` starts a comment:

```
# box  <level>  <x>   <z>   <floor y>  [<scale>  [<bounce speed>  [<crate>]]]
box    02       1166  471   112.5
box    02       600   900   112.5       0.6        1800000        Bp10
box    02       -500  700   112.5       1          1200000        BT10
```

`level` is the number the game uses, in hex (`2` = the warp room; `CB2_PROPLOG=1` prints it for a started game). `x z` is where the middle of the crate is; `floor y` is the height of the floor it stands on (the warp room's is 112.5); `scale` is 1 for the game's own size (0.1 to 10);
the bounce speed is optional (a normal jump starts at 936025; the default is 1200000); `crate` is which of the game's crates it is. When the file has at least one valid line it replaces the built-in prop. At most four different crates can be used at once.
The positions of a level can be read in the game with `CB2_PROPLOG=1 CB2_PROPTRACE=1` (it prints Crash's position every frame).

The crates in the Turtle Woods file (the name is the animation's; all are 12-polygon boxes of the same size unless said):

| Name | Crate | Name | Crate |
| --- | --- | --- | --- |
| `Bp10` | "?" (the default) | `BT10` | TNT |
| `BN10` / `Bn10` | Nitro | `BL10` | Crash (extra life) |
| `BD10` / `Bd10` | the red "X" (a Nitro's switch crate) | `BI10` | steel (iron) |
| `BC10` | checkpoint "C" (22 polygons, wider) | `BS10` | arrow (bounce up) |
| `BF10` | wooden slats (20 polygons) | `BG10` | glass (see-through; the polygons are drawn solid) |

They are drawn but not yet what they are in the game: every one of them is just a box that bounces (a TNT crate does not explode, a Nitro does not hurt). For crates that are what they are in the game, see the next section.

## Crates of the game's own script, as many as you like (Turtle Woods)

In **Turtle Woods** (with Props on) crates can be placed anywhere, in any number, and they are **the game's own crates**: spinning into them or jumping on them breaks them, "?" crates give their Wumpa fruit, the Aku Aku crate gives the mask, the Crash crate a life, TNT counts down and explodes,
Nitro explodes when touched, the arrow crate bounces. None of that is written here: a crate that is near Crash is a real game object, made with the engine's own object-creation routine (`0x80019800`) as an object of the game's crate script `Boxs.C` (object type 34 of that level),
so it is exactly what the game makes. The trick that makes the number unlimited is that a crate is only made **while Crash is near it** (within 2600 units, at most 20 at once, the nearest first) and is thrown away again, untouched, when he is far (3200). This is what the game itself does with its own entities. A crate that the game removed
(broken, exploded, collected) stays removed until the level is entered again or Crash is made anew (a fall, a checkpoint). The cost is the cost of the crates that are near, about 0.1 ms of emulation per crate and frame; crates that are far cost nothing.

A test field of 3 x 8 crates (every kind) is placed by default ahead of where Crash starts in Turtle Woods. Your own go in `props.txt` (the file replaces the default field when it has at least one of these lines):

```
# crate <level> <x> <floor y> <z> <subtype>
crate  1E  45024  -13  172800  2
# wall <level> <x> <floor y> <z> <x|z> <columns> <rows> <subtypes>   a grid, 400 apart, along x or z and upwards; the subtypes are used in turn
wall   1E  43600  -13  172000  x  8  3  2,10,3,0,18,8,9,6
```

The positions are the game's world units (the same in both releases: Crash starts Turtle Woods at x 45024, floor -13, z 173336, running towards smaller z). `CB2_PROPLOG=1` prints what is made and what the game removed.
The **subtype** says what a crate is: 2 wooden, 10 "?" (fruit), 0 TNT, 18 Nitro, 3 arrow (bounce), 6 slats, 8 Crash (extra life), 9 Aku Aku mask; also wooden 21 25 27-30 35-38 and "?" 26 34 (found by making one of each and looking), the outlined "ghost" crates 4 5 7 13 19 23 24 (they float up and down by design
and appear solid when a "!" crate, subtype 22, is hit), 16 another TNT. Subtype 14 removes itself at once. Subtypes 1 11 12 15 17 20 31 32 33 39 fail inside the game (they need data this level does not have): the call is undone and the crate is left out.

**Only in levels whose file has the crate script.** Turtle Woods has it; the warp room does not, so the warp room keeps the host crate below. Other levels have crate scripts of their own that this has not been taught yet.
A state saved with crates of an older version of this feature has them taken away when it is loaded.

## Why the warp room's crate is not a real object (yet)

The "?" crate in the warp room is the **host** crate of the first section: the game's own model and textures, drawn among the game's polygons, with a collision made from its box. It has the game's size (checked by drawing it over the game's own crate in Turtle Woods: the scale 1.0 hides it completely, 1.02 already shows a rim),
but it is not a game object, so it cannot do what the game's crates do: spinning into it does nothing, no fruit comes out, and Crash's shadow (a game object that projects onto the level's floor) falls on the floor below it.
To make it a real crate in the warp room, the warp room's level file has to get the crate script and what it needs. The warp room has `Bp10` and `Bd10` (the models of the "?" and the red "X" crate), `Frui.C` (the fruit) and `Part.C` (particles), but not `Boxs.C`, the other crate models and their textures.
Adding them is a change of that level's file (entries repacked, the NSD index rebuilt: `docs/EDITING.md`) and is the next step; the test ISO `c2_custom_models_test` (a modified warp room with a custom model) is the model for it.
## Why not a GOOL script?

The game's own scripts are GOOL ("Game Object Oriented Lisp"), the language the original objects are written in (`docs/GOOL.md`). Three levels of doing this were considered:

| | What it is | Status |
| --- | --- | --- |
| **Host props** (this page) | The prop is drawn and decided by the program around the emulated game, with the game's own model and textures; it talks to the game only by reading and writing the player object's fields and by adding polygons to the frame. | done |
| **A game object driven from outside** | A real game object (made with the game's own creation routine, like the other players' Crashes in `multiplayer.cpp`) whose script is switched off and whose position, rotation and animation the program writes. The game then draws it with its own models and sorts it itself; it can use any model that the level's data has loaded. | the machinery exists (multiplayer puppets); a prop with it is possible for models that are in the level |
| **A real GOOL entry** | A new script, assembled with the tools in `gool_asm.cpp`, put into the game's memory and registered in the level's entry table, so that the game runs it in its own object loop (collisions with Crash through the game's own box and "collider" mechanism, events, `statetime`...) and the script can be shared as a file. | not done: the engine looks entries up in the page table of the level it loaded, so a new entry needs space in that table and a model/animation to point at; this is the road to custom levels and custom objects |

The host prop is the quick way and is enough for crates, springs, platforms and switches. For things the game itself must treat as objects (an enemy, a fruit, a crate that explodes) the second row is the next step; the third is what a level editor
and custom levels need, and is where this goes (see the roadmap on the project page).

## Limits

* The collision is the box of the model's bounds (axis-aligned, no rotation, no moving ones); for the "?" crate that is its shape. The model's lighting is the game's vertex colours as they are in the file: it is not lit by the level (the game's crates are not either).
* A crate's animated textures (the flipbook of a glowing crate) show their first picture. Crash's own collision with the level (the floor, the walls) is the game's: a box placed inside a wall can push Crash into it.
* Boxes are per level number: the language copies of the PAL warp room (0x2D-0x30) count as the warp room.
* It needs the interpreter and the hi-res renderer (the default), like the multiplayer.

## For developers

`CB2_PROPS=1` switches Props on without the menu. `CB2_PROPLOG=1` prints what it does (`[prop]` lines); with `CB2_PROPTRACE=1` also Crash's position and speed at every update; `CB2_PROPDROP=<frames>` puts Crash above the first prop that many frames after the start,
which makes the bounce a test that needs no pad (`cb2_play --load-state <warp room state> --rel --capture 80:file.png`).
