// lztrace.cpp -- verbose SMK decompression tracer (debug tool).
// Prints every command of a stream so decode failures can be located.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../src/rom.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: lztrace <rom> <offset> [maxOut]\n");
        return 2;
    }
    Rom rom;
    std::string err;
    if (!rom.load(argv[1], &err)) { std::printf("error: %s\n", err.c_str()); return 1; }
    size_t off = std::strtoul(argv[2], nullptr, 0);
    size_t maxOut = argc > 3 ? std::strtoul(argv[3], nullptr, 0) : 0x10000;
    const uint8_t* buf = rom.data.data();
    size_t n = rom.data.size(), ip = off, outSize = 0;
    int oob = 0;

    for (int step = 0; ; step++) {
        if (ip >= n) { std::printf("[%4d] %zX: ran past end of ROM, out=%zX\n", step, ip, outSize); return 1; }
        size_t hip = ip;
        uint8_t b = buf[ip++];
        if (b == 0xFF) {
            std::printf("[%4d] %zX: FF stop. out=%zX (%zu bytes), consumed=%zX, oob-cmds=%d\n",
                        step, hip, outSize, outSize, ip - off, oob);
            return 0;
        }
        int cmd = b >> 5;
        size_t count;
        if (cmd == 7) {
            cmd = (b & 0x1C) >> 2;
            if (ip >= n) { std::printf("[%4d] %zX: eof in ext count\n", step, hip); return 1; }
            count = ((size_t)(b & 3) << 8) + buf[ip++] + 1;
        } else {
            count = (size_t)(b & 0x1F) + 1;
        }
        if (outSize + count > maxOut) {
            std::printf("[%4d] %zX: cmd=%d count=%zX out=%zX EXCEEDS max %zX\n",
                        step, hip, cmd, count, outSize, maxOut);
            return 1;
        }
        switch (cmd) {
            case 0:
                if (ip + count > n) { std::printf("[%4d] %zX: lit eof\n", step, hip); return 1; }
                std::printf("[%4d] %zX: lit  count=%zX\n", step, hip, count);
                ip += count; outSize += count; break;
            case 1:
                if (ip >= n) { std::printf("[%4d] %zX: fill eof\n", step, hip); return 1; }
                std::printf("[%4d] %zX: fill count=%zX val=%02X\n", step, hip, count, buf[ip]);
                ip++; outSize += count; break;
            case 2:
                if (ip + 2 > n) { std::printf("[%4d] %zX: alt eof\n", step, hip); return 1; }
                std::printf("[%4d] %zX: alt  count=%zX %02X %02X\n", step, hip, count, buf[ip], buf[ip+1]);
                ip += 2; outSize += count; break;
            case 3:
                if (ip >= n) { std::printf("[%4d] %zX: inc eof\n", step, hip); return 1; }
                std::printf("[%4d] %zX: inc  count=%zX start=%02X\n", step, hip, count, buf[ip]);
                ip++; outSize += count; break;
            case 4: case 5: {
                if (ip + 2 > n) { std::printf("[%4d] %zX: abs eof\n", step, hip); return 1; }
                size_t p = (size_t)buf[ip] + ((size_t)buf[ip+1] << 8);
                ip += 2;
                if (p >= outSize || p + count > outSize) {
                    std::printf("[%4d] %zX: abs  cmd=%d p=%zX count=%zX out=%zX  <-- reads $7F RAM "
                                "past current output (lenient: zeros)\n",
                                step, hip, cmd, p, count, outSize);
                    oob++;
                    outSize += count;
                    break;
                }
                std::printf("[%4d] %zX: abs  cmd=%d p=%zX count=%zX\n", step, hip, cmd, p, count);
                outSize += count; break;
            }
            case 6: {
                if (ip >= n) { std::printf("[%4d] %zX: rel eof\n", step, hip); return 1; }
                size_t d = buf[ip++];
                if (d == 0 || d > outSize) {
                    std::printf("[%4d] %zX: rel  d=%zX count=%zX out=%zX  <-- pointer wraps before "
                                "buffer start (lenient: zeros)\n",
                                step, hip, d, count, outSize);
                    oob++;
                    outSize += count;
                    break;
                }
                std::printf("[%4d] %zX: rel  d=%zX count=%zX\n", step, hip, d, count);
                outSize += count; break;
            }
            default:
                std::printf("[%4d] %zX: bad cmd %d\n", step, hip, cmd);
                return 1;
        }
    }
}
