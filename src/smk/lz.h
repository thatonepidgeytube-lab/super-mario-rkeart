// lz.h -- Super Mario Kart's native compression codec (what Epic Edit calls
// the codec, Lunar Compress "format 4").  Ported from tools/extract.cpp; it
// matches the game's own decompressor at CODE_84E09E and CODE_84DF38 ($84:xxxx).
//
// Stream of chunks terminated by 0xFF:
//   header byte b:  command = b >> 5;  count = (b & 0x1F) + 1
//   command 7 = extended: command = (b & 0x1C) >> 2,
//                         count   = (((b & 3) << 8) | nextByte) + 1
//   0 copy count literals            1 repeat next byte count times
//   2 alternate two bytes count times 3 increasing fill (byte + i)
//   4 copy count bytes from output   5 same, bytewise inverted
//     little-endian offset
//   6 copy count bytes from destOffset - nextByte (overlaps ok)
// The original reads and writes $7F RAM directly, so a copy that points
// outside the output produced so far is legal -- it just returns whatever
// the buffer held.  We return 0x00 for those bytes (Bowser Castle needs it).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace smk {

struct Decomp {
    std::vector<uint8_t> out;
    size_t consumed = 0;   // input bytes, terminator included
    bool   ok = false;
};

// Decompress one stream. `buf`/`n` is the byte range the stream lives in
// (ROM image, or an already-decompressed stage-1 buffer for nested streams).
inline Decomp decompSmk(const uint8_t* buf, size_t n, size_t off, size_t maxOut) {
    Decomp r;
    if (!buf || off >= n) return Decomp();
    size_t ip = off;
    while (true) {
        if (ip >= n) return Decomp();
        uint8_t b = buf[ip++];
        if (b == 0xFF) { r.ok = true; break; }          // stop command
        int cmd = b >> 5;
        size_t count;
        if (cmd == 7) {
            cmd = (b & 0x1C) >> 2;
            if (ip >= n) return Decomp();
            count = (((size_t)(b & 3)) << 8) + buf[ip++];
            if (cmd > 6) return Decomp();
            count += 1;
        } else {
            count = (size_t)(b & 0x1F) + 1;
        }
        if (r.out.size() + count > maxOut) return Decomp();
        switch (cmd) {
            case 0:
                if (ip + count > n) return Decomp();
                r.out.insert(r.out.end(), buf + ip, buf + ip + count);
                ip += count;
                break;
            case 1:
                if (ip >= n) return Decomp();
                r.out.insert(r.out.end(), count, buf[ip++]);
                break;
            case 2: {
                if (ip + 2 > n) return Decomp();
                uint8_t a = buf[ip++], c = buf[ip++];
                for (size_t i = 0; i < count; i++) r.out.push_back((i & 1) ? c : a);
            } break;
            case 3: {
                if (ip >= n) return Decomp();
                uint8_t v = buf[ip++];
                for (size_t i = 0; i < count; i++) r.out.push_back((uint8_t)(v + i));
            } break;
            case 4:
            case 5: {
                if (ip + 2 > n) return Decomp();
                size_t p = (size_t)buf[ip] + ((size_t)buf[ip + 1] << 8);
                ip += 2;
                for (size_t i = 0; i < count; i++) {
                    // The original reads straight out of $7F RAM at destBase+p,
                    // so a reference past the current output is legal (it just
                    // picks up whatever the buffer held).  Bowser Castle's
                    // stream does this once; we read 0 there.
                    uint8_t v = (p + i < r.out.size()) ? r.out[p + i] : 0x00;
                    r.out.push_back((cmd == 5) ? (uint8_t)(v ^ 0xFF) : v);
                }
            } break;
            case 6: {
                if (ip >= n) return Decomp();
                size_t d = buf[ip++];
                if (d == 0 || d > r.out.size()) {
                    for (size_t i = 0; i < count; i++) r.out.push_back(0x00);
                    break;
                }
                size_t p = r.out.size() - d;
                for (size_t i = 0; i < count; i++) r.out.push_back(r.out[p + i]);
            } break;
            default: return Decomp();
        }
    }
    r.consumed = ip - off;
    return r;
}

inline Decomp decompSmk(const std::vector<uint8_t>& v, size_t off, size_t maxOut) {
    return decompSmk(v.data(), v.size(), off, maxOut);
}

} // namespace smk
