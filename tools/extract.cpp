// extract.cpp -- Super Mario Kart (USA) asset extractor.
//
// Three ways to use it:
//
//   1. Discovery (default when no extraction mode is given):
//        smk-extract "<rom>" -o assets
//      Scans the whole ROM for graphics-like and palette-like byte runs,
//      renders everything it finds into assets/tiles/*.png and
//      assets/palettes/*.png (+ .pal/.hex), and writes an index documenting
//      file offset, SNES bank:addr, length, bpp guess and confidence.
//      False positives are deliberately kept and labelled low-confidence:
//      recall matters more than precision at this stage.
//
//   2. Manual extraction (for iterating once an offset is known):
//        smk-extract "<rom>" --tiles OFF --bpp 4 --pal POFF --palIndex N \
//                --count TILES --cols 16 -o out.png
//        smk-extract "<rom>" --pal POFF --count COLORS -o out.png
//        smk-extract "<rom>" --mode7 OFF [W H] --chars CHROFF -o out.png
//        smk-extract "<rom>" --chars8 OFF [COUNT] -o out.png
//
//   3. --scan forces discovery even when other switches are present.
//
// Numbers (offsets, counts, sizes) accept decimal or 0x-prefixed hex.
//
// Format rules implemented here (see docs/assets.md):
//   * 2bpp tile  = 16 bytes, rows stored as (plane0 byte, plane1 byte).
//   * 4bpp tile  = 32 bytes = two 2bpp blocks: bytes 0-15 -> bits 0/1,
//                  bytes 16-31 -> bits 2/3.
//   * 8bpp tile  = 64 bytes = four 2bpp blocks, 16 bytes per plane pair.
//   * pixel bits are MSB-first (left pixel = bit 7).
//   * palette entries are little-endian 15-bit words 0bbbbbgggggrrrrr;
//                  bit 15 (EXG1 transparency) is ignored, colors are
//                  bit-expanded 5->8 to RGBA8.
//   * Mode 7: map is one byte per tile (tile index), tile pixels are linear
//                  8x8x8bpp (64 bytes per tile) starting at the charset base.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <sys/stat.h>
static int _mkdir(const char* p) { return mkdir(p, 0755); }
#endif

#include "../src/rom.h"
#include "../src/png.h"

// ------------------------------------------------------------------ helpers

static void makeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        char c = (i < path.size()) ? path[i] : '\0';
        if (c == '/' || c == '\\' || c == '\0') {
            bool driveOnly = (cur.size() == 2 && cur[1] == ':');
            if (!cur.empty() && !driveOnly) _mkdir(cur.c_str());  // best effort, EEXIST ok
            if (i < path.size()) cur.push_back('/');
        } else {
            cur.push_back(c);
        }
    }
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

static std::string dirPart(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    if (s == std::string::npos) return std::string();
    return p.substr(0, s);
}

static std::string stripExt(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    size_t d = p.find_last_of('.');
    if (d != std::string::npos && (s == std::string::npos || d > s) && d > 0) return p.substr(0, d);
    return p;
}

static int64_t parseNum(const std::string& s, const char* what) {
    char* end = nullptr;
    long long v = std::strtoll(s.c_str(), &end, 0);
    if (end == s.c_str() || (end && *end != '\0')) {
        std::fprintf(stderr, "error: bad number '%s' for %s\n", s.c_str(), what);
        std::exit(2);
    }
    return (int64_t)v;
}

// ---------------------------------------------------------------- palettes

static uint32_t expand5(int v) {
    v &= 0x1F;
    return (uint32_t)((v << 3) | (v >> 2));
}

static uint32_t snesWordToRGBA(uint16_t w) {
    uint32_t r = expand5(w & 0x1F);
    uint32_t g = expand5((w >> 5) & 0x1F);
    uint32_t b = expand5((w >> 10) & 0x1F);
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;   // packed R,G,B,A big-endian
}

static uint16_t rd16(const Rom& rom, int64_t off) {
    if (off < 0 || off + 1 >= (int64_t)rom.data.size()) return 0;
    return (uint16_t)(rom.data[(size_t)off] | (rom.data[(size_t)off + 1] << 8));
}

static std::vector<uint16_t> readPalWords(const Rom& rom, int64_t off, size_t count) {
    std::vector<uint16_t> words;
    words.reserve(count);
    for (size_t i = 0; i < count; i++) {
        int64_t o = off + (int64_t)i * 2;
        if (o < 0 || o + 1 >= (int64_t)rom.data.size()) break;
        words.push_back(rd16(rom, o));
    }
    return words;
}

// Deterministic fallback palette (index 0 = black, then a 6-step hue wheel)
// used when no ROM palette is available, so shapes are still recognizable.
static uint32_t fallbackRGBA(int i) {
    if (i <= 0) return 0x000000FFu;
    int seg = ((i - 1) / 6) % 6;
    int t   = (((i - 1) % 6) * 40 + 55) & 0xFF;   // 55..255
    uint8_t r = 0, g = 0, b = 0;
    switch (seg) {
        case 0: r = 255; g = (uint8_t)t; break;
        case 1: r = (uint8_t)(255 - t); g = 255; break;
        case 2: g = 255; b = (uint8_t)t; break;
        case 3: g = (uint8_t)(255 - t); b = 255; break;
        case 4: r = (uint8_t)t; b = 255; break;
        default: r = 255; b = (uint8_t)(255 - t); break;
    }
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | 0xFFu;
}

// Extend a palette to at least n colors (falling back past its end).
static std::vector<uint32_t> extendPal(const std::vector<uint32_t>& pal, size_t n) {
    std::vector<uint32_t> out = pal;
    while (out.size() < n) out.push_back(fallbackRGBA((int)out.size()));
    return out;
}

static bool writePalFiles(const std::string& base,
                          const std::vector<uint16_t>& words,
                          const std::vector<uint32_t>& rgba) {
    bool ok = true;
    FILE* f = std::fopen((base + ".pal").c_str(), "w");
    if (f) {                                   // RGBA8 hex, one color per line
        for (size_t i = 0; i < rgba.size(); i++) {
            std::fprintf(f, "%02X%02X%02X%02X\n",
                         (rgba[i] >> 24) & 0xFF, (rgba[i] >> 16) & 0xFF,
                         (rgba[i] >> 8) & 0xFF, rgba[i] & 0xFF);
        }
        std::fclose(f);
    } else ok = false;

    f = std::fopen((base + ".hex").c_str(), "w");  // raw SNES 15-bit words
    if (f) {
        for (size_t i = 0; i < words.size(); i++) {
            std::fprintf(f, "%04X%s", words[i], ((i + 1) % 8 == 0 || i + 1 == words.size()) ? "\n" : " ");
        }
        std::fclose(f);
    } else ok = false;
    return ok;
}

