// palscan.cpp -- locate the per-racer CGRAM palette block in the ROM.
//
// The eight racers each own four CGRAM colours (DATA_80847E: $0180,$0184,
// $0188,$018C then $01C0..$01CC, i.e. palettes 6..9 and 14..17).  That means
// the game stores them as eight 4-word groups, so search for a 32-word run
// whose groups all look like character palettes: each has one saturated
// primary (hat/shirt) and one bright or near-white highlight, and the eight
// groups are pairwise different.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/rom.h"

static void rgb(uint16_t w, int& r, int& g, int& b) {
    r = (w >> 10) & 31; g = (w >> 5) & 31; b = w & 31;
}

// true if any of the 4 words is a saturated primary colour
static bool hasPrimary(const uint16_t* p) {
    for (int i = 0; i < 4; i++) {
        int r, g, b; rgb(p[i], r, g, b);
        if (r >= 22 && g <= 6  && b <= 6 ) return true;   // red
        if (g >= 22 && r <= 6  && b <= 6 ) return true;   // green
        if (b >= 22 && r <= 6  && g <= 12) return true;   // blue
        if (r >= 20 && g >= 18 && b <= 10) return true;   // yellow/orange
    }
    return false;
}

// true if any word is bright (highlight, skin, white) or a dark outline
static bool hasShade(const uint16_t* p) {
    for (int i = 0; i < 4; i++) {
        int r, g, b; rgb(p[i], r, g, b);
        if (r >= 20 && g >= 20 && b >= 20) return true;   // white
        if (r <= 2  && g <= 2  && b <= 2 ) return true;   // black outline
        if (r >= 18 && g >= 12 && b >= 8 && b < r) return true;  // skin tone
    }
    return false;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "Super Mario Kart (USA).sfc";
    Rom rom;
    if (!rom.load(path)) { std::fprintf(stderr, "cannot load %s\n", path); return 1; }
    const std::vector<uint8_t>& d = rom.data;

    std::printf("scanning %zu bytes for an 8x4 racer palette block...\n", d.size());
    int found = 0;
    for (size_t o = 0; o + 64 <= d.size(); o += 2) {
        bool ok = true;
        for (int k = 0; k < 8 && ok; k++) {
            uint16_t p[4];
            for (int i = 0; i < 4; i++)
                p[i] = (uint16_t)(d[o + (k * 4 + i) * 2] | (d[o + (k * 4 + i) * 2 + 1] << 8));
            if (!hasPrimary(p) || !hasShade(p)) ok = false;
        }
        if (!ok) continue;
        // the eight palettes should not be identical copies
        bool dup = false;
        for (int a = 0; a < 8 && !dup; a++)
            for (int b = a + 1; b < 8 && !dup; b++) {
                uint16_t pa[4], pb[4];
                for (int i = 0; i < 4; i++) {
                    pa[i] = (uint16_t)(d[o + (a * 4 + i) * 2] | (d[o + (a * 4 + i) * 2 + 1] << 8));
                    pb[i] = (uint16_t)(d[o + (b * 4 + i) * 2] | (d[o + (b * 4 + i) * 2 + 1] << 8));
                }
                if (!std::memcmp(pa, pb, sizeof(pa))) dup = true;
            }
        if (dup) continue;

        std::printf("\nblock at file offset 0x%06zX:\n", o);
        for (int k = 0; k < 8; k++) {
            std::printf("  racer %d:", k);
            for (int i = 0; i < 4; i++) {
                uint16_t w = (uint16_t)(d[o + (k * 4 + i) * 2] | (d[o + (k * 4 + i) * 2 + 1] << 8));
                int r, g, b; rgb(w, r, g, b);
                std::printf("  %04X(%02d,%02d,%02d)", w, r, g, b);
            }
            std::printf("\n");
        }
        if (++found >= 12) break;
    }
    if (!found) std::printf("no block matched\n");
    return 0;
}
