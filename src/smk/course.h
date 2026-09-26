// course.h -- loads one racetrack's mode-7 assets out of the user's ROM at
// runtime.  Nothing here is embedded in the executable: Nintendo owns the
// graphics, so the player must supply their own Super Mario Kart (USA) image.
//
// Pipeline, decoded from the game's own loader (bank $81, DBR=$81):
//   CODE_81E745  level data  DATA_81EB5B[id]   -> decompress -> $7FC000
//                                                 decompress -> $7F0000 (map)
//   CODE_81E6D4  mode-7 tiles DATA_81EBA3[type] -> decompress -> $7FC000
//                                                 CODE_84E378  -> $7F4000
//   CODE_81E6B9  shared tiles $C40000           -> decompress -> $7F0000
//                                                 CODE_84E378  -> $7F7000
//   CODE_81E72E  palette     DATA_81EBBB[type]  -> decompress -> $7E3A80
//   CODE_81E769  DMA $7F4000 -> VRAM $0000  (192 tiles, high bytes)
//   CODE_81E68E  DMA $7F7000 -> VRAM $3000  ( 64 tiles, high bytes) = tiles 192..255
//   CODE_81E7B5  DMA $7F0000 -> VRAM $0000  (0x4000 bytes, low bytes = map)
//
// CODE_84E378 converts 4bpp source into the 8bpp pixels mode 7 wants:
// for each tile: attribute byte = src[i], pixels come from the low/high
// nibbles of 32 source bytes at src + 0x100 + i*32, and a non-zero nibble
// becomes (nibble | attribute) -- attribute is the palette base.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../rom.h"
#include "lz.h"

namespace smk {

// ------------------------------------------------------------- SNES pointers
inline size_t hiromOffset(uint32_t ptr) {          // 24-bit SNES ptr -> file off
    uint8_t  bank = (uint8_t)(ptr >> 16);
    uint32_t addr = ptr & 0xFFFF;
    return (size_t)(((uint32_t)(bank & 0x3F) << 16) | (addr & 0xFFFF));
}

inline uint32_t rd24(const std::vector<uint8_t>& d, size_t o) {
    if (o + 3 > d.size()) return 0;
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) | ((uint32_t)d[o + 2] << 16);
}
inline uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    if (o + 2 > d.size()) return 0;
    return (uint16_t)((uint16_t)d[o] | ((uint16_t)d[o + 1] << 8));
}

// ------------------------------------------------------------------ tables
// Addresses taken from the labelled disassembly (Yoshifanatic1 oracle) and
// cross-checked against the bytes in this ROM.
static const size_t TBL_LEVEL_DATA = 0x1EB5B;   // DATA_81EB5B, 24 x 3-byte ptrs
static const size_t TBL_M7_TILES   = 0x1EBA3;   // DATA_81EBA3,  8 x 3-byte ptrs
static const size_t TBL_PALETTE    = 0x1EBBB;   // DATA_81EBBB,  8 x 3-byte ptrs
static const size_t TBL_COURSE_ID  = 0x1EC1B;   // DATA_81EC1B, 20 x RacetrackID
static const size_t TBL_COURSE_TYPE= 0x1EC2F;   // DATA_81EC2F, 20 x RacetrackType
static const size_t OFF_GLOBAL_TILES = 0x40000; // SMK_GFX_Mode7_GlobalTrackTiles
static const int    kNumTypedCourses  = 20;     // the 4 battle courses aren't listed

static const int kNumCourses = 24;
static const int kNumFamilies = 8;

static const char* kCourseNames[kNumCourses] = {
    "MARIO CIRCUIT 3", "GHOST VALLEY 2", "DONUT PLAINS 2", "BOWSER CASTLE 2",
    "VANILLA LAKE 2",  "RAINBOW ROAD",   "KOOPA BEACH 2",  "MARIO CIRCUIT 1",
    "GHOST VALLEY 3",  "BOWSER CASTLE 3","CHOCO ISLAND 2", "DONUT PLAINS 3",
    "VANILLA LAKE 1",  "KOOPA BEACH 1",  "MARIO CIRCUIT 4","MARIO CIRCUIT 2",
    "GHOST VALLEY 1",  "BOWSER CASTLE 1","CHOCO ISLAND 1", "DONUT PLAINS 1",
    "BATTLE COURSE 3", "BATTLE COURSE 4","BATTLE COURSE 1","BATTLE COURSE 2",
};

// which of the 8 graphics families each course uses (DATA_81EBA3 order).
// Read from the ROM at runtime by familyForCourse() below; this copy only
// serves as a fallback and documents what the ROM contains.
static const int kCourseFamily[kNumCourses] = {
    1, 0, 2, 6, 4, 7, 5, 1, 0, 6, 3, 2,
    4, 5, 1, 1, 0, 6, 3, 2, -1, -1, -1, -1,
};