static bool writePaletteSwatch(const std::string& path,
                               const std::vector<uint16_t>& words,
                               const std::vector<uint32_t>& rgba,
                               int scale = 1) {
    if (words.empty()) return false;
    const int cols = 16, cell = 16;
    int rows = (int)((words.size() + cols - 1) / cols);
    int bw = cols * cell, bh = rows * cell;
    if (scale < 1) scale = 1;
    std::vector<uint8_t> base((size_t)bw * bh * 4, 0);
    for (size_t i = 0; i < words.size(); i++) {
        int cx = (int)(i % cols), cy = (int)(i / cols);
        uint32_t c = i < rgba.size() ? rgba[i] : snesWordToRGBA(words[i]);
        for (int y = 0; y < cell; y++) {
            for (int x = 0; x < cell; x++) {
                size_t o = ((size_t)(cy * cell + y) * (cols * cell) + cx * cell + x) * 4;
                base[o]     = (uint8_t)(c >> 24);
                base[o + 1] = (uint8_t)(c >> 16);
                base[o + 2] = (uint8_t)(c >> 8);
                base[o + 3] = 0xFF;
            }
        }
    }
    std::vector<uint8_t> scaled;
    const std::vector<uint8_t>* use = &base;
    int ow = bw, oh = bh;
    if (scale > 1) {
        ow = bw * scale;
        oh = bh * scale;
        scaled.resize((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) {
            for (int x = 0; x < ow; x++) {
                size_t s = ((size_t)(y / scale) * bw + x / scale) * 4;
                size_t d = ((size_t)y * ow + x) * 4;
                for (int k = 0; k < 4; k++) scaled[d + k] = base[s + k];
            }
        }
        use = &scaled;
    }
    return png::write(path, ow, oh, *use);
}

// ------------------------------------------------------------- tile decode

// Byte-level reordering used by SMK's "reversed" road/tile sheets: the raw
// tile bytes arrive in a different order than plain planar decode expects.
//   rev 0: none (plain planar)
//   rev 1: reverse bits in every byte (LSB-first planes)
//   rev 2: reverse byte order inside each 16-byte plane pair
//   rev 3: reverse byte order of the whole tile
//   rev 4: reverse the order of the plane-pair blocks
//   rev 5: rev 1 + rev 2
//   rev 6: reverse the order of the 8 pixel rows inside each tile
//   rev 7: planes stored sequentially (bytes 0-7 = plane 0, 8-15 = plane 1, ...)
//   rev 8: 4 bytes per row (planes 0,1,2,3 adjacent) instead of 2+2 pairs
static void reorderTile(const uint8_t* t, int bpp, int rev, uint8_t* out) {
    int tb = bpp * 8;
    for (int i = 0; i < tb; i++) out[i] = t[i];
    if (rev == 0) return;
    if (rev == 8) {                              // interleaved rows: p0 p1 p2 p3 per row
        uint8_t src[64];
        for (int i = 0; i < tb; i++) src[i] = t[i];
        int pairs = bpp / 2;
        for (int p = 0; p < pairs; p++)
            for (int y = 0; y < 8; y++) {
                out[p * 16 + y * 2]     = src[y * (bpp / 2) * 2 + p * 2];
                out[p * 16 + y * 2 + 1] = src[y * (bpp / 2) * 2 + p * 2 + 1];
            }
        return;
    }
    if (rev == 7) {                              // sequential planes -> planar pairs
        uint8_t src[64];
        for (int i = 0; i < tb; i++) src[i] = t[i];
        int pairs = bpp / 2;
        for (int p = 0; p < pairs; p++)
            for (int y = 0; y < 8; y++) {
                out[p * 16 + y * 2]     = src[(p * 2) * 8 + y];
                out[p * 16 + y * 2 + 1] = src[(p * 2 + 1) * 8 + y];
            }
        return;
    }
    if (rev == 1 || rev == 5) {
        for (int i = 0; i < tb; i++) {
            uint8_t v = out[i];
            v = (uint8_t)(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
            v = (uint8_t)(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
            out[i] = (uint8_t)((v >> 4) | (v << 4));
        }
        if (rev == 1) return;
        // fall through to also apply rev 2 (after bit swap, use a copy base)
    }
    if (rev == 2 || rev == 5) {
        int pairs = bpp / 2;
        for (int p = 0; p < pairs; p++) {
            uint8_t blk[16];
            for (int i = 0; i < 16; i++) blk[i] = out[p * 16 + i];
            for (int i = 0; i < 16; i++) out[p * 16 + i] = blk[15 - i];
        }
        if (rev == 2) return;
    }
    if (rev == 3) {
        for (int i = 0, j = tb - 1; i < j; i++, j--) { uint8_t v = out[i]; out[i] = out[j]; out[j] = v; }
    } else if (rev == 6) {
        int pairs = bpp / 2;
        for (int p = 0; p < pairs; p++) {
            uint8_t blk[16];
            for (int i = 0; i < 16; i++) blk[i] = out[p * 16 + i];
            for (int y = 0; y < 4; y++)
                for (int k = 0; k < 2; k++) out[p * 16 + y * 2 + k] = blk[(7 - y) * 2 + k];
        }
    } else if (rev == 4) {
        int pairs = bpp / 2;
        uint8_t tmp[64];
        for (int p = 0; p < pairs; p++)
            for (int i = 0; i < 16; i++) tmp[p * 16 + i] = out[(pairs - 1 - p) * 16 + i];
        for (int i = 0; i < tb; i++) out[i] = tmp[i];
    }
}

static void decodeTile(const uint8_t* t, int bpp, uint8_t* px /*64*/, int rev = 0) {
    for (int i = 0; i < 64; i++) px[i] = 0;
    uint8_t buf[64];
    if (rev != 0) { reorderTile(t, bpp, rev, buf); t = buf; }
    int pairs = bpp / 2;
    for (int p = 0; p < pairs; p++) {
        const uint8_t* blk = t + p * 16;         // 16 bytes per plane pair
        if (!blk) continue;
        for (int y = 0; y < 8; y++) {
            uint8_t p0 = blk[y * 2];
            uint8_t p1 = blk[y * 2 + 1];
            for (int x = 0; x < 8; x++) {
                int bit = ((p0 >> (7 - x)) & 1) | (((p1 >> (7 - x)) & 1) << 1);
                px[y * 8 + x] |= (uint8_t)(bit << (p * 2));
            }
        }
    }
}

struct Sheet {
    int w = 0, h = 0;
    std::vector<uint8_t> idx;                   // color index per pixel
    size_t tiles = 0;
};

// Linear (chunky) bitmap: `width` pixels per row, `bpp` bits per pixel,
// MSB-first. Used for sheets stored as raw bitmaps instead of 8x8 tiles.
static Sheet buildLinear(const uint8_t* data, size_t len, int bpp, int width) {
    Sheet s;
    if (width <= 0 || (bpp != 2 && bpp != 4 && bpp != 8)) return s;
    int rowBytes = (width * bpp) / 8;
    if (rowBytes <= 0) return s;
    int rows = (int)(len / (size_t)rowBytes);
    if (rows <= 0) return s;
    s.w = width;
    s.h = rows;
    s.tiles = (size_t)rows;
    s.idx.assign((size_t)width * rows, 0);
    for (int y = 0; y < rows; y++) {
        const uint8_t* line = data + (size_t)y * rowBytes;
        for (int x = 0; x < width; x++) {
            size_t bit = (size_t)x * bpp;
            size_t byte = bit >> 3;
            int off = (int)(bit & 7);
            uint32_t v;
            if (off + bpp <= 8) {
                v = (line[byte] >> (8 - off - bpp)) & ((1u << bpp) - 1);
            } else {
                v = (((uint32_t)line[byte] << 8) | line[byte + 1]) >> (16 - off - bpp);
                v &= (1u << bpp) - 1;
            }
            s.idx[(size_t)y * width + x] = (uint8_t)v;
        }
    }
    return s;
}

static Sheet buildSheet(const uint8_t* data, size_t len, int bpp, int cols, int rev = 0) {
    Sheet s;
    int tb = 8 * bpp;                           // 16 / 32 / 64 bytes per tile
    size_t nt = len / (size_t)tb;
    if (nt == 0 || cols <= 0) return s;
    int rows = (int)((nt + (size_t)cols - 1) / (size_t)cols);
    s.w = cols * 8;
    s.h = rows * 8;
    s.tiles = nt;
    s.idx.assign((size_t)s.w * s.h, 0);
    std::vector<uint8_t> px(64);
    for (size_t t = 0; t < nt; t++) {
        decodeTile(data + t * (size_t)tb, bpp, px.data(), rev);
        int tx = (int)(t % (size_t)cols);
        int ty = (int)(t / (size_t)cols);
        for (int y = 0; y < 8; y++) {
            uint8_t* row = &s.idx[((size_t)(ty * 8 + y)) * s.w + (size_t)(tx * 8)];
            for (int x = 0; x < 8; x++) row[x] = px[y * 8 + x];
        }
    }
    return s;
}

// Fraction of horizontally/vertically adjacent pixel pairs that are equal,
// corrected by the chance agreement implied by the value histogram.
// ~0 = noise/code/compressed data, ~0.3+ = image-like data.
static double adjEqualScore(const Sheet& s) {
    int w = s.w, h = s.h;
    if (w < 2 || h < 2 || s.idx.empty()) return 0.0;
    long hist[256] = {0};
    long n = (long)w * h;
    for (long i = 0; i < n; i++) hist[s.idx[(size_t)i]]++;
    double chance = 0.0;
    for (int v = 0; v < 256; v++) {
        if (hist[v]) { double p = (double)hist[v] / n; chance += p * p; }
    }
    if (chance > 0.97) return 0.0;              // (near-)uniform: no signal
    long eq = 0, pairs = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x + 1 < w; x++) { pairs++; if (s.idx[(size_t)y * w + x] == s.idx[(size_t)y * w + x + 1]) eq++; }
    for (int x = 0; x < w; x++)
        for (int y = 0; y + 1 < h; y++) { pairs++; if (s.idx[(size_t)y * w + x] == s.idx[(size_t)(y + 1) * w + x]) eq++; }
    if (pairs == 0) return 0.0;
    double obs = (double)eq / pairs;
    double adj = (obs - chance) / (1.0 - chance);
    if (adj < 0) adj = 0;
    if (adj > 1) adj = 1;
    return adj;
}

static std::vector<uint8_t> colorize(const Sheet& s, const std::vector<uint32_t>& pal, int scale) {
    int w = s.w * scale, h = s.h * scale;
    std::vector<uint8_t> out((size_t)w * h * 4, 0);
    if (s.w <= 0 || s.h <= 0) return out;
    for (int y = 0; y < h; y++) {
        int sy = y / scale;
        for (int x = 0; x < w; x++) {
            int sx = x / scale;
            uint8_t v = s.idx[(size_t)sy * s.w + sx];
            uint32_t c = (v < pal.size()) ? pal[v] : fallbackRGBA(v);
            size_t o = ((size_t)y * w + x) * 4;
            out[o]     = (uint8_t)(c >> 24);
            out[o + 1] = (uint8_t)(c >> 16);
            out[o + 2] = (uint8_t)(c >> 8);
            out[o + 3] = 0xFF;
        }
    }
    return out;
}

// ------------------------------------------------------------------ usage

static void usage() {
    std::printf(
        "smk-extract -- Super Mario Kart (USA) asset extractor   [WIP]\n"
        "usage: smk-extract <rom> [options]\n"
        "\n"
        "discovery (default when no extraction mode is given):\n"
        "  -o DIR                output directory (e.g. assets)\n"
        "  --scan                force the discovery scan\n"
        "\n"
        "tile sheet:\n"
        "  --tiles OFF           file offset of tile data\n"
        "  --bpp {2|4|8}         bitplanes (default 4)\n"
        "  --count TILES         number of tiles (default 256)\n"
        "  --cols N              tiles per row (default 16)\n"
        "  --rev N               tile byte reordering for 'reversed' sheets:\n"
        "                        0 none, 1 bit-reverse each byte, 2 reverse each\n"
        "                        16-byte plane pair, 3 reverse whole tile,\n"
        "                        4 reverse plane-pair order, 5 = 1+2,\n"
        "                        6 reverse pixel-row order inside each tile,\n"
        "                        7 planes stored sequentially (8 bytes/plane),\n"
        "                        8 4 adjacent plane bytes per row (bpp/2 bytes)\n"
        "  --linear [W]           render raw bits as a linear bitmap (default\n"
        "                        width 64 px) instead of 8x8 tiles\n"
        "  --pal POFF            palette file offset (optional)\n"
        "  --palIndex N          sub-palette index (stride 16 colors, 256 for bpp 8)\n"
        "  --palColors N         how many colors to read from POFF (default: 4/16/256)\n"
        "\n"
        "palette swatch:\n"
        "  --pal POFF --count COLORS -o out.png\n"
        "     also writes out.pal (RGBA8 hex, one color per line)\n"
        "     and out.hex  (raw 15-bit SNES words, 8 per line)\n"
        "\n"
        "mode 7 map:\n"
        "  --mode7 OFF [W H]     map file offset, size in tiles (default 128 128)\n"
        "  --chars CHROFF        8bpp character set file offset\n"
        "  --charsize {64|256}   bytes per mode 7 tile (default 64)\n"
        "\n"
        "compressed stream (SMK codec, stops at the 0xFF terminator):\n"
        "  --decomp OFF          decompress first; combine with --tiles/--bpp/--pal\n"
        "                        or dump raw bytes when -o ends in .bin\n"
        "\n"
        "8bpp character sheet:\n"
        "  --chars8 OFF [COUNT]  8bpp tiles, count default 256\n"
        "\n"
        "common:\n"
        "  --scale N             nearest-neighbour upscale of the PNG (default 1)\n"
        "  -o FILE               output PNG (manual modes)\n"
        "  --help\n"
        "\n"
        "numbers accept decimal or 0x hex.\n");
}

// ------------------------------------------------------------ mode 7 render

static bool renderMode7(const Rom& rom, int64_t mapOff, int mw, int mh,
                        int64_t charOff, int charSize,
                        const std::vector<uint32_t>& pal, int scale,
                        std::vector<uint8_t>& rgba, int& ow, int& oh) {
    if (mw <= 0 || mh <= 0 || charSize < 64) return false;
    if (mapOff < 0 || charOff < 0) return false;
    ow = mw * scale;
    oh = mh * scale;
    rgba.assign((size_t)ow * oh * 4, 0);
    for (int y = 0; y < mh; y++) {
        for (int x = 0; x < mw; x++) {
            int64_t mo = mapOff + (int64_t)y * mw + x;
            if (mo >= (int64_t)rom.data.size()) continue;
            uint8_t tile = rom.data[(size_t)mo];
            int64_t base = charOff + (int64_t)tile * charSize;
            int64_t po = base + (int64_t)(y & 7) * 8 + (x & 7);
            uint8_t v = (po >= 0 && po < (int64_t)rom.data.size()) ? rom.data[(size_t)po] : 0;
            uint32_t c = (v < pal.size()) ? pal[v] : fallbackRGBA(v);
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    size_t o = ((size_t)(y * scale + sy) * ow + x * scale + sx) * 4;
                    rgba[o]     = (uint8_t)(c >> 24);
                    rgba[o + 1] = (uint8_t)(c >> 16);
                    rgba[o + 2] = (uint8_t)(c >> 8);
                    rgba[o + 3] = 0xFF;
                }
            }
        }
    }
    return true;
}

