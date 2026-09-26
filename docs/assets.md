# Super Mario Kart (USA) — asset formats and confirmed offsets

Everything here was produced and verified against `Super Mario Kart (USA).sfc`
(512 KiB, headerless, HiROM; SNES address = bank `0xC0 + (offset >> 16)`, page
`offset & 0xFFFF`, i.e. `C0:0000` = file offset `0x000000`).

Ground truth for cross-checking: the *Epic Racers* SMK documentation
(<https://epicedit.stifu.fr/smk_doc>) and its reference PNGs under `/gfx/`.

---

## 1. Regenerating everything

```sh
# build the tool (repo root, PowerShell)
g++ -std=c++17 -O2 -Wall -Wextra -o bin/smk-extract.exe tools/extract.cpp

# or: make tools          (builds smk-disasm, smk-extract, smk-game)
# or: make assets          (builds + runs the discovery scan)

# full discovery scan (default mode)
bin\smk-extract.exe "Super Mario Kart (USA).sfc" -o assets
```

The scan is deterministic; deleting `assets/` first reproduces the exact file
set below.

## 2. What `assets/` contains (current counts)

| item | count |
|---|---|
| index entries (`index.txt` / `index.html`) | **1392** (729 palette, 420 decomp, 243 tiles) |
| confidence: high / medium / low / confirmed | 935 / 415 / 12 / 30 |
| `assets/decomp/*.png` — decompressed SMK streams | **420** (415 discovered + 5 curated) |
| `assets/tiles/*.png` — raw planar tile sheets | **286** (243 entries + 43 `_bppN` alternates) |
| `assets/palettes/*.png` swatches | **729** |
| `assets/palettes/*.pal` (RGBA8 hex, 1/line) | 729 |
| `assets/palettes/*.hex` (raw 15-bit words, 8/line) | 729 |
| **total PNG files** | **1435** |
| **total files** | **2896** (incl. `index.txt`, `index.html`, `dma-hints.txt`) |

Naming: every region is `region_<offset in hex>` (`region_040000.png` = file
offset `0x040000`). Curated regions carry a human name in the index notes
(`lakitu:`, `road-gfx:`, `driver-0:`, …) and the confidence column `confirmed`.

### Manual modes (for iterating on one offset)

```sh
# tile sheet
bin\smk-extract.exe ROM --tiles 0x2000 --bpp 4 --count 768 --cols 16 \
    --pal 0x1F4C --palIndex 0 -o build\drv.png
# palette swatch only (also writes .pal + .hex)
bin\smk-extract.exe ROM --pal 0x4117F --count 256 -o build\tpal.png
# SMK-compressed stream: render as tiles, or dump raw bytes (.bin)
bin\smk-extract.exe ROM --decomp 0x40000 --bpp 4 -o build\road.png
bin\smk-extract.exe ROM --decomp 0x40000 -o build\roadg.bin
# mode 7 map + character data
bin\smk-extract.exe ROM --mode7 <mapOff> 128 128 --chars <chrOff> -o build\m7.png
```

Handy flags: `--rev N` (byte/plane reordering, see §3), `--linear [W]`
(chunky bitmap instead of tiles), `--scale N`, `--cols N`, `--score` (prints
the adj-equal-ness score; 0 = noise, ≥0.3 = image-like), `--trace`
(decompression step trace). Numbers accept `0x` hex.

---

## 3. Tile formats (SNES planar)

* **2bpp tile = 16 bytes**: 8 row bytes for plane 0, then 8 row bytes for
  plane 1. Pixel bit `x` (MSB = leftmost pixel).
* **4bpp tile = 32 bytes**: two 16-byte plane-pairs — bytes 0–15 hold bit
  planes 0/1 interleaved per row (`p0 r0, p1 r0, p0 r1, p1 r1, …`), bytes
  16–31 hold planes 2/3 the same way.
* **8bpp tile = 64 bytes**: four 16-byte plane-pairs (planes 0/1, 2/3, 4/5,
  6/7).
* Sheets are a plain grid of tiles, row-major, `--cols` per row (default 16).

`--rev N` handles the "reversed" orderings seen in some regions:

| N | reordering |
|---|---|
| 0 | canonical (above) |
| 1 | bit-reverse every byte (LSB-first planes) |
| 2 | reverse byte order inside each 16-byte plane-pair |
| 3 | reverse the whole 32-byte tile |
| 4 | reverse the order of the plane-pairs |
| 5 | 1 + 2 |
| 6 | reverse pixel-row order inside each tile |
| 7 | planes sequential (bytes 0–7 = plane 0, 8–15 = plane 1, …) |
| 8 | 4 adjacent plane bytes per row instead of 2+2 pairs |

Palettes are *assigned* to discovered graphics by proximity (nearest palette
run within 64 KiB) or supplied explicitly with `--pal` / `--palIndex`
(sub-palette stride = 16 colors, 256 for 8bpp).

## 4. Palettes (15-bit BGR)

Word layout: `%00000000 bbbbb ggggg rrrrr` — red in bits 0–4, green 5–9,
blue 10–14 (bit 15 = "normal/BGR" flag, always 0 in this ROM).

* Tool conversion (`snesWordToRGBA`): 5→8 bit expansion with bit replication,
  `(v << 3) | (v >> 2)`, packed `R,G,B,A` big-endian.
* The doc previews at epicedit.stifu.fr use a plain `v << 3` shift instead;
  road comparisons in §7 therefore compare *color partitions*, not raw RGB.
* Discovery gate for a palette run: ≥32 consecutive words, all `< 0x8000`,
  >10 distinct values, luminance spread ≥16.

Each `palettes/region_XXXXXX` gets three files: `.png` (16×16 swatch grid),
`.pal` (RGBA8 hex, one color per line), `.hex` (raw little-endian 15-bit
words, 8 per line).

## 5. Mode 7

`--mode7 <mapOffset> [W H] --chars <charOffset> [--charsize 64|256]`

Map entries are 16-bit: bit 15 = H flip, bit 14 = V flip, bit 10–13 =
palette, bits 0–9 = character index (SNES Mode 7 map format). Characters are
8bpp, 64 bytes per tile (8×8 × 1 byte/pixel, chunky).

## 6. SMK compression (verified)

SMK's native codec (a.k.a. Lunar Compress "format 4"): a stream of chunks
terminated by `0xFF`.

```
header byte b:
  b == 0xFF                    -> end of stream (terminator, consumed)
  cmd = b >> 5
  if cmd == 7:                 -> extended form
        cmd    = (b & 0x1C) >> 2          (must be <= 6)
        count10= ((b & 3) << 8) | nextByte
        count  = count10 + 1              (always +1, incl. extended)
  else:  count = (b & 0x1F) + 1
```

| cmd | action |
|---|---|
| 0 | copy `count` literal bytes |
| 1 | repeat next byte `count` times |
| 2 | alternate the next two bytes `count` times (ABAB…) |
| 3 | increasing fill: next byte `v`, emit `v, v+1, v+2, …` |
| 4 | absolute copy: next 2 bytes = little-endian offset into the *output*, copy `count` bytes from there |
| 5 | same as 4, but each copied byte is inverted (`^ 0xFF`) |
| 6 | relative copy: source = `currentOutputOffset - nextByte`, `count` bytes (overlap allowed → run-length) |
| 7 | invalid |

Pointers must reference already-written output. Implemented as
`decompSmk()` in `tools/extract.cpp`; `--decomp OFF -o file.bin` dumps raw
bytes and prints `consumed` + output size, which is how the table in §8 was
verified against the documented compressed lengths.

Discovery scans the whole ROM for such streams (input limit 0x4000, output
limit 0x10000, non-overlapping enumeration) and keeps those whose output looks
image-like (window score ≥0.12, ≥8 distinct bytes), labelling bpp by the best
of the 2/4/8bpp window scores.

### Known compression gaps

* `0x737DA` (podium) decompresses on the first pass to `0x295D` bytes of
  still-compressed data — it is **double compressed**; the second pass is not
  implemented, so `decomp/region_0737DA.png` renders as noise.
* `0x48F6A` **is** a valid stream: it is `DATA_81EBA3[6]`, Bowser Castle's
  mode-7 tile set (138 tiles). It used to be reported as a failure because one
  absolute copy in it points past the output written so far; the original reads
  `$7F` RAM there rather than failing, so the decoder now clamps instead of
  erroring. See `docs/mode7.md`.
* `0x4A000` (documented as Luigi) still fails to decode standalone (`bad command
  / no clean terminator`) at the documented offset and nearby offsets; it is
  left undocumented/unextracted here.

## 7. Road graphics format (resolved)

The road/course graphics are **not** planar tiles. Each road stream
(decompressing at `0x40000`, `0x41EBB`, `0x481C9`, `0x50000`, `0x51636`,
`0x60189`, `0x78000`) is:

* **chunky 4bpp** — 2 pixels per byte, **low nibble = left pixel**, bytes are
  *not* bit-reversed (a bit-reverse variant maps identically for raw indices,
  but only the non-reversed variant reproduces the palette match below);
* laid out as **8×8 tiles, 32 bytes each**;
* tiles are arranged in rows with **stride = 16 tiles (128 pixels)** — i.e.
  each 128-px-wide source row holds 16 tiles, then the next row starts 512
  bytes later;
* the documented preview region starts at **base tile 8 (byte `0x100`)** and
  shows a **4×4-tile (32×32 px) block**;
* the palette is **per tile-row** (one 16-color sub-palette per 8 pixel rows)
  taken from a track palette stream (§8, `0x04117F`…`0x041D0B`), colors
  converted as `5bit << 3`.

### Verified reproductions (exact colour match against the doc PNGs)

| stream | palette stream | sub-palette (word offset) per tile-row | result |
|---|---|---|---|
| `0x040000` road-gfx | `0x04117F` | rows 0–3 = `16, 16, 32, 80` (sub-pals #1,#1,#2,#5) | **exact, all 32×32 pixels** |
| `0x0481C9` road-mc | `0x04117F` | rows 1–3 = `16` (sub-pal #1); row 0 unmatched | 3 of 4 rows exact |
| `0x078000` road-ci | `0x0419C0` | rows 2–3 = `0` (sub-pal #0); rows 0–1 unmatched | 2 of 4 rows exact |
| `0x041EBB` road-rr | — | structure only (source is repetitive) | layout confirmed, palette not reproduced |

The unmatched rows are consistent with the game **generating road palettes at
runtime** (the per-scanline road gradient), so no static ROM window can
reproduce them.

**Specificity control**: pairing each road stream against the *other* tracks'
previews yields **0** exact hits (e.g. road-mc data vs road-g preview: 0;
road-g vs road-rr: 0), while each stream matches its own preview. So the
recipe above is specific, not a coincidence of the flexible per-row palette.

Because the palette varies per row and the tiles are chunky, plain
`--tiles`/`--linear` renders of these streams do not look right; the recipe in
this section is the reference interpretation (the `assets/decomp/`
previews of these offsets are only indicative).

## 8. Confirmed asset map

### 8.1 Compressed streams (all decode cleanly; consumed length = published length)

| offset | SNES | name | consumed | output | contents |
|---|---|---|---|---|---|
| `0x010000` | C1:0000 | lakitu | `0xAA5` | `0x1380` | Lakitu sprites |
| `0x0112F8` | C1:12F8 | race-font | `0x40E` | `0x700` | race HUD font (2bpp) |
| `0x040000` | C4:0000 | road-gfx | `0x594` | `0x900` | road graphics (chunky, §7) |
| `0x040594` | C4:0594 | items | `0xBEB` | `0xF00` | item icons |
| `0x04117F` | C4:117F | track-pal-1 | — | `0x200` | track palette (16 sub-palettes × 16 colors) |
| `0x041313` | C4:1313 | track-pal-2 | — | `0x200` | " |
| `0x0414C4` | C4:14C4 | track-pal-3 | — | `0x200` | " |
| `0x041675` | C4:1675 | track-pal-4 | — | `0x200` | " |
| `0x04182F` | C4:182F | track-pal-5 | — | `0x200` | " |
| `0x0419C0` | C4:19C0 | track-pal-6 | — | `0x200` | " |
| `0x041B5B` | C4:1B5B | track-pal-7 | — | `0x200` | " |
| `0x041D0B` | C4:1D0B | track-pal-8 | — | `0x200` | " |
| `0x041EBB` | C4:1EBB | road-rr | `0x114` | `0x280` | Rainbow Road road graphics |
| `0x0481C9` | C4:81C9 | road-mc | `0xDA1` | `0x1900` | Mario Circuit road graphics |
| `0x050000` | C5:0000 | road-dp | `0xC45` | `0x1100` | Donut Plains road graphics |
| `0x051636` | C5:1636 | road-kb | `0x98D` | `0x1100` | Koopa Beach road graphics |
| `0x060000` | C6:0000 | chain-chomp | `0x189` | `0x280` | Chain Chomp sprite |
| `0x060189` | C6:0189 | road-gv | `0x399` | `0x620` | Ghost Valley road graphics |
| `0x070000` | C7:0000 | fonts | `0xAAC` | `0x1000` | menu/race fonts |
| `0x070B29` | C7:0B29 | select-screen | `0xE53` | `0x4000` | character & cup select screen |
| `0x071996` | C7:1996 | title-results | `0x1E44` | `0x4400` | title screen + GP results |
| `0x0737DA` | C7:37DA | podium | `0x2853` | `0x295D`* | podium / trophy scene |
| `0x078000` | C7:8000 | road-ci | `0xA6B` | `0xD80` | Choco Island road graphics |

\* first pass only — double compressed (§6).

### 8.2 Uncompressed regions

| offset | SNES | length | contents |
|---|---|---|---|
| `0x002000` | C0:2000 | `0x6000` | driver sprite sheet (768 tiles 4bpp), palette `0x001F4C` |
| `0x012000` | C1:2000 | `0x6000` | " (palette `0x011F4C`) |
| `0x022000` | C2:2000 | `0x6000` | " (`0x021F4C`) |
| `0x032000` | C3:2000 | `0x6000` | " (`0x031F4C`) |
| `0x042000` | C4:2000 | `0x6000` | " (`0x041F4C`) |
| `0x052000` | C5:2000 | `0x6000` | " (`0x051F4C`) |
| `0x062000` | C6:2000 | `0x6000` | " (`0x061F4C`) |

Driver palettes sit exactly `0x1F4C` bytes below the start of each bank's
sheet (`(offset & 0xF0000) | 0x1F4C`), 16 colours.

## 9. Validation methodology

Comparing our renders with the doc previews by raw RGB difference is
misleading (the doc uses slightly different colour expansion and different
palettes). The metric used throughout is **colour-class partition equality**:
canonicalise each image's colours by first appearance, then require identical
labels — i.e. "pixel A and pixel B have the same colour in image 1 iff they do
in image 2". This is palette-independent. `tilecmp` additionally compares this
per 8×8 tile, which allows a *per-tile palette remap* (as the game does for
sprites/road rows).

| doc reference | source | position in our render | result |
|---|---|---|---|
| `ref_fonts` | `0x070000` | `(0,0)` | **exact** — 16/16 tiles identical |
| `ref_playsel` | `0x070B29` | `(0,336)` | **exact** — 16/16 tiles identical |
| `ref_road_g` | `0x040000` + `0x04117F` | stride 16, base tile 8 | **exact** (§7) |
| `ref_road_mc` | `0x0481C9` + `0x04117F` | " | 3/4 tile-rows exact |
| `ref_road_ci` | `0x078000` + `0x0419C0` | " | 2/4 tile-rows exact |
| `ref_road_rr` | `0x041EBB` | base tiles 12–15 | layout exact, palette n/a |
| `ref_lakitu` | `0x010000` | `(0,8)` | partial — 8/16 tiles structurally identical |
| `ref_items` | `0x040594` | `(64,16)` | partial — 2/4 tiles structurally identical |
| `ref_rf_itemi` | ? | no window >0.42 anywhere | **unresolved** — not found in any `decomp/` sheet |

Notes:

* Negative controls: cross-pairing road streams/previews (6 combinations)
  always produced 0 hits, so the exact matches are meaningful.
* `ref_lakitu` / `ref_items` line up visually (ASCII structure comparison
  matches row-for-row) but not partition-exactly; the doc previews were most
  likely rendered with the in-game per-tile palette selection, which our flat
  sheets do not apply.

## 10. Discovery scanner behaviour (recall over precision)

* **Raw graphics scan**: whole-ROM planar-tile scan with an 85% zero-byte
  gate plus a window "image-likeness" score; produces `tiles/region_XXXXXX`.
  Alternate bpp renders are emitted as `*_bppN.png`.
* **Palette scan**: gates in §4; produces `palettes/region_XXXXXX.{png,pal,hex}`.
* **Compressed stream scan**: §6; produces `decomp/region_XXXXXX.png`.
* **Curated table** (`kKnown[]` in `tools/extract.cpp`): the 30 regions in §8
  are merged into the scan results. If the heuristic already found the region
  it is annotated (`confirmed` + human name + description); if it was missed
  (12 regions were: the 7 driver sheets + lakitu, road-gfx, road-mc, road-dp,
  chain-chomp) it is decompressed/rendered and added.
* Confidence labels: `high ≥55`, `medium ≥40`, else `low`. Low-confidence
  entries are deliberately kept (they are likely false positives) — treat
  `confirmed` as the trustworthy set.
* `dma-hints.txt` lists every `STA/STX/STY $43xx` (DMA registers) with the
  nearest preceding `LDA/LDX/LDY #imm` — a starting point for mapping transfers
  to the regions above.
