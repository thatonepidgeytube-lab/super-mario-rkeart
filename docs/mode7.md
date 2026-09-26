# Mode-7 track graphics, as the game actually loads them

Everything below was read out of the disassembly (`bin/smk-disasm.exe`, oracle
`Routine_Macros_SMK.asm`) and verified against the ROM the player supplies.
Nothing is embedded in the executable.

## The loader

Three routines in bank $81 do the work, all with `DBR = $81`:

```
CODE_81E745  level data   DATA_81EB5B[id]    -> decompress -> $7FC000
                                               decompress -> $7F0000   (2-stage)
CODE_81E6D4  track tiles  DATA_81EBA3[type]  -> decompress -> $7FC000
                                               CODE_84E378 -> $7F4000
            decor tilemap DATA_81EBEB[type]  -> decompress -> $7FC800
            HUD tilemap  SMK_Tilemap_TimerAndItemBox -> $7FD000
CODE_81E6B9  shared tiles $C4:0000            -> decompress -> $7F0000
                                               CODE_84E378 -> $7F7000
CODE_81E72E  palette      DATA_81EBBB[type]  -> decompress -> $7E3A80
CODE_81E769  DMA $7F4000 -> VRAM $0000  0x3000 bytes (192 tiles, 8bpp)
CODE_81E68E  DMA $7F7000 -> VRAM $3000  0x1000 bytes ( 64 tiles, 8bpp)
CODE_81E7B5  DMA $7F0000 -> VRAM $0000  0x4000 bytes (tilemap, low bytes)
```

So the char block mode 7 reads is 192 course tiles followed by 64 shared tiles,
and the tilemap is the 128x128 grid of tile numbers, 0x4000 bytes.

## Table addresses (file offsets, HiROM)

| Label | File offset | Contents |
|---|---|---|
| `DATA_81EB5B` | `0x1EB5B` | 24 x 3-byte level-data pointers |
| `DATA_81EBA3` | `0x1EBA3` | 8 x 3-byte mode-7 tile pointers |
| `DATA_81EBBB` | `0x1EBBB` | 8 x 3-byte palette pointers |
| `DATA_81EBEB` | `0x1EBEB` | 8 x 3-byte decor-tilemap pointers |
| `DATA_81EC03` | `0x1EC03` | 8 x 3-byte BG-tilemap pointers |
| `DATA_81EC1B` | `0x1EC1B` | 20 x `Define_SMK_RacetrackID_*` |
| `DATA_81EC2F` | `0x1EC2F` | 20 x `Define_SMK_RacetrackType_*` |
| `SMK_GFX_Mode7_GlobalTrackTiles` | `0x40000` | the 64 shared tiles |

## Which tile set a course uses

`CODE_81E84C` turns the track type into a table index:

```
LDA RAM_SMK_Global_RacetrackType
LSR
CLC
ADC RAM_SMK_Global_RacetrackType   ; type + type/2 = type * 3/2
TAX
```

The types are the even numbers 0, 2, 4 ... 14, so the index is `type * 3 / 2`,
i.e. **3 bytes per entry and family = type / 2**. `DATA_81EC2F` holds one type
byte per race course, in the same order as `DATA_81EB5B`, and `familyForCourse()`
in `src/smk/course.h` reads it at runtime. The values in the ROM are:

```
type/2:  1 0 2 6 4 7 5 1 0 6 3 2 4 5 1 1 0 6 3 2
```

The 4 battle courses (ids 20..23) are not in that table.

## Tile set sizes

Each family has a different number of custom tiles; the rest of the 192 slots
are simply unused, and maps only ever reference the tiles that exist plus the
64 shared ones. `--stats` prints this table straight from the ROM:

| fam | family | stream | tiles |
|---|---|---|---|
| 0 | Ghost Valley | `0x620` | 41 |
| 1 | Mario Circuit | `0x1900` | 192 |
| 2 | Donut Plains | `0x1100` | 128 |
| 3 | Choco Island | `0xD80` | 100 |
| 4 | Vanilla Lake | `0x10C0` | 126 |
| 5 | Koopa Beach | `0x1100` | 128 |
| 6 | Bowser Castle | `0x125A` | 138 |
| 7 | Rainbow Road | `0x280` | 12 |

## `CODE_84E378`: 4bpp + attributes -> 8bpp

```
attribute byte for tile i  =  src[i]
32 source bytes            =  src[0x100 + i*32]        <- five ASLs, i.e. i*32
pixel = nibble ? (nibble | attribute) : 0
low nibble is the left pixel
```

The **32-byte stride** is confirmed twice over: the shared stream decompresses
to exactly `0x900` = `0x100 + 64*32` bytes, and Mario Circuit's to exactly
`0x1900` = `0x100 + 192*32`. (An earlier port used `i*64` and got away with it
because unused slots were read as 0.)

## `CODE_84E09E`: the decompressor

Command byte `b`, 8 commands plus an extended form:

| `b >> 5` | meaning |
|---|---|
| 0 | `count` literals follow |
| 1 | fill `count` copies of the next byte |
| 2 | alternate the next two bytes, `count` bytes total |
| 3 | fill with an incrementing byte |
| 4 | copy `count` bytes from output offset `next2` (16-bit) |
| 5 | same, with every byte inverted |
| 6 | copy `count` bytes from `out.size() - next` (overlaps allowed) |
| 7 | same, inverted (extended form only) |

`count = (b & 0x1F) + 1`. When `b >= 0xE0` an extra byte follows, the command
becomes `(b & 0x1C) >> 2` (so commands 6 and 7 exist) and the count is
10-bit: `count = (((b & 3) << 8) | next) + 1`. `0xFF` ends the stream.

The original reads and writes `$7F` RAM directly, so a copy that points
outside the output produced so far is **legal** - it just returns whatever the
buffer happened to hold. Bowser Castle's stream does this once (an absolute
copy of 2 bytes from `0xA306` with only `0xD90` bytes written), so `src/smk/lz.h`
clamps those reads to `0x00` instead of failing. Everything else in every
stream decodes with no out-of-range reference at all.