// ------------------------------------------------- SMK decompression (LZ)
//
// Super Mario Kart's native compression (what Epic Edit calls the codec,
// Lunar Compress "format 4"): a stream of chunks terminated by 0xFF.
//   header byte b:
//     command = b >> 5;  if command == 7 the command is extended:
//         command = (b & 0x1C) >> 2,  count10 = ((b & 3) << 8) | nextByte
//     else count10 = b & 0x1F
//     count = count10 + 1
//   commands:
//     0 copy count literal bytes          1 repeat next byte count times
//     2 alternate two bytes count times   3 increasing fill (byte + i)
//     4 copy count bytes from absolute    5 same, bytewise inverted
//       little-endian dest offset
//     6 copy count bytes from destOffset - nextByte (relative, overlaps ok)
//   7 (extended) = invalid
// Pointers must reference already-written output.

struct DecompResult {
    std::vector<uint8_t> out;
    size_t consumed = 0;                        // input bytes, terminator incl.
    bool   ok = false;
};

static bool gTrace = false;
static bool gScore = false;

static DecompResult decompSmk(const Rom& rom, int64_t off, size_t maxOut, size_t maxIn) {
    DecompResult r;
    const uint8_t* src = rom.data.data();
    int64_t n = (int64_t)rom.data.size();
    if (off < 0 || off >= n) return r;
    int64_t ip = off;
    const int64_t inLimit = off + (int64_t)maxIn;
    auto die = [&](const char* msg) -> DecompResult {
        if (gTrace) {
            std::fprintf(stderr, "  FAIL @0x%llX: %s (produced 0x%zX bytes, consumed 0x%llX)\n",
                         (unsigned long long)(off + (ip - off)), msg, r.out.size(),
                         (unsigned long long)(ip - off));
        }
        return DecompResult();
    };
    size_t step = 0;
    while (true) {
        if (ip >= n || ip >= inLimit) return die("ran off end / input limit");
        int64_t hdrAt = ip;
        uint8_t b = src[ip++];
        if (b == 0xFF) { r.ok = true; break; }   // stop command
        int cmd = b >> 5;
        size_t count;
        if (cmd == 7) {
            cmd = (b & 0x1C) >> 2;
            if (ip >= n || ip >= inLimit) return die("truncated extended header");
            count = (((size_t)(b & 3)) << 8) + src[ip++];
            if (cmd > 6) return die("invalid extended command");
            count += 1;
        } else {
            count = (size_t)(b & 0x1F) + 1;
        }
        if (r.out.size() + count > maxOut) return die("output limit exceeded");
        if (gTrace) {
            std::fprintf(stderr, "  %04zX: hdr@0x%llX cmd=%d count=%zu out=0x%zX\n",
                         step++, (unsigned long long)hdrAt, cmd, count, r.out.size());
        }
        switch (cmd) {
            case 0: {                                       // literals
                if (ip + (int64_t)count > n || ip + (int64_t)count > inLimit) return die("literal run off end");
                r.out.insert(r.out.end(), src + ip, src + ip + count);
                ip += count;
            } break;
            case 1: {                                       // byte fill
                if (ip >= n) return die("truncated byte fill");
                uint8_t v = src[ip++];
                r.out.insert(r.out.end(), count, v);
            } break;
            case 2: {                                       // word fill
                if (ip + 2 > n) return die("truncated word fill");
                uint8_t a = src[ip++], c = src[ip++];
                for (size_t i = 0; i < count; i++) r.out.push_back((i & 1) ? c : a);
            } break;
            case 3: {                                       // increasing fill
                if (ip >= n) return die("truncated inc fill");
                uint8_t v = src[ip++];
                for (size_t i = 0; i < count; i++) r.out.push_back((uint8_t)(v + i));
            } break;
            case 4:                                         // absolute copy
            case 5: {
                if (ip + 2 > n) return die("truncated abs copy");
                size_t p = (size_t)src[ip] + ((size_t)src[ip + 1] << 8);
                ip += 2;
                if (p >= r.out.size()) {
                    if (gTrace) std::fprintf(stderr, "    abs ptr=0x%zX but out=0x%zX\n", p, r.out.size());
                    return die("absolute pointer into unwritten output");
                }
                for (size_t i = 0; i < count; i++) {
                    uint8_t v = (p + i < r.out.size()) ? r.out[p + i] : 0x00;
                    r.out.push_back((cmd == 5) ? (uint8_t)(v ^ 0xFF) : v);
                }
            } break;
            case 6: {                                       // relative copy
                if (ip >= n) return die("truncated rel copy");
                size_t d = src[ip++];
                if (d == 0 || d > r.out.size()) {
                    if (gTrace) std::fprintf(stderr, "    rel delta=0x%zX but out=0x%zX\n", d, r.out.size());
                    return die("relative pointer into unwritten output");
                }
                size_t p = r.out.size() - d;
                for (size_t i = 0; i < count; i++) r.out.push_back(r.out[p + i]);
            } break;
            default: return die("invalid command");
        }
    }
    r.consumed = (size_t)(ip - off);
    return r;
}

// ------------------------------------------------------------------ scan

struct PalReg {
    size_t off = 0, len = 0;                    // bytes
    double conf = 0;
    int words = 0, distinct = 0;
};

struct GfxReg {
    size_t off = 0, len = 0;
    int bpp = 4, altBpp = -1;
    double score = 0, altScore = 0, conf = 0;
    double zeroFrac = 0;
    int distinct = 0;
    size_t palOff = 0;
    bool palFound = false;
    double palDist = 0;
};

static const size_t kWin = 128;                 // 4 x 32-byte tiles
static const size_t kStep = 16;
static const int kHeaderLo = 0xFFB0;            // SNES header / vectors area

static bool overlapsHeader(size_t off, size_t len) {
    size_t a = off, b = off + len;
    return a < (size_t)kHeaderLo + 16 && b > (size_t)kHeaderLo;
}

