// smk-disasm -- recursive-descent 65816 disassembler for Super Mario Kart.
//
//   smk-disasm <rom> [--start BB:AAAA] [--entry BB:AAAA]... [--linear BB:AAAA LEN]
//              [--nmi] [--irq] [--reset] [--m16|--m8] [--x16|--x8] [--limit N]
//
// Default behaviour: start at the reset vector and follow control flow.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../src/disasm.h"
#include "../src/rom.h"

static void usage() {
    std::printf(
        "usage: smk-disasm <rom> [options]\n"
        "  --start BB:AAAA   first address to disassemble (default: reset vector)\n"
        "  --entry  BB:AAAA   add another entry point (repeatable)\n"
        "  --entries-file F   read entry addresses from F, one hex address per line\n"
        "  --table  BB:AAAA,N add N 16-bit pointers from a dispatch table as entries\n"
        "  --linear BB:AAAA N decode N bytes linearly, no flow following\n"
        "  --reset/--nmi/--irq  use the corresponding CPU vector as --start\n"
        "  --m8 / --m16       initial accumulator width (default: m8 after reset)\n"
        "  --x8 / --x16       initial index width        (default: x8 after reset)\n"
        "  --limit N          max instructions (default 20000)\n"
        "  --no-zeros         don't stop at runs of zero bytes\n"
        "  --tables N         follow N pointers out of each indirect dispatch table\n"
        "  -o FILE            write listing to FILE instead of stdout\n");
}