// DATA_81EC2F holds one RacetrackType byte per race course; CODE_81E84C turns
// it into a table index with x = type + (type >> 1) = type * 3/2, so the
// graphics family is simply type / 2.
inline int familyForCourse(const std::vector<uint8_t>& d, int id) {
    if (id >= 0 && id < kNumTypedCourses && TBL_COURSE_TYPE + (size_t)id < d.size()) {
        uint8_t t = d[TBL_COURSE_TYPE + (size_t)id];
        if ((t & 1) == 0 && (t / 2) < kNumFamilies) return t / 2;
    }
    return (id >= 0 && id < kNumCourses) ? kCourseFamily[id] : -1;
}

static const char* kFamilyNames[kNumFamilies] = {
    "GHOST VALLEY", "MARIO CIRCUIT", "DONUT PLAINS", "CHOCO ISLAND",
    "VANILLA LAKE", "KOOPA BEACH", "BOWSER CASTLE", "RAINBOW ROAD",
};

static const char* kCharNames[8] = {
    "MARIO", "LUIGI", "PEACH", "DAISY", "YOSHI", "TOAD", "KOOPA", "BOWSER",
};

// ------------------------------------------------------------------ course
struct Course {
    std::vector<uint8_t> map;     // 128 x 128 tile numbers (0x4000 bytes)
    std::vector<uint8_t> tiles;   // 256 tiles x 64 bytes, 8bpp
    uint16_t pal[256];            // BGR555, CGRAM order
    int      id = -1, family = -1;
    bool     ok = false;
    std::string err;
};

// port of CODE_84E378 -- 4bpp source + palette attributes -> 8bpp tiles
// Source layout: attr bytes at [0..count), then tile pixels at 0x100 + i*32
// (the routine computes that address with five ASLs, i.e. i*32).
inline void expandMode7Tiles(const std::vector<uint8_t>& s, int count, uint8_t* dst) {
    for (int i = 0; i < count; i++) {
        uint8_t attr = (i < (int)s.size()) ? s[i] : 0;
        size_t base = 0x100 + (size_t)i * 32;
        for (int k = 0; k < 32; k++) {                  // 32 bytes = 64 pixels
            uint8_t b  = (base + k < s.size()) ? s[base + k] : 0;
            uint8_t lo = (uint8_t)(b & 0x0F), hi = (uint8_t)(b >> 4);
            dst[i * 64 + k * 2]     = lo ? (uint8_t)(lo | attr) : 0;
            dst[i * 64 + k * 2 + 1] = hi ? (uint8_t)(hi | attr) : 0;
        }
    }
}

inline Course loadCourse(const Rom& rom, int id) {
    Course c;
    c.id = id;
    if (id < 0 || id >= kNumCourses) { c.err = "bad course id"; return c; }
    const std::vector<uint8_t>& d = rom.data;
    int family = familyForCourse(d, id);
    c.family = family;
    if (family < 0) { c.err = "battle course: no mode-7 family mapped yet"; return c; }

    // ---- mode-7 tile set for this graphics family (tiles 0..191)
    std::vector<uint8_t> tiles(256 * 64, 0);
    if (family < kNumFamilies) {
        uint32_t pTiles = rd24(d, TBL_M7_TILES + (size_t)family * 3);
        smk::Decomp t = smk::decompSmk(d, hiromOffset(pTiles), 0x20000);
        if (t.ok) expandMode7Tiles(t.out, 192, tiles.data());
        else c.err = "mode-7 tile stream did not decompress";
    }

    // ---- 64 shared tiles (tiles 192..255)
    {
        smk::Decomp g = smk::decompSmk(d, OFF_GLOBAL_TILES, 0x20000);
        if (g.ok) expandMode7Tiles(g.out, 64, tiles.data() + 192 * 64);
        else c.err += "; shared tile stream did not decompress";
    }
    c.tiles = std::move(tiles);

    // ---- level data -> nested stream -> 128x128 tilemap
    {
        uint32_t pLevel = rd24(d, TBL_LEVEL_DATA + (size_t)id * 3);
        smk::Decomp outer = smk::decompSmk(d, hiromOffset(pLevel), 0x80000);
        if (!outer.ok) { c.err = "level data did not decompress"; return c; }
        smk::Decomp inner = smk::decompSmk(outer.out, 0, 0x20000);
        if (!inner.ok || inner.out.size() < 0x4000) {
            c.err = "level tilemap did not decompress (got " +
                    std::to_string(inner.out.size()) + " bytes)";
            return c;
        }
        c.map.assign(inner.out.begin(), inner.out.begin() + 0x4000);
    }

    // ---- course palette
    for (int i = 0; i < 256; i++) c.pal[i] = 0;
    {
        uint32_t pPal = rd24(d, TBL_PALETTE + (size_t)family * 3);
        smk::Decomp p = smk::decompSmk(d, hiromOffset(pPal), 0x4000);
        size_t words = p.ok ? p.out.size() / 2 : 0;
        if (words > 256) words = 256;
        for (size_t i = 0; i < words; i++)
            c.pal[i] = (uint16_t)(p.out[i * 2] | (p.out[i * 2 + 1] << 8));
    }

    c.ok = true;
    return c;
}

} // namespace smk