static std::vector<PalReg> scanPalettes(const Rom& rom) {
    std::vector<PalReg> regs;
    const uint8_t* d = rom.data.data();
    size_t n = rom.data.size();
    size_t i = 0;
    while (i + 64 <= n) {
        uint16_t w = (uint16_t)(d[i] | (d[i + 1] << 8));
        if (w & 0x8000) { i += 2; continue; }
        size_t start = i;
        std::vector<uint16_t> words;
        while (i + 1 < n) {
            uint16_t v = (uint16_t)(d[i] | (d[i + 1] << 8));
            if (v & 0x8000) break;
            words.push_back(v);
            i += 2;
        }
        size_t len = i - start;
        if (overlapsHeader(start, len)) { continue; }
        if (words.size() >= 32) {
            // plausibility: many distinct words, luminance spread, saturation
            std::vector<uint16_t> uniq = words;
            std::sort(uniq.begin(), uniq.end());
            uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
            double lo = 1e9, hi = -1e9;
            int satCount = 0;
            for (size_t k = 0; k < words.size(); k++) {
                uint16_t v = words[k];
                double r = expand5(v & 0x1F), g = expand5((v >> 5) & 0x1F), b = expand5((v >> 10) & 0x1F);
                double lum = 0.3 * r + 0.59 * g + 0.11 * b;
                lo = std::min(lo, lum);
                hi = std::max(hi, lum);
                double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
                if (mx > 0 && (mx - mn) / mx >= 0.25 && mx >= 64) satCount++;
            }
            double spread = hi - lo;
            if (uniq.size() < 10) continue;                   // >10 distinct words
            if (spread < 16) continue;                        // plausible luminance spread
            double distinctRatio = (double)uniq.size() / words.size();
            double lumScore = std::min(spread / 96.0, 1.0);
            double lenScore = std::min((double)words.size() / 64.0, 1.0);
            double satRatio = std::min((double)satCount / std::max<size_t>(1, words.size()) * 2.0, 1.0);
            double conf = 100.0 * (0.30 * std::min(distinctRatio * 2.0, 1.0) +
                                   0.30 * lumScore +
                                   0.20 * lenScore +
                                   0.20 * satRatio);
            PalReg r;
            r.off = start;
            r.len = len;
            r.words = (int)words.size();
            r.distinct = (int)uniq.size();
            r.conf = conf;
            regs.push_back(r);
        }
        if (i == start) i += 2;
    }
    return regs;
}

// score a fixed window as a tile sheet with the given bpp
static double windowScore(const uint8_t* p, size_t len, int bpp, int cols) {
    Sheet s = buildSheet(p, len, bpp, cols);
    return adjEqualScore(s);
}

static std::vector<GfxReg> scanGraphics(const Rom& rom) {
    std::vector<GfxReg> regs;
    const uint8_t* d = rom.data.data();
    size_t n = rom.data.size();
    if (n < kWin) return regs;

    std::vector<char> pass((n - kWin) / kStep + 1, 0);
    size_t nSteps = pass.size();
    for (size_t si = 0; si < nSteps; si++) {
        size_t off = si * kStep;
        const uint8_t* p = d + off;
        if (overlapsHeader(off, kWin)) continue;
        int zeros = 0;
        bool seen[256] = {false};
        int distinct = 0;
        for (size_t k = 0; k < kWin; k++) {
            if (p[k] == 0) zeros++;
            if (!seen[p[k]]) { seen[p[k]] = true; distinct++; }
        }
        if (zeros > (int)(kWin * 85 / 100)) continue;      // >85% zeros = padding
        if (distinct < 6) continue;
        double s2 = windowScore(p, kWin, 2, 8);          //  8 tiles -> 64x8
        double s4 = windowScore(p, kWin, 4, 4);          //  4 tiles -> 32x8
        double s8 = windowScore(p, kWin, 8, 2);          //  2 tiles -> 16x8
        double best = std::max(s2, std::max(s4, s8));
        if (best >= 0.12) pass[si] = 1;
    }

    // merge passing windows, tolerating gaps up to 32 bytes
    size_t si = 0;
    while (si < nSteps) {
        if (!pass[si]) { si++; continue; }
        size_t start = si * kStep;
        size_t end = start + kWin;
        size_t j = si;
        while (j + 1 < nSteps) {
            size_t k = j + 1;
            while (k < nSteps && !pass[k]) k++;
            if (k >= nSteps) break;
            if (k * kStep <= end + 32) { end = k * kStep + kWin; j = k; }
            else break;
        }
        if (end - start >= kWin && !overlapsHeader(start, end - start)) {
            GfxReg r;
            r.off = start;
            r.len = end - start;
            regs.push_back(r);
        }
        si = j + 1;
    }
    return regs;
}

static void evaluateGfx(const Rom& rom, GfxReg& r) {
    const uint8_t* d = rom.data.data() + r.off;
    int zeros = 0;
    bool seen[256] = {false};
    int distinct = 0;
    for (size_t k = 0; k < r.len; k++) {
        if (d[k] == 0) zeros++;
        if (!seen[d[k]]) { seen[d[k]] = true; distinct++; }
    }
    r.zeroFrac = (double)zeros / r.len;
    r.distinct = distinct;

    int cols = 16;
    double s2 = windowScore(d, r.len, 2, cols);
    double s4 = windowScore(d, r.len, 4, cols);
    double s8 = r.len >= 256 ? windowScore(d, r.len, 8, cols) : -1.0;

    // prefer 4bpp on ties; only switch when clearly better
    int best = 4;
    double bs = s4;
    if (s2 > bs + 0.04) { best = 2; bs = s2; }
    if (s8 >= 0 && s8 > bs + 0.04) { best = 8; bs = s8; }
    int alt = -1;
    double ascore = -1;
    double cand[3] = {s2, s4, s8};
    int cbpp[3] = {2, 4, 8};
    for (int k = 0; k < 3; k++) {
        if (cbpp[k] == best || cand[k] < 0) continue;
        if (cand[k] > ascore) { ascore = cand[k]; alt = cbpp[k]; }
    }
    if (ascore < bs - 0.04) { alt = -1; ascore = -1; }   // only keep a close runner-up

    r.bpp = best;
    r.score = bs;
    r.altBpp = alt;
    r.altScore = ascore;

    double zeroScore = 1.0 - std::min(r.zeroFrac, 0.6) / 0.6;
    double divScore = std::min(distinct / 48.0, 1.0);
    double lenScore = std::min((double)r.len / 2048.0, 1.0);
    r.conf = 100.0 * (0.50 * bs + 0.20 * zeroScore + 0.15 * divScore + 0.15 * lenScore);
}

static const char* confLabel(double c) {
    if (c >= 55) return "high";
    if (c >= 40) return "medium";
    return "low";
}

struct DecompReg {
    size_t off = 0, srcLen = 0, outLen = 0;
    int bpp = 4;
    double score = 0, conf = 0;
};

// Scan the whole ROM for SMK-compressed streams whose output looks like
// tile graphics. Streams are enumerated non-overlapping: after a clean
// stream (terminated by 0xFF) is found, scanning resumes after it.
static std::vector<DecompReg> scanCompressed(const Rom& rom) {
    std::vector<DecompReg> regs;
    size_t n = rom.data.size();
    size_t o = 0;
    while (o < n) {
        DecompResult r = decompSmk(rom, (int64_t)o, 0x10000, 0x4000);
        if (r.ok && r.consumed >= 8 && r.out.size() >= 128 &&
            !overlapsHeader(o, r.consumed)) {
            bool seen[256] = {false};
            int distinct = 0;
            for (uint8_t v : r.out) if (!seen[v]) { seen[v] = true; distinct++; }
            double s2 = windowScore(r.out.data(), r.out.size(), 2, 16);
            double s4 = windowScore(r.out.data(), r.out.size(), 4, 16);
            double s8 = r.out.size() >= 256 ? windowScore(r.out.data(), r.out.size(), 8, 16) : -1.0;
            int best = 4;
            double bs = s4;
            if (s2 > bs + 0.04) { best = 2; bs = s2; }
            if (s8 >= 0 && s8 > bs + 0.04) { best = 8; bs = s8; }
            if (bs >= 0.12 && distinct >= 8) {          // image-like output only
                DecompReg g;
                g.off = o;
                g.srcLen = r.consumed;
                g.outLen = r.out.size();
                g.bpp = best;
                g.score = bs;
                double lenScore = std::min((double)r.out.size() / 2048.0, 1.0);
                double divScore = std::min(distinct / 48.0, 1.0);
                g.conf = 100.0 * (0.55 * bs + 0.25 * lenScore + 0.20 * divScore);
                regs.push_back(g);
            }
            o += r.consumed;
            continue;
        }
        o += 1;
    }
    return regs;
}

// pick the nearest discovered palette (<= 64 KB away) for a graphics region
static void assignPalette(std::vector<GfxReg>& gfx, const std::vector<PalReg>& pals) {
    for (auto& g : gfx) {
        double best = 1e18;
        const PalReg* bp = nullptr;
        for (const auto& p : pals) {
            double d;
            if (p.off + p.len <= g.off) d = (double)(g.off - (p.off + p.len));
            else if (g.off + g.len <= p.off) d = (double)(p.off - (g.off + g.len));
            else d = 0;
            if (d < best) { best = d; bp = &p; }
        }
        if (bp && best <= 0x10000) {
            g.palOff = bp->off;
            g.palFound = true;
            g.palDist = best;
        }
    }
}

