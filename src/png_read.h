// png_read.h -- minimal PNG decoder (DEFLATE + unfilter) so the game can use
// the reference renders in the project folder as a ground texture and as the
// kart sprite sheet.  The runtime still needs the user's ROM: these files
// carry no tile indices, no palettes and no game data, and Nintendo owns them.
//
// Supports bit depth 8 (and 1/2/4 for palette and greyscale), colour types
// 0/2/3/4/6, no interlacing -- which covers the reference PNGs.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pngread {

// ------------------------------------------------------------------- inflate
// Canonical DEFLATE (RFC 1951): stored, fixed and dynamic Huffman blocks.
namespace infl {

struct Stream {
    const uint8_t* d; size_t n; size_t pos = 0;
    uint32_t bitbuf = 0; int bitcnt = 0;
    bool bad = false;

    int getBit() {
        if (bitcnt == 0) {
            if (pos >= n) { bad = true; return 0; }
            bitbuf = d[pos++]; bitcnt = 8;
        }
        int b = bitbuf & 1; bitbuf >>= 1; bitcnt--; return b;
    }
    uint32_t getBits(int need) {
        uint32_t v = 0;
        for (int i = 0; i < need; i++) v |= (uint32_t)getBit() << i;
        return v;
    }
    void align() { bitcnt = 0; }
};

// Canonical Huffman table: counts per length plus symbols in order.
struct Huff {
    int count[16] = {};
    std::vector<int> symbol;
    void build(const uint8_t* lengths, int n) {
        for (int i = 0; i < 16; i++) count[i] = 0;
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        count[0] = 0;
        int offs[16] = {}, total = 0;
        for (int i = 1; i < 16; i++) { offs[i] = total; total += count[i]; }
        symbol.assign(total, 0);
        for (int i = 0; i < n; i++)
            if (lengths[i]) symbol[offs[lengths[i]]++] = i;
    }
    int decode(Stream& s) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= s.getBit();
            if (s.bad) return -1;
            int cnt = count[len];
            if (code - cnt < first) return symbol[index + (code - first)];
            index += cnt; first = (first + cnt) << 1; code <<= 1;
        }
        return -1;
    }
};

static const int kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                                 67,83,99,115,131,163,195,227,258};
static const int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
                                  4,4,4,4,5,5,5,5,0};
static const int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
                                  513,769,1025,1537,2049,3073,4097,6145,8193,12289,
                                  16385,24577};
static const int kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
                                   9,9,10,10,11,11,12,12,13,13};

inline bool inflate(const uint8_t* src, size_t len, std::vector<uint8_t>& out) {
    Stream s{src, len};
    Huff fixedLit, fixedDist;
    {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        fixedLit.build(l, 288);
        uint8_t d[30];
        for (int i = 0; i < 30; i++) d[i] = 5;
        fixedDist.build(d, 30);
    }
    for (;;) {
        int last = s.getBit(), type = (int)s.getBits(2);
        if (s.bad) return false;
        if (type == 0) {                          // stored
            s.align();
            if (s.pos + 4 > s.n) return false;
            unsigned len = s.d[s.pos] | (s.d[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + len > s.n) return false;
            out.insert(out.end(), s.d + s.pos, s.d + s.pos + len);
            s.pos += len;
        } else if (type == 1 || type == 2) {
            Huff dynLit, dynDist;
            const Huff *lit = &fixedLit, *dist = &fixedDist;
            if (type == 2) {
                static const int ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                int hlit = (int)s.getBits(5) + 257;
                int hdist = (int)s.getBits(5) + 1;
                int hclen = (int)s.getBits(4) + 4;
                uint8_t clen[19] = {};
                for (int i = 0; i < hclen; i++) clen[ord[i]] = (uint8_t)s.getBits(3);
                Huff cl; cl.build(clen, 19);
                std::vector<uint8_t> lens(hlit + hdist, 0);
                for (int i = 0; i < hlit + hdist; ) {
                    int sym = cl.decode(s);
                    if (sym < 0) return false;
                    if (sym < 16) lens[i++] = (uint8_t)sym;
                    else if (sym == 16) {
                        if (i == 0) return false;
                        uint8_t prev = lens[i - 1];
                        int rep = 3 + (int)s.getBits(2);
                        while (rep-- && i < hlit + hdist) lens[i++] = prev;
                    } else if (sym == 17) {
                        int rep = 3 + (int)s.getBits(3);
                        while (rep-- && i < hlit + hdist) lens[i++] = 0;
                    } else {
                        int rep = 11 + (int)s.getBits(7);
                        while (rep-- && i < hlit + hdist) lens[i++] = 0;
                    }
                }
                dynLit.build(lens.data(), hlit);
                dynDist.build(lens.data() + hlit, hdist);
                lit = &dynLit; dist = &dynDist;
            }
            for (;;) {
                int sym = lit->decode(s);
                if (sym < 0) return false;
                if (sym < 256) out.push_back((uint8_t)sym);
                else if (sym == 256) break;
                else {
                    sym -= 257;
                    if (sym >= 29) return false;
                    int length = kLenBase[sym] + (int)s.getBits(kLenExtra[sym]);
                    int dsym = dist->decode(s);
                    if (dsym < 0 || dsym >= 30) return false;
                    size_t distance = kDistBase[dsym] + s.getBits(kDistExtra[dsym]);
                    if (distance > out.size()) return false;
                    size_t start = out.size() - distance;
                    for (int i = 0; i < length; i++) out.push_back(out[start + i]);
                }
                if (s.bad) return false;
            }
        } else return false;
        if (last) break;
    }
    return !s.bad;
}

} // namespace infl

