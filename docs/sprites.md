# Driver / kart sprites

Everything here is read from the ROM the player supplies; nothing is embedded
in the executable. Code: `src/smk/sprite.h`, CLI switches in `tools/game.cpp`.

## The sheets

Each driver has a 0x6000-byte 4bpp sprite sheet at `<bank>:2000`, i.e. 768
tiles of 32 bytes:

| driver | file offset | SNES |
|---|---|---|
| Mario  | `0x002000` | C0:2000 |
| Luigi  | `0x012000` | C1:2000 |
| Peach  | `0x022000` | C2:2000 |
| Daisy  | `0x032000` | C3:2000 |
| Yoshi  | `0x042000` | C4:2000 |
| Toad   | `0x052000` | C5:2000 |
| Koopa  | `0x062000` | C6:2000 |

The sheets are genuinely different graphics, not recolours: Mario's and
Luigi's differ in 14403 of 24576 bytes, Yoshi's in 12047, and so on. Bowser
(driver 7) has no sheet of its own here, so he borrows Koopa's for now.

The 4bpp planar reading is the right one: decoding the same bytes as 2bpp
produces only vertical stripes, while 4bpp (16 bytes of bitplanes 0/1 followed
by 16 of bitplanes 2/3) yields unmistakable karts.

The 88-word block at `<bank>:1F4C` that the extractor flagged as a palette is
**not** the driver's palette -- it is all blue/teal, and rendering with it
gives teal karts. The real colours come from the course palette stream (see
below).

## Where the colours come from

`CODE_81EFB9` initialises each racer:

```
CODE_81EFB9:
	LDY.w #$1700
	LDX.w #$000E
CODE_81EFBF:
	LDA.l DATA_80846E,x     ; tile base
	STA.w $00B6,y
	CLC
	ADC.w #$1000           ; second buffer for player 2
	STA.w $00B8,y
	LDA.l DATA_80847E,x     ; OAM palette / attribute
	PHX
	LDX.b $12,y
	ORA.l DATA_80848E,x
	STA.w $0014,y          ; attribute word
```

and `DATA_80847E` is

```
dw $0180,$0184,$0188,$018C,$01C0,$01C4,$01C8,$01CC
```

-- four words per racer, at CGRAM `$0180` and `$01C0`. The only CGRAM write the
game ever does is `CODE_808413`, which DMAs 0x200 bytes from `$7E3A80` to
CGRAM `$0000`; that is the course palette stream, and it covers `$0000-$01FF`.
So **CGRAM `$0180` is word `0xC0` of the course palette stream**, and the
driver's colours are simply a sub-block of the palette we already load for the
track.

That is confirmed, and the per-driver block is derived rather than hard-coded:

* the kart's dominant pixel is index 1 (45% of the sprite, from
  `smk-tilehist`);
* in BGR555 `0x7C00` is red, `0x03E0` is green, `0x03FF` is yellow and
  `0x001F` is **blue** -- getting this backwards is what made Mario's kart come
  out blue for a while;
* so a driver's block is the first one in the course palette whose second word
  is that driver's signature colour, which `driverPalBlock()` does from
  `kDriverSignature[]`.

That puts Mario on block `0x02` (word 1 = `0x7C00`, pure red), Luigi on `0x04`
(`0x03E0`), Daisy on `0x05` (`0x03FF`), and renders them red and green as the
reference sheet shows.

The reference renders in the project root (`SNES - Super Mario Kart - Racers -
*.png`, `... - Tracks - *.png`) are what this was checked against. They are
*rendered images*, not palette dumps -- the exact words in them do not appear
anywhere in the ROM -- so they are a visual specification, useful for getting
hues and layouts right and useless for reading values out.

The Mario Circuit 1 map render was checked against the matching track
reference and matches: same road shape, kerbs, dirt, grass and starting grid.

## Sprite size and placement

A kart frame is a 4 x 2 block of 8x8 hardware tiles (32 x 16 pixels). The OAM
builder writes four entries across the top row with the tile number stepping by
one each time (`ADC.w #$0100` between `STA.w $0EA0,x` and `STA.w $0EA6,x`), and
a second row below (`ADC.w #$0200` for Y, `ADC.w #$0400` for the tile).

Rendering tiles 48..55 with block `0xD0` gives a complete kart with driver;
16x16 and 32x32 grids both smear neighbouring frames into each other, which is
how the 4x2 size was confirmed. The driver sprite mirrors when the kart points
left (`fX < 0`), which is what the original does with its OAM h-flip bit.

## Debug switches

Screenshots are not always eyeballed, so these print what they drew:

```
--tilebase N      first tile of the kart sprite (default 48)
--sprcols N       tiles across  (default 4)
--sprrows N       tiles down    (default 2)
--sprscale N      pixels per source pixel (default 3)
--palidx N        16-word palette block; -1 derives it from DATA_80847E
--sheet FILE      the whole 768-tile sheet at 2x, 16 tiles per row
--blocks FILE     every frame as a grid, --blockcount N to limit
--paldump         print all 256 palette words as BGR555 and RGB
```

`--blocks` with a small `--blockcount` and `--sprscale 3` is the quickest way
to find a new frame: it lays frames out 4 per row on grey so they can be
counted back to a tile index.

## Still to do

* `DATA_80847E` says racer N uses CGRAM `$0180 + N*4`, i.e. four words, but the
  sheets use all sixteen indices, so the game's own table does not describe
  this directly. `driverPalBlock()` matches the reference renders instead; the
  real table is still worth finding.
* The animation order: which tile block is "driving straight ahead", and the
  per-direction and per-speed frame sets.
* Steering frames, the squash/stretch frames and the spin frames are all in the
  sheet but are not addressed yet.
* Vertical scaling with distance: the original shrinks and squashes the kart
  as it moves away; here it is drawn at a fixed size.
* The kart shadow (`SMK_GFX_Sprite_DirtAndTireSmoke` and friends) is separate.
* Bowser has no sheet of his own and borrows Koopa's.
* Peach, Toad and Koopa's signature colours are guesses and have not been
  checked against their reference renders.