static void scanDmaHints(const Rom& rom, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "# DMA source hints for %s\n", rom.path.c_str());
    std::fprintf(f, "# occurrences of STA/STX/STY to a $43xx DMA register, plus the\n");
    std::fprintf(f, "# nearest preceding LDA/LDX/LDY #imm (8-bit imm8 and 16-bit imm16).\n");
    std::fprintf(f, "# offset    reg      opcode@   imm8   imm16\n");
    const uint8_t* d = rom.data.data();
    size_t n = rom.data.size();
    static const struct { uint8_t reg; const char* name; } regs[] = {
        {0x00, "DMAP0"}, {0x01, "BBAD0"}, {0x02, "A1T0L"}, {0x03, "A1T0H"},
        {0x04, "A1B0"},  {0x05, "DAS0L"}, {0x06, "DAS0H"},
        {0x08, "DMAP1"}, {0x0A, "A1T1L"}, {0x0C, "A1B1"},
    };
    for (size_t i = 0; i + 3 <= n; i++) {
        if (d[i] != 0x8D && d[i] != 0x8E && d[i] != 0x8C) continue;
        uint8_t lo = d[i + 1], hi = d[i + 2];
        if (hi != 0x43) continue;
        const char* rname = nullptr;
        for (const auto& r : regs) if (r.reg == lo) { rname = r.name; break; }
        if (!rname) continue;
        int imm8 = -1, imm16 = -1;
        for (size_t back = 1; back <= 16 && i >= back; back++) {
            size_t a = i - back;
            if (d[a] == 0xA9 || d[a] == 0xA2 || d[a] == 0xA0) {
                if (a + 3 <= n) { imm8 = d[a + 1]; imm16 = d[a + 1] | (d[a + 2] << 8); }
                break;
            }
        }
        const char* st = (d[i] == 0x8D) ? "STA" : (d[i] == 0x8E) ? "STX" : "STY";
        std::fprintf(f, "0x%06zX  %-7s %s $%02X%02X  %s", i, rname, st, hi, lo,
                     imm8 >= 0 ? "" : "-");
        if (imm8 >= 0) std::fprintf(f, "0x%02X  ", imm8); else std::fprintf(f, "-     ");
        if (imm16 >= 0) std::fprintf(f, "0x%04X", imm16); else std::fprintf(f, "-");
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

// ------------------------------------------------------------- discovery

struct Entry {
    std::string type;                           // "tiles" / "palette"
    size_t off = 0, len = 0;
    int bpp = 0;
    double conf = 0;
    std::string confName;                       // non-empty overrides confLabel(conf)
    std::string rel;                            // path relative to outDir
    std::string note;
};

// confidence label for an entry ("confirmed" for curated known regions)
static const char* confOf(const Entry& e) {
    return e.confName.empty() ? confLabel(e.conf) : e.confName.c_str();
}

static std::string snesAddr(const Rom& rom, size_t off) {
    (void)rom;
    unsigned bank = 0xC0 + (unsigned)(off >> 16);
    unsigned addr = (unsigned)(off & 0xFFFF);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X:%04X", bank, addr);
    return buf;
}

static int discover(const Rom& rom, const std::string& outDir) {
    makeDirs(joinPath(outDir, "tiles"));
    makeDirs(joinPath(outDir, "palettes"));

    std::printf("scanning %s (%zu bytes)...\n", rom.path.c_str(), rom.data.size());
    std::vector<PalReg> pals = scanPalettes(rom);
    std::printf("  palette runs: %zu\n", pals.size());
    std::vector<GfxReg> gfx = scanGraphics(rom);
    for (auto& g : gfx) evaluateGfx(rom, g);
    assignPalette(gfx, pals);
    std::printf("  graphics runs: %zu\n", gfx.size());
    std::vector<DecompReg> dcmp = scanCompressed(rom);
    std::printf("  compressed graphics streams: %zu\n", dcmp.size());

    std::vector<Entry> entries;

    // ---- palettes
    for (size_t i = 0; i < pals.size(); i++) {
        const PalReg& p = pals[i];
        char name[64];
        std::snprintf(name, sizeof name, "region_%06zX", p.off);
        std::vector<uint16_t> words = readPalWords(rom, (int64_t)p.off, p.len / 2);
        std::vector<uint32_t> rgba;
        rgba.reserve(words.size());
        for (uint16_t w : words) rgba.push_back(snesWordToRGBA(w));
        std::string base = joinPath(joinPath(outDir, "palettes"), name);
        std::vector<uint16_t> swatch(words.begin(),
            words.begin() + std::min<size_t>(words.size(), 256));
        std::vector<uint32_t> swatchRgba(rgba.begin(),
            rgba.begin() + std::min<size_t>(rgba.size(), 256));
        writePaletteSwatch(base + ".png", swatch, swatchRgba, 1);
        writePalFiles(base, words, rgba);
        Entry e;
        e.type = "palette";
        e.off = p.off;
        e.len = p.len;
        e.bpp = 0;
        e.conf = p.conf;
        e.rel = "palettes/" + std::string(name) + ".png";
        char buf[160];
        std::snprintf(buf, sizeof buf, "%d words, %d distinct, conf=%s",
                      p.words, p.distinct, confLabel(p.conf));
        e.note = buf;
        entries.push_back(e);
    }

    // ---- graphics
    for (size_t i = 0; i < gfx.size(); i++) {
        const GfxReg& g = gfx[i];
        char name[64];
        std::snprintf(name, sizeof name, "region_%06zX", g.off);
        size_t tb = (size_t)(8 * g.bpp);
        size_t len = g.len - (g.len % tb);
        Sheet sh = buildSheet(rom.data.data() + g.off, len, g.bpp, 16);

        std::vector<uint32_t> pal;
        int palColors = (g.bpp == 2) ? 4 : (g.bpp == 4) ? 16 : 256;
        if (g.palFound) {
            std::vector<uint16_t> w = readPalWords(rom, (int64_t)g.palOff, (size_t)palColors);
            for (uint16_t v : w) pal.push_back(snesWordToRGBA(v));
        }
        pal = extendPal(pal, (size_t)palColors);

        std::string base = joinPath(joinPath(outDir, "tiles"), name);
        std::vector<uint8_t> rgba = colorize(sh, pal, 1);
        png::write(base + ".png", sh.w, sh.h, rgba);

        if (g.altBpp > 0) {
            size_t atb = (size_t)(8 * g.altBpp);
            size_t alen = g.len - (g.len % atb);
            Sheet ash = buildSheet(rom.data.data() + g.off, alen, g.altBpp, 16);
            std::vector<uint32_t> apal;
            int ac = (g.altBpp == 2) ? 4 : (g.altBpp == 4) ? 16 : 256;
            if (g.palFound) {
                std::vector<uint16_t> w = readPalWords(rom, (int64_t)g.palOff, (size_t)ac);
                for (uint16_t v : w) apal.push_back(snesWordToRGBA(v));
            }
            apal = extendPal(apal, (size_t)ac);
            std::string abase = base + "_bpp" + std::to_string(g.altBpp);
            png::write(abase + ".png", ash.w, ash.h, colorize(ash, apal, 1));
        }

        Entry e;
        e.type = "tiles";
        e.off = g.off;
        e.len = g.len;
        e.bpp = g.bpp;
        e.conf = g.conf;
        e.rel = "tiles/" + std::string(name) + ".png";
        char buf[256];
        std::snprintf(buf, sizeof buf, "%zu tiles, score=%.2f, conf=%s, pal=%s",
                      len / tb, g.score, confLabel(g.conf),
                      g.palFound ? ("0x" + Rom::hex8((uint32_t)g.palOff, 6)).c_str() : "none");
        e.note = buf;
        entries.push_back(e);
    }

    // ---- compressed streams (decompressed on the fly)
    makeDirs(joinPath(outDir, "decomp"));
    for (size_t i = 0; i < dcmp.size(); i++) {
        const DecompReg& g = dcmp[i];
        DecompResult r = decompSmk(rom, (int64_t)g.off, 0x10000, 0x4000);
        if (!r.ok) continue;
        char name[64];
        std::snprintf(name, sizeof name, "region_%06zX", g.off);
        size_t tb = (size_t)(8 * g.bpp);
        size_t len = r.out.size() - (r.out.size() % tb);
        Sheet sh = buildSheet(r.out.data(), len, g.bpp, 16);

        // nearest palette (same rule as raw regions)
        size_t palOff = 0;
        bool palFound = false;
        {
            double bestD = 1e18;
            for (const auto& p : pals) {
                double d;
                if (p.off + p.len <= g.off) d = (double)(g.off - (p.off + p.len));
                else if (g.off + g.srcLen <= p.off) d = (double)(p.off - (g.off + g.srcLen));
                else d = 0;
                if (d < bestD) { bestD = d; palOff = p.off; }
            }
            palFound = bestD <= 0x10000;
        }
        std::vector<uint32_t> pal;
        int palColors = (g.bpp == 2) ? 4 : (g.bpp == 4) ? 16 : 256;
        if (palFound) {
            std::vector<uint16_t> w = readPalWords(rom, (int64_t)palOff, (size_t)palColors);
            for (uint16_t v : w) pal.push_back(snesWordToRGBA(v));
        }
        pal = extendPal(pal, (size_t)palColors);

        std::string base = joinPath(joinPath(outDir, "decomp"), name);
        png::write(base + ".png", sh.w, sh.h, colorize(sh, pal, 1));

        Entry e;
        e.type = "decomp";
        e.off = g.off;
        e.len = g.srcLen;
        e.bpp = g.bpp;
        e.conf = g.conf;
        e.rel = "decomp/" + std::string(name) + ".png";
        char buf[256];
        std::snprintf(buf, sizeof buf, "compressed -> 0x%zX bytes, %zu tiles, score=%.2f, conf=%s, pal=%s",
                      r.out.size(), len / tb, g.score, confLabel(g.conf),
                      palFound ? ("0x" + Rom::hex8((uint32_t)palOff, 6)).c_str() : "none");
        e.note = buf;
        entries.push_back(e);
    }

    // ---- curated known regions ------------------------------------------
    // Cross-checked against the Epic Racers SMK documentation
    // (https://epicedit.stifu.fr/smk_doc): every compressed stream listed
    // here decodes cleanly with decompSmk() and its consumed/compressed
    // length matches the published value.  Known regions are merged into
    // the heuristic scan results so confirmed assets are never missed, and
    // matching scan hits get labelled "confirmed" with a human name.
    struct Known {
        size_t off;                              // ROM offset
        size_t rawLen;                           // byte length of raw region (0 = compressed)
        int bpp;                                 // render bpp
        bool comp;                               // SMK-compressed stream?
        size_t palOff;                           // explicit palette (raw regions; 0 = nearest)
        const char* name;
        const char* desc;
    };
    static const Known kKnown[] = {
        {0x002000, 0x6000, 4, false, 0x01F4C, "driver-0", "driver sprite sheet (768 tiles 4bpp)"},
        {0x012000, 0x6000, 4, false, 0x11F4C, "driver-1", "driver sprite sheet (768 tiles 4bpp)"},
        {0x022000, 0x6000, 4, false, 0x21F4C, "driver-2", "driver sprite sheet (768 tiles 4bpp)"},
        {0x032000, 0x6000, 4, false, 0x31F4C, "driver-3", "driver sprite sheet (768 tiles 4bpp)"},
        {0x042000, 0x6000, 4, false, 0x41F4C, "driver-4", "driver sprite sheet (768 tiles 4bpp)"},
        {0x052000, 0x6000, 4, false, 0x51F4C, "driver-5", "driver sprite sheet (768 tiles 4bpp)"},
        {0x062000, 0x6000, 4, false, 0x61F4C, "driver-6", "driver sprite sheet (768 tiles 4bpp)"},
        {0x010000, 0,     4, true,  0, "lakitu",       "Lakitu sprites (0x1380 bytes out)"},
        {0x0112F8, 0,     2, true,  0, "race-font",    "race HUD font (0x40E in -> 0x700 out)"},
        {0x040000, 0,     4, true,  0, "road-gfx",     "road graphics sheet (chunky 4bpp; docs/assets.md)"},
        {0x040594, 0,     4, true,  0, "items",        "item icons (mushroom, shells, banana, ...)"},
        {0x04117F, 0,     8, true,  0, "track-pal-1",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x041313, 0,     8, true,  0, "track-pal-2",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x0414C4, 0,     8, true,  0, "track-pal-3",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x041675, 0,     8, true,  0, "track-pal-4",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x04182F, 0,     8, true,  0, "track-pal-5",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x0419C0, 0,     8, true,  0, "track-pal-6",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x041B5B, 0,     8, true,  0, "track-pal-7",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x041D0B, 0,     8, true,  0, "track-pal-8",  "track palette stream (0x200 = 16 sub-palettes)"},
        {0x041EBB, 0,     4, true,  0, "road-rr",      "Rainbow Road road graphics (0x280 out)"},
        {0x0481C9, 0,     4, true,  0, "road-mc",      "Mario Circuit road graphics (0x1900 out)"},
        {0x050000, 0,     4, true,  0, "road-dp",      "Donut Plains road graphics (0x1100 out)"},
        {0x051636, 0,     4, true,  0, "road-kb",      "Koopa Beach road graphics (0x1100 out)"},
        {0x060000, 0,     4, true,  0, "chain-chomp",  "Chain Chomp sprite (0x280 out)"},
        {0x060189, 0,     4, true,  0, "road-gv",      "Ghost Valley road graphics (0x620 out)"},
        {0x070000, 0,     4, true,  0, "fonts",        "menu/race fonts (0x1000 out)"},
        {0x070B29, 0,     4, true,  0, "select-screen","character & cup select screen (0x4000 out)"},
        {0x071996, 0,     4, true,  0, "title-results","title screen + GP results (0x4400 out)"},
        {0x0737DA, 0,     4, true,  0, "podium",       "podium scene, double-compressed (1st pass 0x295D)"},
        {0x078000, 0,     4, true,  0, "road-ci",      "Choco Island road graphics (0xD80 out)"},
    };

    auto findEntry = [&](size_t off, const char* type) -> Entry* {
        for (auto& e : entries)
            if (e.off == off && e.type == type) return &e;
        return nullptr;
    };
    int curatedNew = 0;
    for (const Known& k : kKnown) {
        const char* type = k.comp ? "decomp" : "tiles";
        if (Entry* ex = findEntry(k.off, type)) {
            ex->confName = "confirmed";
            ex->note = std::string(k.name) + ": " + k.desc + "; " + ex->note;
            continue;
        }
        char name[64];
        std::snprintf(name, sizeof name, "region_%06zX", k.off);
        Entry e;
        e.off = k.off;
        e.bpp = k.bpp;
        e.confName = "confirmed";

        if (k.comp) {
            DecompResult r = decompSmk(rom, (int64_t)k.off, 0x40000, rom.data.size());
            if (!r.ok) {
                std::printf("  curated 0x%zX (%s): decompression failed, skipped\n", k.off, k.name);
                continue;
            }
            // nearest palette (same rule as the scan)
            size_t palOff = 0;
            bool palFound = false;
            {
                double bestD = 1e18;
                for (const auto& p : pals) {
                    double d;
                    if (p.off + p.len <= k.off) d = (double)(k.off - (p.off + p.len));
                    else if (k.off + r.consumed <= p.off) d = (double)(p.off - (k.off + r.consumed));
                    else d = 0;
                    if (d < bestD) { bestD = d; palOff = p.off; }
                }
                palFound = bestD <= 0x10000;
            }
            size_t tb = (size_t)(8 * k.bpp);
            size_t len = r.out.size() - (r.out.size() % tb);
            Sheet sh = buildSheet(r.out.data(), len, k.bpp, 16);
            std::vector<uint32_t> pal;
            int palColors = (k.bpp == 2) ? 4 : (k.bpp == 4) ? 16 : 256;
            if (palFound) {
                std::vector<uint16_t> w = readPalWords(rom, (int64_t)palOff, (size_t)palColors);
                for (uint16_t v : w) pal.push_back(snesWordToRGBA(v));
            }
            pal = extendPal(pal, (size_t)palColors);
            makeDirs(joinPath(outDir, "decomp"));
            std::string base = joinPath(joinPath(outDir, "decomp"), name);
            png::write(base + ".png", sh.w, sh.h, colorize(sh, pal, 1));
            e.type = "decomp";
            e.len = r.consumed;
            e.rel = "decomp/" + std::string(name) + ".png";
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s: %s; compressed -> 0x%zX bytes, %zu tiles, pal=%s",
                          k.name, k.desc, r.out.size(), len / tb,
                          palFound ? ("0x" + Rom::hex8((uint32_t)palOff, 6)).c_str() : "none");
            e.note = buf;
        } else {
            // raw tile sheet with a known palette
            if (k.off + k.rawLen > rom.data.size()) continue;
            size_t tb = (size_t)(8 * k.bpp);
            size_t len = k.rawLen - (k.rawLen % tb);
            Sheet sh = buildSheet(rom.data.data() + k.off, len, k.bpp, 16);
            std::vector<uint32_t> pal;
            int palColors = (k.bpp == 2) ? 4 : (k.bpp == 4) ? 16 : 256;
            bool palFound = k.palOff != 0 && k.palOff + 32 <= rom.data.size();
            if (palFound) {
                std::vector<uint16_t> w = readPalWords(rom, (int64_t)k.palOff, (size_t)palColors);
                for (uint16_t v : w) pal.push_back(snesWordToRGBA(v));
            }
            pal = extendPal(pal, (size_t)palColors);
            makeDirs(joinPath(outDir, "tiles"));
            std::string base = joinPath(joinPath(outDir, "tiles"), name);
            png::write(base + ".png", sh.w, sh.h, colorize(sh, pal, 1));
            e.type = "tiles";
            e.len = k.rawLen;
            e.rel = "tiles/" + std::string(name) + ".png";
            char buf[256];
            std::snprintf(buf, sizeof buf, "%s: %s; %zu tiles, pal=%s",
                          k.name, k.desc, len / tb,
                          palFound ? ("0x" + Rom::hex8((uint32_t)k.palOff, 6)).c_str() : "none");
            e.note = buf;
        }
        entries.push_back(e);
        curatedNew++;
    }
    if (curatedNew)
        std::printf("  curated known regions added: %d\n", curatedNew);

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.off < b.off; });

    // ---- index.txt
    {
        std::string path = joinPath(outDir, "index.txt");
        FILE* f = std::fopen(path.c_str(), "w");
        if (f) {
            std::fprintf(f, "# Super Mario Kart (USA) -- discovered asset regions\n");
            std::fprintf(f, "# generated by tools/extract.cpp (discovery mode)\n");
            std::fprintf(f, "# offset    snes      type     length   bpp conf     file / notes\n");
            for (const auto& e : entries) {
                std::fprintf(f, "0x%06zX  %s  %-7s  0x%05zX  %-3d %-8s %s  %s\n",
                             e.off, snesAddr(rom, e.off).c_str(), e.type.c_str(),
                             e.len, e.bpp, confOf(e), e.rel.c_str(), e.note.c_str());
            }
            std::fclose(f);
        }
    }

    // ---- index.html
    {
        std::string path = joinPath(outDir, "index.html");
        FILE* f = std::fopen(path.c_str(), "w");
        if (f) {
            std::fprintf(f, "<!doctype html>\n<meta charset=\"utf-8\">\n");
            std::fprintf(f, "<title>SMK asset index</title>\n");
            std::fprintf(f, "<style>body{font:13px monospace;background:#111;color:#ddd}"
                            "table{border-collapse:collapse}td,th{border:1px solid #444;"
                            "padding:3px;vertical-align:top}img{image-rendering:pixelated;"
                            "background:#000}a{color:#8cf}"
                            ".bar{display:flex;align-items:center;gap:10px;padding:8px 12px;"
                            "background:#1b1b1b;border-bottom:1px solid #444}"
                            ".bar .logo{font-weight:bold;color:#8cf}"
                            ".bar button{font:12px monospace;background:#333;color:#888;"
                            "border:1px solid #555;padding:4px 10px;cursor:not-allowed}"
                            ".bar .tag{font-weight:bold;background:#a60;color:#fff;"
                            "border:1px solid #f66;border-radius:3px;padding:2px 8px;"
                            "letter-spacing:2px}"
                            ".wip{margin:10px 12px;padding:8px 10px;background:#2a1414;"
                            "border:1px solid #a44;border-left:4px solid #f55;"
                            "color:#fbb}</style>\n");
            std::fprintf(f, "<div class=\"bar\"><span class=\"logo\">SMK asset index</span>"
                            "<button disabled title=\"not implemented yet\">Import ROM</button>"
                            "<span class=\"tag\">WIP</span></div>\n");
            std::fprintf(f, "<div class=\"wip\">WIP -- work in progress: the <b>Import ROM</b> "
                            "loader is not implemented yet (pressing it does nothing). "
                            "Everything below is a static index of assets extracted locally "
                            "from your own copy of Super Mario Kart (USA) via "
                            "<code>bin\\smk-extract.exe</code>.</div>\n");
            std::fprintf(f, "<h1>Super Mario Kart (USA) -- discovered asset regions</h1>\n");
            std::fprintf(f, "<p>%zu regions (recall-first scan; low confidence entries are "
                            "likely false positives). See index.txt for the same data.</p>\n",
                         entries.size());
            std::fprintf(f, "<table><tr><th>offset</th><th>SNES</th><th>type</th>"
                            "<th>length</th><th>bpp</th><th>conf</th><th>preview</th>"
                            "<th>notes</th></tr>\n");
            for (const auto& e : entries) {
                std::fprintf(f,
                    "<tr><td>0x%06zX</td><td>%s</td><td>%s</td><td>0x%05zX</td>"
                    "<td>%d</td><td>%s</td><td>",
                    e.off, snesAddr(rom, e.off).c_str(), e.type.c_str(), e.len,
                    e.bpp, confOf(e));
                if (e.type == "tiles" || e.type == "decomp") {
                    std::fprintf(f, "<a href=\"%s\"><img src=\"%s\" width=\"128\" height=\"auto\"></a>",
                                 e.rel.c_str(), e.rel.c_str());
                } else {
                    std::fprintf(f, "<a href=\"%s\"><img src=\"%s\" width=\"128\"></a>",
                                 e.rel.c_str(), e.rel.c_str());
                }
                std::fprintf(f, "</td><td>%s</td></tr>\n", e.note.c_str());
            }
            std::fprintf(f, "</table>\n");
            std::fclose(f);
        }
    }

    scanDmaHints(rom, joinPath(outDir, "dma-hints.txt"));

    std::printf("wrote %zu entries to %s\n", entries.size(), outDir.c_str());
    return 0;
}

