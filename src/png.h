// png.h -- minimal dependency-free PNG writer (RGBA8, zlib stored blocks).
// Emits valid PNGs without needing zlib: deflate "stored" blocks are legal
// and keep the implementation to a few dozen lines.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace png {

inline void crcTable(uint32_t* t) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
}
inline uint32_t crc32(const uint8_t* d, size_t n) {
    static uint32_t t[256]; static bool inited = false;
    if (!inited) { crcTable(t); inited = true; }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
inline uint32_t adler32(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}
inline void be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    be32(out, (uint32_t)body.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32(out.data() + start, 4 + body.size()));
}

// Wrap raw bytes in a zlib stream made of stored deflate blocks.
inline std::vector<uint8_t> zlibStore(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);            // CMF/FLG: 32K window, no dict
    size_t i = 0;
    if (raw.empty()) { z.push_back(0x01); z.push_back(0x00); z.push_back(0x00);
                       z.push_back(0xFF); z.push_back(0xFF); }
    while (i < raw.size()) {
        size_t take = raw.size() - i;
        if (take > 65535) take = 65535;
        bool last = (i + take == raw.size());
        z.push_back(last ? 0x01 : 0x00);             // BFINAL | BTYPE=00, then pad to byte
        z.push_back((uint8_t)(take & 0xFF));          // LEN
        z.push_back((uint8_t)(take >> 8));
        z.push_back((uint8_t)(~take & 0xFF));         // NLEN
        z.push_back((uint8_t)(~take >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + take);
        i += take;
    }
    be32(z, adler32(raw.data(), raw.size()));
    return z;
}

// rgba: w*h*4 bytes, row-major, non-premultiplied.
inline bool write(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    if (w <= 0 || h <= 0 || rgba.size() != (size_t)w * h * 4) return false;

    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * (1 + (size_t)w * 4));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);                                   // filter type: None
        const uint8_t* row = rgba.data() + (size_t)y * w * 4;
        raw.insert(raw.end(), row, row + (size_t)w * 4);
    }

    std::vector<uint8_t> out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    be32(ihdr, (uint32_t)w);
    be32(ihdr, (uint32_t)h);
    ihdr.push_back(8);      // bit depth
    ihdr.push_back(6);      // colour type: RGBA
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", zlibStore(raw));
    chunk(out, "IEND", {});

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t n = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return n == out.size();
}

} // namespace png