int main(int argc, char** argv) {
    std::string romPath;
    std::string outFile;
    uint32_t start = 0, linearStart = 0;
    int linearLen = 0;
    bool haveStart = false, useVector[3] = {false, false, false}; // reset/nmi/irq
    bool m16 = false, x16 = false;   // reset stub ends in SEP #$30
    bool startM16 = false, startX16 = false;
    size_t limit = 20000;
    bool stopZeros = true;
    int tableFollow = 0;
    std::vector<uint32_t> entries;
    std::vector<std::pair<uint32_t, int>> tables;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](int n) { if (i + n >= argc) { usage(); std::exit(2); } };
        if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (a == "--start") { need(1); if (!snes::parseAddr(argv[++i], start)) { usage(); return 2; } haveStart = true; startM16 = m16; startX16 = x16; }
        else if (a == "--entry") { need(1); uint32_t e; if (!snes::parseAddr(argv[++i], e)) { usage(); return 2; } entries.push_back(e); }
        else if (a == "--entries-file") {
            need(1);
            FILE* f = std::fopen(argv[++i], "rb");
            if (!f) { std::fprintf(stderr, "error: cannot open %s\n", argv[i]); return 1; }
            char line[64];
            while (std::fgets(line, sizeof line, f)) {
                uint32_t e;
                if (snes::parseAddr(line, e)) entries.push_back(e);
            }
            std::fclose(f);
        }
        else if (a == "--table") {
            need(1);
            std::string spec = argv[++i];
            size_t comma = spec.rfind(',');
            if (comma == std::string::npos) { usage(); return 2; }
            uint32_t t;
            if (!snes::parseAddr(spec.substr(0, comma), t)) { usage(); return 2; }
            tables.push_back({t, std::atoi(spec.substr(comma + 1).c_str())});
        }
        else if (a == "--linear") {
            need(2);
            if (!snes::parseAddr(argv[++i], linearStart)) { usage(); return 2; }
            linearLen = std::atoi(argv[++i]);
        }
        else if (a == "--reset") useVector[0] = true;
        else if (a == "--nmi")   useVector[1] = true;
        else if (a == "--irq")   useVector[2] = true;
        else if (a == "--m16")   { m16 = true;  startM16 = true; }
        else if (a == "--m8")    { m16 = false; startM16 = false; }
        else if (a == "--x16")   { x16 = true;  startX16 = true; }
        else if (a == "--x8")    { x16 = false; startX16 = false; }
        else if (a == "--limit") { need(1); limit = (size_t)std::atol(argv[++i]); }
        else if (a == "--no-zeros") stopZeros = false;
        else if (a == "--tables") { need(1); tableFollow = std::atoi(argv[++i]); }
        else if (a == "-o")      { need(1); outFile = argv[++i]; }
        else if (!a.empty() && a[0] == '-') { usage(); return 2; }
        else                     romPath = a;
    }
    if (romPath.empty()) { usage(); return 2; }

    Rom rom;
    std::string err;
    if (!rom.load(romPath, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    std::fprintf(stderr, "%s: %s, %zu bytes, %s%s, header @ 0x%06X\n",
                 romPath.c_str(), rom.hdr.title.c_str(), rom.data.size(),
                 rom.hdr.hirom ? "HiROM" : "LoROM", rom.hdr.fastrom ? "+FastROM" : "",
                 (unsigned)rom.hdr.offset);

    // Vector table lives just before the header in HiROM files: $FFE4-$FFFF at 0xFFE4.
    auto vec = [&](int fileOff) { return (uint32_t)rom.vector(fileOff); };
    if (useVector[0]) { start = 0x000000 | vec(0xFFFC); haveStart = true; m16 = x16 = false; }
    if (useVector[1]) { start = 0x000000 | vec(0xFFFA); haveStart = true; m16 = x16 = false; }
    if (useVector[2]) { start = 0x000000 | vec(0xFFFE); haveStart = true; m16 = x16 = false; }
    if (!haveStart && entries.empty() && linearLen == 0) {
        start = 0x000000 | vec(0xFFFC);
        m16 = x16 = false;
        haveStart = true;
    }

    std::string out;

    if (linearLen > 0) {
        // ---- linear byte-decode: walk forward, widening flags as REP/SEP appear
        uint32_t addr = linearStart;
        int left = linearLen;
        while (left > 0) {
            snes::Insn in = snes::decode(rom, addr, m16, x16);
            if (in.len > left) break;
            if (in.bytes[0] == 0xC2 || in.bytes[0] == 0xE2) {
                uint8_t mask = in.bytes[1];
                if (in.bytes[0] == 0xC2) { if (mask & 0x20) m16 = true; if (mask & 0x10) x16 = true; }
                else                     { if (mask & 0x20) m16 = false; if (mask & 0x10) x16 = false; }
            }
            char b[64]; std::snprintf(b, sizeof b, "$%06X:  ", (unsigned)in.addr);
            std::string bytes;
            for (int i = 0; i < in.len; i++) bytes += snes::hx(in.bytes[i], 2) + " ";
            out += b;
            out += bytes;
            for (size_t i = bytes.size(); i < 16; i++) out += ' ';
            out += in.text + "\n";
            addr += in.len;
            left -= in.len;
        }
    } else {
        snes::Disassembler d;
        d.rom = &rom;
        d.limit = limit;
        d.stopOnZeros = stopZeros;
        d.followTables = tableFollow;
        if (haveStart) d.addEntry(start, startM16, startX16);
        for (uint32_t e : entries) d.addEntry(e, m16, x16);
        for (auto& t : tables) d.addTable(t.first, t.second);
        d.run();

        out += "; Super Mario Kart (USA) disassembly\n";
        if (haveStart) out += "; entry: $" + snes::hx(start, 6) + "\n";
        for (uint32_t e : entries) out += "; entry: $" + snes::hx(e, 6) + "\n";
        out += "\n";
        out += d.listing();

        std::fprintf(stderr, "%zu instructions, %zu labels\n", d.insns.size(), d.labels.size());
    }

    if (outFile.empty()) {
        std::fputs(out.c_str(), stdout);
    } else {
        FILE* f = std::fopen(outFile.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "error: cannot write %s\n", outFile.c_str()); return 1; }
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "wrote %s (%zu bytes)\n", outFile.c_str(), out.size());
    }
    return 0;
}