// ------------------------------------------------------------------ main

struct Opts {
    std::string rom, out;
    bool haveTiles = false;
    int64_t tilesOff = 0;
    int bpp = 4;
    bool havePal = false;
    int64_t palOff = 0;
    int palIndex = 0;
    int64_t palColors = 0;                      // 0 = default
    int64_t count = -1;
    bool haveMode7 = false;
    int64_t m7Off = 0;
    int m7W = 128, m7H = 128;
    bool haveChars = false;
    int64_t charsOff = 0;
    bool haveChars8 = false;
    int64_t c8Off = 0;
    int64_t c8Count = 256;
    bool haveDecomp = false;
    int64_t decompOff = 0;
    int charSize = 64;
    int cols = 16;
    int scale = 1;
    int rev = 0;                                 // tile byte reordering (see reorderTile)
    bool linear = false;                         // render as linear bitmap instead of tiles
    int linWidth = 64;                           // pixels per row for --linear
    bool scan = false;
};

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    if (argc == 2 && (!std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }

    Opts o;
    std::vector<std::string> args(argv + 1, argv + argc);
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "error: missing value after %s\n", what);
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "-o" || a == "--out") o.out = need(a.c_str());
        else if (a == "--scan") o.scan = true;
        else if (a == "--trace") gTrace = true;
        else if (a == "--score") gScore = true;
        else if (a == "--tiles") { o.haveTiles = true; o.tilesOff = parseNum(need("--tiles"), "--tiles"); }
        else if (a == "--bpp") { o.bpp = (int)parseNum(need("--bpp"), "--bpp"); }
        else if (a == "--count") { o.count = parseNum(need("--count"), "--count"); }
        else if (a == "--cols") { o.cols = (int)parseNum(need("--cols"), "--cols"); }
        else if (a == "--pal") { o.havePal = true; o.palOff = parseNum(need("--pal"), "--pal"); }
        else if (a == "--palIndex") { o.palIndex = (int)parseNum(need("--palIndex"), "--palIndex"); }
        else if (a == "--palColors") { o.palColors = parseNum(need("--palColors"), "--palColors"); }
        else if (a == "--charsize") { o.charSize = (int)parseNum(need("--charsize"), "--charsize"); }
        else if (a == "--rev") { o.rev = (int)parseNum(need("--rev"), "--rev"); }
        else if (a == "--linear") {
            o.linear = true;
            if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
                const std::string nx = args[++i];
                char* end = nullptr;
                std::strtoll(nx.c_str(), &end, 0);
                if (end == nx.c_str() || (end && *end != '\0')) {
                    std::fprintf(stderr, "error: bad width '%s' for --linear\n", nx.c_str());
                    return 2;
                }
                o.linWidth = (int)parseNum(nx, "--linear width");
            }
        }
        else if (a == "--scale") { o.scale = (int)parseNum(need("--scale"), "--scale"); }
        else if (a == "--chars") { o.haveChars = true; o.charsOff = parseNum(need("--chars"), "--chars"); }
        else if (a == "--decomp") { o.haveDecomp = true; o.decompOff = parseNum(need("--decomp"), "--decomp"); }
        else if (a == "--mode7") {
            o.haveMode7 = true;
            o.m7Off = parseNum(need("--mode7"), "--mode7");
            // optional positional W H
            for (int k = 0; k < 2 && i + 1 < args.size(); k++) {
                const std::string& nx = args[i + 1];
                if (!nx.empty() && nx[0] == '-') break;
                char* end = nullptr;
                std::strtoll(nx.c_str(), &end, 0);
                if (end == nx.c_str() || (end && *end != '\0')) break;
                i++;
                int64_t v = parseNum(nx, "--mode7 size");
                if (k == 0) o.m7W = (int)v; else o.m7H = (int)v;
            }
        } else if (a == "--chars8") {
            o.haveChars8 = true;
            o.c8Off = parseNum(need("--chars8"), "--chars8");
            if (i + 1 < args.size() && !args[i + 1].empty() && args[i + 1][0] != '-') {
                const std::string nx = args[++i];
                char* end = nullptr;
                std::strtoll(nx.c_str(), &end, 0);
                if (end == nx.c_str() || (end && *end != '\0')) {
                    std::fprintf(stderr, "error: bad number '%s' for --chars8\n", nx.c_str());
                    return 2;
                }
                o.c8Count = parseNum(nx, "--chars8 count");
            }
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            return 2;
        } else if (o.rom.empty()) {
            o.rom = a;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", a.c_str());
            return 2;
        }
    }

    if (o.rom.empty()) { std::fprintf(stderr, "error: no ROM given\n"); usage(); return 1; }
    if (o.out.empty()) { std::fprintf(stderr, "error: -o <dir-or-file> is required\n"); usage(); return 1; }
    if (o.scale < 1) o.scale = 1;
    if (o.cols < 1) o.cols = 16;
    if (o.bpp != 2 && o.bpp != 4 && o.bpp != 8) {
        std::fprintf(stderr, "error: --bpp must be 2, 4 or 8\n");
        return 2;
    }

    Rom rom;
    std::string err;
    if (!rom.load(o.rom, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    // Discovery runs when no extraction mode was requested (or --scan was given).
    bool paletteOnly = o.havePal && !o.haveTiles && !o.haveMode7 && !o.haveChars8 && !o.haveDecomp;
    bool anyMode = o.haveTiles || o.haveMode7 || o.haveChars8 || paletteOnly || o.haveDecomp;
    if (o.scan || !anyMode) {
        return discover(rom, o.out);
    }

    std::string outPath = o.out;
    std::string d = dirPart(outPath);
    if (!d.empty()) makeDirs(d);

    // ---------------------------------------------------- palette swatch
    if (paletteOnly) {
        int64_t ncolors = (o.count >= 0) ? o.count : (o.palColors > 0 ? o.palColors : 16);
        std::vector<uint16_t> words = readPalWords(rom, o.palOff, (size_t)std::max<int64_t>(ncolors, 1));
        if (words.empty()) { std::fprintf(stderr, "error: no palette data at 0x%llX\n", (unsigned long long)o.palOff); return 1; }
        std::vector<uint32_t> rgba;
        for (uint16_t w : words) rgba.push_back(snesWordToRGBA(w));
        if (!writePaletteSwatch(outPath, words, rgba, o.scale)) {
            std::fprintf(stderr, "error: failed to write %s\n", outPath.c_str());
            return 1;
        }
        writePalFiles(stripExt(outPath), words, rgba);
        std::printf("wrote %s (+ .pal/.hex), %zu colors from offset 0x%llX\n",
                    outPath.c_str(), words.size(), (unsigned long long)o.palOff);
        return 0;
    }

    // ---------------------------------------------------------- mode 7
    if (o.haveMode7) {
        if (!o.haveChars) {
            std::fprintf(stderr, "error: --mode7 requires --chars <charFileOffset>\n");
            return 2;
        }
        std::vector<uint32_t> pal;
        if (o.havePal) {
            int64_t n = (o.palColors > 0) ? o.palColors : (o.count >= 0 ? o.count : 256);
            std::vector<uint16_t> w = readPalWords(rom, o.palOff + (int64_t)o.palIndex * 256 * 2, (size_t)n);
            for (uint16_t v : w) pal.push_back(snesWordToRGBA(v));
        }
        pal = extendPal(pal, 256);
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!renderMode7(rom, o.m7Off, o.m7W, o.m7H, o.charsOff, o.charSize, pal, o.scale, rgba, w, h)) {
            std::fprintf(stderr, "error: mode 7 render failed\n");
            return 1;
        }
        if (!png::write(outPath, w, h, rgba)) {
            std::fprintf(stderr, "error: failed to write %s\n", outPath.c_str());
            return 1;
        }
        std::printf("wrote %s: mode7 map 0x%llX (%dx%d), chars 0x%llX (%d bytes/tile)\n",
                    outPath.c_str(), (unsigned long long)o.m7Off, o.m7W, o.m7H,
                    (unsigned long long)o.charsOff, o.charSize);
        return 0;
    }

    // ------------------------------------------------- 8bpp char sheet
    if (o.haveChars8) {
        std::vector<uint32_t> pal;
        std::vector<uint16_t> palWords;
        if (o.havePal) {
            int64_t stride = (o.palColors > 0) ? o.palColors : 256;
            palWords = readPalWords(rom, o.palOff + (int64_t)o.palIndex * stride * 2, (size_t)stride);
            for (uint16_t v : palWords) pal.push_back(snesWordToRGBA(v));
        }
        pal = extendPal(pal, 256);
        int64_t n = (o.count >= 0) ? o.count : o.c8Count;
        size_t len = (size_t)std::max<int64_t>(n, 1) * 64;
        if (o.c8Off < 0 || (size_t)o.c8Off >= rom.data.size()) {
            std::fprintf(stderr, "error: offset out of range\n");
            return 1;
        }
        if ((size_t)o.c8Off + len > rom.data.size()) len = rom.data.size() - (size_t)o.c8Off;
        Sheet sh = buildSheet(rom.data.data() + o.c8Off, len, 8, o.cols);
        if (sh.w == 0) { std::fprintf(stderr, "error: nothing to render\n"); return 1; }
        std::vector<uint8_t> rgba = colorize(sh, pal, o.scale);
        if (!png::write(outPath, sh.w * o.scale, sh.h * o.scale, rgba)) {
            std::fprintf(stderr, "error: failed to write %s\n", outPath.c_str());
            return 1;
        }
        if (o.havePal && !palWords.empty()) writePalFiles(stripExt(outPath), palWords, pal);
        std::printf("wrote %s: %zu 8bpp tiles from 0x%llX\n", outPath.c_str(),
                    sh.tiles, (unsigned long long)o.c8Off);
        return 0;
    }

    // ------------------------------------------ decompressed stream -> out
    if (o.haveDecomp) {
        DecompResult dr = decompSmk(rom, o.decompOff, 0x40000, rom.data.size());
        if (!dr.ok || dr.out.empty()) {
            std::fprintf(stderr, "error: decompression failed at 0x%llX (no clean 0xFF terminator or bad command)\n",
                         (unsigned long long)o.decompOff);
            return 1;
        }
        std::string low = outPath;
        for (char& c : low) c = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        if (low.size() >= 4 && low.substr(low.size() - 4) == ".bin") {
            FILE* f = std::fopen(outPath.c_str(), "wb");
            if (!f) { std::fprintf(stderr, "error: cannot write %s\n", outPath.c_str()); return 1; }
            std::fwrite(dr.out.data(), 1, dr.out.size(), f);
            std::fclose(f);
            std::printf("wrote %s: 0x%zX bytes decompressed from 0x%llX (consumed 0x%zX)\n",
                        outPath.c_str(), dr.out.size(), (unsigned long long)o.decompOff, dr.consumed);
            return 0;
        }
        // render the decompressed bytes as a tile sheet below
        int tb = 8 * o.bpp;
        size_t len = dr.out.size() - (dr.out.size() % (size_t)tb);
        if (o.count >= 0) len = std::min(len, (size_t)o.count * (size_t)tb);

        std::vector<uint32_t> pal;
        std::vector<uint16_t> palWords;
        int palColors = (o.palColors > 0) ? (int)o.palColors : (o.bpp == 2 ? 4 : o.bpp == 4 ? 16 : 256);
        if (o.havePal) {
            int64_t stride = (o.bpp == 8 ? 256 : 16);
            int64_t pOff = o.palOff + (int64_t)o.palIndex * stride * 2;
            palWords = readPalWords(rom, pOff, (size_t)palColors);
            std::vector<uint32_t> rgba;
            for (uint16_t w : palWords) {
                pal.push_back(snesWordToRGBA(w));
                rgba.push_back(snesWordToRGBA(w));
            }
            if (!palWords.empty()) writePalFiles(stripExt(outPath), palWords, rgba);
        }
        pal = extendPal(pal, (size_t)palColors);

        if (o.rev < 0 || o.rev > 8) {
            std::fprintf(stderr, "error: --rev must be 0..8\n");
            return 2;
        }
        Sheet sh = o.linear
            ? buildLinear(dr.out.data(), dr.out.size(), o.bpp, o.linWidth)
            : buildSheet(dr.out.data(), len, o.bpp, o.cols, o.rev);
        if (sh.w == 0) { std::fprintf(stderr, "error: nothing to render\n"); return 1; }
        if (gScore) std::printf("score=%.4f (adj-equal, 0=noise, 0.3+=image)\n", adjEqualScore(sh));
        std::vector<uint8_t> rgba = colorize(sh, pal, o.scale);
        if (!png::write(outPath, sh.w * o.scale, sh.h * o.scale, rgba)) {
            std::fprintf(stderr, "error: failed to write %s\n", outPath.c_str());
            return 1;
        }
        std::printf("wrote %s: %zu tiles (%dbpp) from decompressed 0x%llX [0x%zX -> 0x%zX bytes]\n",
                    outPath.c_str(), sh.tiles, o.bpp, (unsigned long long)o.decompOff,
                    dr.consumed, dr.out.size());
        return 0;
    }

    // ------------------------------------------------------- tile sheet
    {
        int tb = 8 * o.bpp;
        int64_t n = (o.count >= 0) ? o.count : 256;
        if (n < 1) n = 1;
        size_t len = (size_t)n * (size_t)tb;
        if (o.tilesOff < 0 || (size_t)o.tilesOff >= rom.data.size()) {
            std::fprintf(stderr, "error: offset out of range\n");
            return 1;
        }
        if ((size_t)o.tilesOff + len > rom.data.size()) len = rom.data.size() - (size_t)o.tilesOff;

        std::vector<uint32_t> pal;
        std::vector<uint16_t> palWords;
        int palColors = (o.palColors > 0) ? (int)o.palColors : (o.bpp == 2 ? 4 : o.bpp == 4 ? 16 : 256);
        if (o.havePal) {
            int64_t stride = (o.bpp == 8 ? 256 : 16);
            int64_t pOff = o.palOff + (int64_t)o.palIndex * stride * 2;
            palWords = readPalWords(rom, pOff, (size_t)palColors);
            std::vector<uint32_t> rgba;
            for (uint16_t w : palWords) {
                pal.push_back(snesWordToRGBA(w));
                rgba.push_back(snesWordToRGBA(w));
            }
            if (!palWords.empty()) writePalFiles(stripExt(outPath), palWords, rgba);
        }
        pal = extendPal(pal, (size_t)palColors);

        if (o.rev < 0 || o.rev > 8) {
            std::fprintf(stderr, "error: --rev must be 0..8\n");
            return 2;
        }
        Sheet sh = o.linear
            ? buildLinear(rom.data.data() + o.tilesOff, len, o.bpp, o.linWidth)
            : buildSheet(rom.data.data() + o.tilesOff, len, o.bpp, o.cols, o.rev);
        if (sh.w == 0) { std::fprintf(stderr, "error: nothing to render\n"); return 1; }
        if (gScore) std::printf("score=%.4f (adj-equal, 0=noise, 0.3+=image)\n", adjEqualScore(sh));
        std::vector<uint8_t> rgba = colorize(sh, pal, o.scale);
        if (!png::write(outPath, sh.w * o.scale, sh.h * o.scale, rgba)) {
            std::fprintf(stderr, "error: failed to write %s\n", outPath.c_str());
            return 1;
        }
        std::printf("wrote %s: %zu tiles (%dbpp) from 0x%llX%s\n", outPath.c_str(),
                    sh.tiles, o.bpp, (unsigned long long)o.tilesOff,
                    o.havePal ? " with palette" : "");
        return 0;
    }
}

