// rom.h -- SNES ROM container: header parsing + LoROM/HiROM address mapping.
// Super Mario Kart (USA) is HiROM/FastROM; verified against the file's own
// header (0xFFC0: map mode 0x31) and its byte-sum checksum (0xEB44).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct SnesHeader {
    int         offset      = -1;   // file offset of the 0x30-byte header block
    std::string title;
    uint8_t     mapMode     = 0;
    uint8_t     cartType    = 0;
    uint8_t     romSizeCode = 0;
    uint8_t     sramSizeCode= 0;
    uint8_t     country     = 0;
    uint8_t     developer   = 0;
    uint8_t     version     = 0;
    uint16_t     checksum    = 0;
    uint16_t     complement  = 0;
    bool        valid       = false;
    bool        hirom       = false;
    bool        fastrom     = false;
    size_t      romBytes    = 0;
    size_t      sramBytes   = 0;
};

class Rom {
public:
    std::vector<uint8_t> data;
    SnesHeader           hdr;
    std::string          path;

    // ---------------------------------------------------------------- loading
    static uint32_t crc32(const uint8_t* p, size_t n) {
        static uint32_t t[256]; static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    bool load(const std::string& p, std::string* err = nullptr) {
        path = p;
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) { if (err) *err = "cannot open: " + p; return false; }
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0) { std::fclose(f); if (err) *err = "empty file"; return false; }
        data.resize((size_t)n);
        size_t got = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (got != data.size()) { if (err) *err = "short read"; return false; }
        detectHeader();
        if (!hdr.valid) { if (err) *err = "no valid internal header found"; return false; }
        return true;
    }

    // -------------------------------------------------------- header sniffing
    void detectHeader() {
        int cand[2] = {-1, -1};
        if (data.size() >= 0x8000)  cand[0] = 0x7FC0;   // LoROM position
        if (data.size() >= 0x10000) cand[1] = 0xFFC0;   // HiROM position
        for (int i = 0; i < 2; i++) {
            int off = cand[i];
            if (off < 0 || (size_t)off + 0x30 > data.size()) continue;
            const uint8_t* q = data.data() + off;
            uint8_t  mm  = q[0x15];
            uint16_t cmp = (uint16_t)(q[0x1C] | (q[0x1D] << 8));
            uint16_t sum = (uint16_t)(q[0x1E] | (q[0x1F] << 8));
            if ((mm & 0x30) != 0x20 && (mm & 0x30) != 0x30) continue;   // map mode
            if ((uint16_t)(cmp + sum) != 0xFFFF)              continue;   // checksum pair
            int printable = 0;
            for (int k = 0; k < 21; k++) {
                uint8_t c = q[k];
                if ((c >= 0x20 && c < 0x7F) || c == 0x00) printable++;
            }
            if (printable < 18) continue;

            hdr.offset       = off;
            hdr.mapMode      = mm;
            hdr.hirom        = ((mm & 0x30) == 0x30);
            hdr.fastrom      = (mm & 0x01) != 0;
            hdr.cartType     = q[0x16];
            hdr.romSizeCode  = q[0x17];
            hdr.sramSizeCode = q[0x18];
            hdr.country      = q[0x19];
            hdr.developer    = q[0x1A];
            hdr.version      = q[0x1B];
            hdr.complement   = cmp;
            hdr.checksum     = sum;
            hdr.romBytes     = hdr.romSizeCode ? ((size_t)1 << (hdr.romSizeCode + 10)) : data.size();
            hdr.sramBytes    = hdr.sramSizeCode ? ((size_t)1 << (hdr.sramSizeCode + 10)) : 0;
            hdr.title.clear();
            for (int k = 0; k < 21; k++) {
                uint8_t c = q[k];
                if (c == 0x00) break;
                hdr.title.push_back((char)c);
            }
            while (!hdr.title.empty() && hdr.title.back() == ' ') hdr.title.pop_back();
            hdr.valid = true;
            return;
        }
    }

    // ---------------------------------------------------------- address decode
    bool powerOfTwo() const { return data.size() && (data.size() & (data.size() - 1)) == 0; }

    // bank:addr -> file offset, or -1 if that region is not cartridge ROM.
    int64_t offsetOf(uint8_t bank, uint32_t addr) const {
        if (data.empty()) return -1;
        int64_t off = -1;
        if (hdr.hirom) {
            // 64 KB banks. $40-$7D / $C0-$FF expose a full bank;
            // $00-$3F / $80-$BF expose only the upper half ($8000-$FFFF).
            if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0)
                off = ((int64_t)(bank & 0x3F) << 16) | (addr & 0xFFFF);
            else if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && addr >= 0x8000)
                off = ((int64_t)(bank & 0x3F) << 16) | (addr & 0xFFFF);
        } else {
            // 32 KB banks, upper half only.
            if (addr >= 0x8000 && !(bank >= 0x7E && bank <= 0x7F))
                off = ((int64_t)(bank & 0x7F) << 15) | (addr & 0x7FFF);
        }
        if (off < 0) return -1;
        if (powerOfTwo()) off &= (int64_t)(data.size() - 1);
        else              off %= (int64_t)data.size();
        return off;
    }

    bool contains(uint8_t bank, uint32_t addr) const { return offsetOf(bank, addr) >= 0; }

    uint8_t read8(uint8_t bank, uint32_t addr) const {
        int64_t o = offsetOf(bank, addr);
        return o < 0 ? 0x00 : data[(size_t)o];
    }
    uint16_t read16(uint8_t bank, uint32_t addr) const {
        return (uint16_t)(read8(bank, addr) | (read8(bank, addr + 1) << 8));
    }
    uint8_t readFile(int64_t off) const {
        if (off < 0 || (size_t)off >= data.size()) return 0x00;
        return data[(size_t)off];
    }

    // Vector table helpers (bank 0 lower/upper vectors are at $FFEx-$FFFx).
    uint16_t vector(int fileOffset) const {
        if ((size_t)fileOffset + 1 >= data.size()) return 0;
        return (uint16_t)(data[fileOffset] | (data[fileOffset + 1] << 8));
    }

    static std::string hex8(uint32_t v, int digits = 2) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%0*X", digits, (unsigned)v);
        return buf;
    }
};
