# Crash 2 NSF/NSD format notes (as implemented)

Sources: the nocash PSX-SPX "NSD/NSF" page and the open-source CrashEdit project, cross-checked against the
shipped files (all 38 NSD files are version v2; all 2,548 NSF chunks decode; all 696 scenery entries match
the expected item sizes).

## NSF container
- A sequence of chunks, each starting on a 0x800 boundary. Every chunk decodes to exactly 0x10000 bytes.
- `0x1234` = raw chunk (occupies 0x10000 bytes). `0x1235` = compressed chunk: `u32 decodedSize`, `u32 skipSize`, then
  LZSS/literal-run data, `skipSize` unused bytes, then the remaining raw tail up to 0x10000.
  - control byte `< 0x80`: copy that many literal bytes.
  - control byte `>= 0x80`: `v = ((b & 0x7F) << 8) | next`; `disp = v >> 3`, `len = (v & 7) + 3` (`len == 10` means 0x40);
    copy `len` bytes from `disp` bytes back.
- Decoded chunk header: `u16 0x1234`, `u16 family`. Family 0 = entries, 1 = texture page, 2–5 = sound.
- Header checksum at +0xC (verified on all 2,548 chunks): `c = 0x12345678`; for every byte `i` of the 64 KiB page
  `if (i < 12 || i >= 16) c += byte; c = rotl32(c, 3);` (the rotation also runs for the four checksum bytes).
  The page must be rewritten with a fresh checksum after any edit (`finalizePage`).
- Family 0 (and 2-5, which share the layout): `u32 chunkId` at +4 (`chunkNumber * 2 + 1`), `u32 count` at +8, then
  `count + 1` offsets from +0x10; the last offset is the end of the used area. Each entry is `0x0100FFFF`, `u32 eid`,
  `u32 type`, `u32 itemCount`, `itemCount + 1` item offsets (relative to the entry), then the items back to back.
  Packing rule (reproduces all 1,759 entry pages byte for byte): entries are laid out in order; each starts at the
  first offset where its *item data* (entry start + 16 + 4 * (itemCount + 1)) is 4-byte aligned; unused bytes are zero.
- Names (`eid`): five 6-bit characters from `0-9a-zA-Z_!`; chars 1–4 at bits 25/19/13/7, extension at bit 1.

## Texture pages (family 1)
Raw VRAM bytes: 512 bytes per row, 128 rows, referenced by EID. A texel's CLUT sits in the same page.
Mode 0 = 4-bit, 1 = 8-bit, 2 = 15-bit direct. Segment `n` starts `128 * n` bytes into each row.
CLUT entry = `halfword[clutY][clutX * 16 + index]`. Value 0 is transparent. 15-bit = `0BBBBBGGGGGRRRRR`.

## Scenery entry (type 3), 7 items
0. Header (76 bytes): `i32 xOffset, yOffset, zOffset`, `u32 isSky` (+0xC), counts at +0x10..0x24
   (vertices, triangles, quads, textures, colours, animated textures), `u32 tpageCount` at +0x28, then page EIDs at +0x2C.
1. Vertices: `count` XY words stored in reverse order, then `count` Z halfwords. Each component is `value >> 4` (signed 12-bit);
   the low nibbles of x/y/z form the colour index `((y & 3) << 8) | ((x & 0xF) << 4) | (z & 0xF)`.
2. Triangles: same reversed-word + halfword layout; three 12-bit vertex indices, 11-bit texture index, animated flag.
3. Quads: 8 bytes each; vertex order A, B, D, C.
4. Texture info (12 bytes): UVs, CLUT x/y, segment, blend mode, colour mode, page index.
5. Colours: RGB + 1 byte, 128 = neutral.
6. Animated textures: 32-bit flipbook (mask/delay/latency/leap) or LOD descriptors.

World position = `(vertex * 16 + offset) / 400`.

## NSD index (v2)
Layout: `u32 table[256]` (0x000), `u32 chunkCount` (0x400), `u32 N` (0x404), level-data name (0x408), loading-bitmap
palette size / width / height (0x40C-0x414), compression info (0x418-0x51F, all zero in Crash 2), then `N` lookup
entries of `{u32 chunk, u32 eid}` at 0x520, then level data with spawn points, then the loading bitmap.
All 38 shipped NSDs are v2. The lookup is fully derivable from the NSF (reproduced byte for byte by `rebuildNsd`):
- list = every entry of every non-texture page as `{page's chunkId, eid}`, plus every texture page as
  `{pageIndex * 2 + 1, textureEid}`;
- sorted by bucket `(eid >> 15) & 0xFF`, then `eid` ascending (no duplicate EIDs occur);
- `table[k]` = index of the first list entry whose bucket is `>= k`, clamped to `N - 1`;
- `chunkCount` = number of pages in the NSF.
Spawn points, level data and the loading bitmap are not derived and are carried over unchanged.

## Executable
`SCUS_941.54` is a PS-X EXE (load 0x80010000, entry 0x80049B2C, 0x4F800 bytes). REA's native providers do not
support MIPS, so analysis of it needs a different disassembler (for example Capstone's MIPS decoder).
