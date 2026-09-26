// tilehist.cpp -- histogram the 4bpp pixel values inside a tile range of a
// driver sprite sheet, so the right 16-colour CGRAM block can be identified
// (Mario's kart should be dominated by his red, index 1).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/rom.h"

int main(int argc, char** argv) {
    const char* path = "Super Mario Kart (USA).sfc";
    size_t sheet = 0x002000, first = 0, count = 768;
    if (argc > 1) path = argv[1];
    if (argc > 2) sheet = (size_t)std::strtoul(argv[2], nullptr, 0);
    if (argc > 3) first = (size_t)std::strtoul(argv[3], nullptr, 0);
    if (argc > 4) count = (size_t)std::strtoul(argv[4], nullptr, 0);

    Rom rom;
    if (!rom.load(path)) { std::fprintf(stderr, "cannot load %s\n", path); return 1; }
    const std::vector<uint8_t>& d = rom.data;
    if (sheet + first * 32 + count * 32 > d.size()) { std::fprintf(stderr, "range past EOF\n"); return 1; }

    long hist[16] = {};
    for (size_t t = first; t < first + count; t++) {
        const uint8_t* s = &d[sheet + t * 32];
        for (int row = 0; row < 8; row++) {
            uint8_t p0 = s[row * 2], p1 = s[row * 2 + 1];
            uint8_t p2 = s[16 + row * 2], p3 = s[16 + row * 2 + 1];
            for (int bit = 7; bit >= 0; bit--) {
                int v = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1)
                      | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
                hist[v]++;
            }
        }
    }
    long total = 0;
    for (int i = 0; i < 16; i++) total += hist[i];
    std::printf("tiles %zu..%zu (%zu) of sheet 0x%zX, %ld pixels\n",
                first, first + count - 1, count, sheet, total);
    for (int i = 0; i < 16; i++)
        std::printf("  index %2d : %6ld  %5.1f%%  %s\n", i, hist[i],
                    100.0 * hist[i] / (double)(total ? total : 1),
                    "################################################"[0] ? "" : "");
    return 0;
}
