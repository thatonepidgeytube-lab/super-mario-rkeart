// sprite.h -- loads driver/kart sprite sheets from the ROM at runtime.
// Nothing here is embedded in the executable: Nintendo owns the graphics.

#pragma once
#include <cstdint>
#include <vector>
#include <string>

#include "../rom.h"
#include "lz.h"

namespace smk {

// There are 8 drivers but only 7 distinct sprite sheets in the ROM
// (Mario..Koopa at C0:2000 .. C6:2000). Bowser (driver 7) appears to
// share or use a different format; we fall back to Koopa for now.
//
// Each sheet is 0x6000 bytes of 4bpp tiles: 768 tiles of 32 bytes.  Decoding
// the same data as 2bpp produces only stripes, so the 4bpp planar reading is
// the right one (16 bytes of bitplanes 0/1 then 16 of bitplanes 2/3).
inline const size_t kDriverSheetOffsets[7] = {
    0x002000,  // Mario   (C0:2000)
    0x012000,  // Luigi   (C1:2000)
    0x022000,  // Peach   (C2:2000)
    0x032000,  // Daisy   (C3:2000)
    0x042000,  // Yoshi   (C4:2000)
    0x052000,  // Toad    (C5:2000)
    0x062000,  // Koopa   (C6:2000)
};

inline const size_t kDriverPaletteOffsets[7] = {
    0x001F4C,  // Mario
    0x011F4C,  // Luigi
    0x021F4C,  // Peach
    0x031F4C,  // Daisy
    0x041F4C,  // Yoshi
    0x051F4C,  // Toad
    0x061F4C,  // Koopa
};

inline const int kTilesPerSheet = 768;
inline const int kTileSize4bpp = 32;
inline const int kSheetSize = kTilesPerSheet * kTileSize4bpp; // 0x6000

// SNES CGRAM word -> 0xAARRGGBB, expanding 5 bits to 8 the way the PPU does
// (bit replicated, so 0x1F maps to 0xFF).
inline uint32_t bgr555(uint16_t w) {
    uint32_t r = (w >> 10) & 0x1F, g = (w >> 5) & 0x1F, b = w & 0x1F;
    auto x8 = [](uint32_t v) { return (v << 3) | (v >> 2); };
    return 0xFF000000u | (x8(r) << 16) | (x8(g) << 8) | x8(b);
}

// Decode a single 4bpp planar tile (32 bytes) to 8x8 8bpp pixels (64 bytes).
// SNES 4bpp format: 16 bytes for bitplanes 0/1 (low bits), 16 bytes for
// bitplanes 2/3 (high bits). Each 2bpp block is 8 rows of (plane0, plane1)
// pairs, MSB-first.
inline void decode4bppTile(const uint8_t* src, uint8_t* dst) {
    // Plane 0/1 (bits 0,1) -> low nibble
    for (int row = 0; row < 8; row++) {
        uint8_t p0 = src[row * 2];
        uint8_t p1 = src[row * 2 + 1];
        for (int col = 0; col < 8; col++) {
            int bit = 7 - col;
            dst[row * 8 + col] = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1);
        }
    }
    // Plane 2/3 (bits 2,3) -> high nibble
    for (int row = 0; row < 8; row++) {
        uint8_t p2 = src[16 + row * 2];
        uint8_t p3 = src[16 + row * 2 + 1];
        for (int col = 0; col < 8; col++) {
            int bit = 7 - col;
            int hi = (((p2 >> bit) & 1) | (((p3 >> bit) & 1) << 1)) << 2;
            dst[row * 8 + col] |= (uint8_t)hi;
        }
    }
}

// Load a driver's sprite sheet from the ROM.  The four CGRAM colours come from
// the caller (they are part of the course palette stream, not the sheet).
struct DriverSprites {
    std::vector<uint8_t> tiles; // 1536 * 64 bytes, 8bpp linear, values 0..3
    bool ok = false;
    std::string err;
};

inline DriverSprites loadDriverSprites(const Rom& rom, int driver) {
    DriverSprites ds;
    if (driver < 0 || driver >= 8) { ds.err = "bad driver id"; return ds; }
    int sheetIdx = (driver < 7) ? driver : 6; // Bowser -> Koopa fallback
    const std::vector<uint8_t>& d = rom.data;

    size_t sheetOff = kDriverSheetOffsets[sheetIdx];
    if (sheetOff + kSheetSize > d.size()) { ds.err = "sheet past EOF"; return ds; }

    ds.tiles.resize((size_t)kTilesPerSheet * 64);
    for (int t = 0; t < kTilesPerSheet; t++)
        decode4bppTile(&d[sheetOff + (size_t)t * kTileSize4bpp], &ds.tiles[t * 64]);
    ds.ok = true;
    return ds;
}