// ----------------------------------------------------------------- PNG decode
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;      // w*h*4
    bool ok = false;
    std::string err;
};

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

inline int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

inline Image read(const std::string& path) {
    Image img;
    auto fail = [&](const char* m) { img.err = m; return img; };

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return fail("cannot open");
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len < 8) { std::fclose(f); return fail("too short"); }
    std::vector<uint8_t> buf((size_t)len);
    size_t got = std::fread(buf.data(), 1, (size_t)len, f);
    std::fclose(f);
    if (got != (size_t)len) return fail("short read");

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (std::memcmp(buf.data(), sig, 8) != 0) return fail("not a PNG");

    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    std::vector<uint8_t> idat, plte, trns;
    size_t p = 8;
    while (p + 8 <= buf.size()) {
        uint32_t clen = be32(&buf[p]);
        std::string type((const char*)&buf[p + 4], 4);
        const uint8_t* body = &buf[p + 8];
        if (p + 12 + clen > buf.size()) return fail("truncated chunk");
        if (type == "IHDR" && clen >= 13) {
            w = (int)be32(body); h = (int)be32(body + 4);
            depth = body[8]; ctype = body[9];
            interlace = body[12];
        } else if (type == "PLTE") {
            plte.assign(body, body + clen);
        } else if (type == "tRNS") {
            trns.assign(body, body + clen);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), body, body + clen);
        } else if (type == "IEND") {
            break;
        }
        p += 12 + clen;
    }
    if (!w || !h) return fail("no IHDR");
    if (interlace) return fail("interlaced PNG unsupported");
    if (w > 8192 || h > 8192) return fail("image too large");
    if (idat.empty()) return fail("no IDAT");

    int channels;
    switch (ctype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return fail("unsupported colour type");
    }
    if (depth != 8 && !(ctype == 3 && (depth == 1 || depth == 2 || depth == 4)))
        return fail("unsupported bit depth");
    if (ctype == 3 && plte.empty()) return fail("palette image without PLTE");

    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * ((size_t)w * channels * depth / 8 + 1));
    // IDAT payloads are zlib streams: 2-byte header (CM=8), deflate data, adler32
    if (idat.size() < 3) return fail("IDAT too short");
    if ((idat[0] & 0x0F) != 8) return fail("unsupported zlib compression method");
    if (((idat[0] << 8) | idat[1]) % 31 != 0) return fail("bad zlib header check");
    if (!infl::inflate(idat.data() + 2, idat.size() - 2, raw)) return fail("inflate failed");

    size_t bpp = (size_t)channels * depth / 8;              // bytes per pixel
    if (bpp == 0) bpp = 1;
    size_t rowBytes = ((size_t)w * channels * depth + 7) / 8;
    if (raw.size() < (size_t)h * (rowBytes + 1)) return fail("truncated image data");

    img.w = w; img.h = h;
    img.rgba.assign((size_t)w * h * 4, 0);

    std::vector<uint8_t> prev(rowBytes, 0), cur(rowBytes, 0);
    for (int y = 0; y < h; y++) {
        const uint8_t* rp = &raw[(size_t)y * (rowBytes + 1)];
        int filter = rp[0];
        std::memcpy(cur.data(), rp + 1, rowBytes);
        for (size_t i = 0; i < rowBytes; i++) {
            int a = (i >= bpp) ? cur[i - bpp] : 0;
            int b = prev[i];
            int c = (i >= bpp) ? prev[i - bpp] : 0;
            switch (filter) {
                case 0: break;
                case 1: cur[i] = (uint8_t)(cur[i] + a); break;
                case 2: cur[i] = (uint8_t)(cur[i] + b); break;
                case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
                case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
                default: return fail("bad filter type");
            }
        }
        uint8_t* dst = &img.rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (depth == 8) {
                const uint8_t* s = &cur[(size_t)x * channels];
                if (ctype == 0) { r = g = b = s[0]; }
                else if (ctype == 2) { r = s[0]; g = s[1]; b = s[2]; }
                else if (ctype == 3) {
                    size_t idx = s[0];
                    if (idx * 3 + 2 < plte.size()) { r = plte[idx*3]; g = plte[idx*3+1]; b = plte[idx*3+2]; }
                    if (idx < trns.size()) a = trns[idx];
                } else if (ctype == 4) { r = g = b = s[0]; a = s[1]; }
                else { r = s[0]; g = s[1]; b = s[2]; a = s[3]; }
            } else {                                   // packed palette/grey
                int ppb = 8 / depth;
                int shift = 8 - depth * (x % ppb + 1);
                int v = (cur[(size_t)x / ppb] >> shift) & ((1 << depth) - 1);
                if (ctype == 3) {
                    size_t idx = (size_t)v;
                    if (idx * 3 + 2 < plte.size()) { r = plte[idx*3]; g = plte[idx*3+1]; b = plte[idx*3+2]; }
                    if (idx < trns.size()) a = trns[idx];
                } else {
                    r = g = b = (uint8_t)(v * 255 / ((1 << depth) - 1));
                }
            }
            dst[x * 4 + 0] = r; dst[x * 4 + 1] = g; dst[x * 4 + 2] = b; dst[x * 4 + 3] = a;
        }
        prev.swap(cur);
    }
    img.ok = true;
    return img;
}

} // namespace pngread
