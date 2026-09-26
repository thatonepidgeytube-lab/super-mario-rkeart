// pngprobe.cpp -- read a PNG and print what came out, so the decoder can be
// checked against the reference renders before the game depends on it.
#include <cstdio>
#include <string>

#include "../src/png_read.h"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: pngprobe FILE.png\n"); return 2; }
    pngread::Image img = pngread::read(argv[1]);
    if (!img.ok) { std::fprintf(stderr, "FAIL: %s\n", img.err.c_str()); return 1; }

    // count distinct colours and the top few, plus a couple of probe pixels
    const size_t kSlots = 1u << 24;
    std::vector<long> counts(kSlots, 0);
    long total = 0;
    for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
        uint32_t c = ((uint32_t)img.rgba[i*4] << 16) | ((uint32_t)img.rgba[i*4+1] << 8) | img.rgba[i*4+2];
        counts[c]++;
        total++;
    }
    int distinct = 0;
    for (size_t i = 0; i < kSlots; i++) if (counts[i]) distinct++;
    std::printf("%s: %dx%d, %ld pixels, %d distinct colours\n",
                argv[1], img.w, img.h, total, distinct);

    // top 8 colours
    std::vector<std::pair<long, uint32_t>> top;
    for (size_t i = 0; i < kSlots; i++) if (counts[i]) top.push_back({counts[i], (uint32_t)i});
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::printf("  top colours:");
    for (size_t i = 0; i < top.size() && i < 8; i++)
        std::printf(" %06X(x%ld)", top[i].second, top[i].first);
    std::printf("\n");
    std::printf("  corners: TL=%02X%02X%02X TR=%02X%02X%02X BL=%02X%02X%02X\n",
        img.rgba[0], img.rgba[1], img.rgba[2],
        img.rgba[(size_t)(img.w-1)*4+0], img.rgba[(size_t)(img.w-1)*4+1], img.rgba[(size_t)(img.w-1)*4+2],
        img.rgba[(size_t)(img.h-1)*img.w*4+0], img.rgba[(size_t)(img.h-1)*img.w*4+1], img.rgba[(size_t)(img.h-1)*img.w*4+2]);
    return 0;
}