// A driver's 16 colours come out of the *course* palette stream, which is the
// only thing the game writes to CGRAM ($7E3A80 -> $0000, 0x200 bytes), so
// CGRAM $0180 is word 0xC0 of that stream.  Which sub-block a driver uses is
// worked out from the reference renders (docs/sprites.md): the kart's
// dominant pixel is index 1, so a driver's block is the one whose second word
// is that driver's signature colour.  For Mario that lands on block 0x02,
// whose word 1 is 0x7C00 -- pure red -- and the kart renders red.
inline const uint16_t kDriverSignature[8] = {
    0x7C00, 0x03E0, 0x7BFF, 0x03FF, 0x03E0, 0x7FFF, 0x02A0, 0x7C00,
};

inline int driverPalBlock(const Course& co, int driver) {
    if (driver < 0 || driver >= 8) return 0x02;
    uint16_t want = kDriverSignature[driver];
    for (int b = 0; b + 16 <= 256; b++)
        if (co.pal[b + 1] == want) return b;
    return 0x02;                                   // fall back to Mario's
}

inline uint16_t driverPal(const Course& co, int block, int i) {
    int w = block + (i & 15);
    return (w >= 0 && w < 256) ? co.pal[w] : 0;
}

// Draw a block of tiles as a grid.  `pal` supplies the sixteen CGRAM colours;
// index 0 is transparent, which is how the original hides the sprite's
// corners.
inline void drawKartSprite(uint32_t* fb, int fbW, int fbH,
                           int x, int y, int scale,
                           const DriverSprites& ds, const uint16_t* pal,
                           int tileBase = 0, int cols = 4, int rows = 2,
                           bool hflip = false) {
    if (!ds.ok) return;
    uint32_t rgba[16];
    for (int i = 0; i < 16; i++) rgba[i] = pal[i] ? bgr555(pal[i]) : 0;
    int tileW = 8 * scale, tileH = 8 * scale;
    int left = x - (cols * tileW) / 2, top = y - rows * tileH;
    for (int ty = 0; ty < rows; ty++) {
        for (int tx = 0; tx < cols; tx++) {
            int tileIdx = tileBase + ty * cols + tx;
            if (tileIdx < 0 || tileIdx >= kTilesPerSheet) continue;
            const uint8_t* tile = &ds.tiles[(size_t)tileIdx * 64];
            for (int dy = 0; dy < tileH; dy++) {
                int sy = dy / scale; if (sy > 7) sy = 7;
                for (int dx = 0; dx < tileW; dx++) {
                    int sx = hflip ? 7 - dx / scale : dx / scale;
                    uint8_t pix = tile[sy * 8 + sx];
                    if (pix == 0) continue;              // transparent
                    int fx = left + tx * tileW + dx, fy = top + ty * tileH + dy;
                    if (fx >= 0 && fx < fbW && fy >= 0 && fy < fbH)
                        fb[fy * fbW + fx] = rgba[pix & 15];
                }
            }
        }
    }
}

// One 2bpp tile: 8 rows of (plane0, plane1), MSB = leftmost pixel.
inline void decode2bppTile(const uint8_t* src, uint8_t* dst) {
    for (int row = 0; row < 8; row++) {
        uint8_t p0 = src[row * 2], p1 = src[row * 2 + 1];
        for (int col = 0; col < 8; col++) {
            int bit = 7 - col;
            dst[row * 8 + col] = (uint8_t)(((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1));
        }
    }
}

// SMK_GFX_TitleScreen ($C71996 -> file 0x71996, loaded to $7F0000 by
// CODE_81E10A) decompresses to 0x4400 bytes, which splits cleanly as a 32x32
// background of 8x8 tiles: 1024 bytes of tilemap, then 15360 bytes of 2bpp
// tile data (960 tiles).
static const size_t OFF_TITLE_SCREEN = 0x71996;
static const int kTitleMapSide  = 32;
static const int kTitleMapBytes = kTitleMapSide * kTitleMapSide;
static const int kTitlePixels   = kTitleMapSide * 8;     // 256 x 256

struct TitleScreen {
    std::vector<uint8_t> map;                            // tile numbers
    std::vector<uint8_t> tiles;                          // n * 64, 2bpp indices
    int tileCount = 0;
    bool ok = false;
    std::string err;
};

inline TitleScreen loadTitleScreen(const Rom& rom) {
    TitleScreen ts;
    const std::vector<uint8_t>& d = rom.data;
    Decomp t = decompSmk(d, OFF_TITLE_SCREEN, 0x8000);
    if (!t.ok) { ts.err = "title screen stream did not decompress"; return ts; }
    if (t.out.size() < (size_t)kTitleMapBytes + 16) { ts.err = "title screen too small"; return ts; }
    ts.map.assign(t.out.begin(), t.out.begin() + kTitleMapBytes);
    const uint8_t* tp = t.out.data() + kTitleMapBytes;
    ts.tileCount = (int)((t.out.size() - kTitleMapBytes) / 16);
    ts.tiles.resize((size_t)ts.tileCount * 64);
    for (int i = 0; i < ts.tileCount; i++)
        decode2bppTile(tp + (size_t)i * 16, &ts.tiles[(size_t)i * 64]);
    ts.ok = true;
    return ts;
}

} // namespace smk