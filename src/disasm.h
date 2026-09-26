// disasm.h -- 65816 disassembler: full opcode table + control-flow traversal.
//
// The traversal decodes instructions from one or more entry points, tracks the
// M/X width flags through REP/SEP, and follows branches/calls so that the
// listing comes out with real labels instead of a blind linear sweep.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "rom.h"

namespace snes {

// ---------------------------------------------------------------- opcode table
enum class Am : uint8_t {
    IMP, ACC, IMM, IMM8,            // implied / accumulator / (variable) imm
    DP, DPX, DPY,                   // $12   $12,X   $12,Y
    IDP, IDPX, IDPY,                // ($12) ($12,X) ($12),Y
    ILDP, ILDPY,                    // [$12] [$12],Y
    ABS, ABSX, ABSY,                // $1234 $1234,X $1234,Y
    IABS, IABSX, ILABS,             // ($1234) ($1234,X) [$1234]
    LONG, LONGX,                    // $123456 $123456,X
    REL, RELL,                      // 8-bit / 16-bit relative
    SR, SRY,                        // $12,S ($12,S),Y
    BLK                             // MVN/MVP banks
};

enum : uint8_t { IMMS_NONE = 0, IMMS_M = 1, IMMS_X = 2 };

struct Op {
    const char* mn;
    Am          am;
    uint8_t     imms;   // which width flag sizes an IMM operand
};

#define O(mn_, am_, im_) { mn_, Am::am_, im_ }

inline const Op kOps[256] = {
    /* 00 */ O("BRK",IMM8,0),  O("ORA",IDP,0),  O("COP",IMM8,0),  O("ORA",SR,0),
             O("TSB",DP,0),    O("ORA",DP,0),   O("ASL",DP,0),    O("ORA",ILDP,0),
             O("PHP",IMP,0),   O("ORA",IMM,IMMS_M), O("ASL",ACC,0), O("PHD",IMP,0),
             O("TSB",ABS,0),   O("ORA",ABS,0),  O("ASL",ABS,0),   O("ORA",LONG,0),
    /* 10 */ O("BPL",REL,0),   O("ORA",IDPY,0), O("ORA",IDP,0),   O("ORA",SRY,0),
             O("TRB",DP,0),    O("ORA",DPX,0),  O("ASL",DPX,0),   O("ORA",ILDPY,0),
             O("CLC",IMP,0),   O("ORA",ABSY,0), O("INC",ACC,0),   O("TCS",IMP,0),
             O("TRB",ABS,0),   O("ORA",ABSX,0), O("ASL",ABSX,0),  O("ORA",LONGX,0),
    /* 20 */ O("JSR",ABS,0),   O("AND",IDP,0),  O("JSL",LONG,0),  O("AND",SR,0),
             O("BIT",DP,0),    O("AND",DP,0),   O("ROL",DP,0),    O("AND",ILDP,0),
             O("PLP",IMP,0),   O("AND",IMM,IMMS_M), O("ROL",ACC,0),O("PLD",IMP,0),
             O("BIT",ABS,0),   O("AND",ABS,0),  O("ROL",ABS,0),   O("AND",LONG,0),
    /* 30 */ O("BMI",REL,0),   O("AND",IDPY,0), O("AND",IDP,0),   O("AND",SRY,0),
             O("BIT",DPX,0),   O("AND",DPX,0),  O("ROL",DPX,0),   O("AND",ILDPY,0),
             O("SEC",IMP,0),   O("AND",ABSY,0), O("DEC",ACC,0),   O("TSC",IMP,0),
             O("BIT",ABSX,0),  O("AND",ABSX,0), O("ROL",ABSX,0),  O("AND",LONGX,0),
    /* 40 */ O("RTI",IMP,0),   O("EOR",IDP,0),  O("WDM",IMM8,0),  O("EOR",SR,0),
             O("MVN",BLK,0),   O("EOR",DP,0),   O("LSR",DP,0),    O("EOR",ILDP,0),
             O("PHA",IMP,0),   O("EOR",IMM,IMMS_M), O("LSR",ACC,0),O("PHK",IMP,0),
             O("JMP",ABS,0),   O("EOR",ABS,0),  O("LSR",ABS,0),   O("EOR",LONG,0),
    /* 50 */ O("BVC",REL,0),   O("EOR",IDPY,0), O("EOR",IDP,0),   O("EOR",SRY,0),
             O("MVP",BLK,0),   O("EOR",DPX,0),  O("LSR",DPX,0),   O("EOR",ILDPY,0),
             O("CLI",IMP,0),   O("EOR",ABSY,0), O("PHY",IMP,0),   O("TCD",IMP,0),
             O("JML",LONG,0),  O("EOR",ABSX,0), O("LSR",ABSX,0),  O("EOR",LONGX,0),
    /* 60 */ O("RTS",IMP,0),   O("ADC",IDP,0),  O("PER",RELL,0),  O("ADC",SR,0),
             O("STZ",DP,0),    O("ADC",DP,0),   O("ROR",DP,0),    O("ADC",ILDP,0),
             O("PLA",IMP,0),   O("ADC",IMM,IMMS_M), O("ROR",ACC,0),O("RTL",IMP,0),
             O("JMP",IABS,0),  O("ADC",ABS,0),  O("ROR",ABS,0),   O("ADC",LONG,0),
    /* 70 */ O("BVS",REL,0),   O("ADC",IDPY,0), O("ADC",IDP,0),   O("ADC",SRY,0),
             O("STZ",DPX,0),   O("ADC",DPX,0),  O("ROR",DPX,0),   O("ADC",ILDPY,0),
             O("SEI",IMP,0),   O("ADC",ABSY,0), O("PLY",IMP,0),   O("TDC",IMP,0),
             O("JMP",IABSX,0), O("ADC",ABSX,0), O("ROR",ABSX,0),  O("ADC",LONGX,0),
    /* 80 */ O("BRA",REL,0),   O("STA",IDP,0),  O("BRL",RELL,0),  O("STA",SR,0),
             O("STY",DP,0),    O("STA",DP,0),   O("STX",DP,0),    O("STA",ILDP,0),
             O("DEY",IMP,0),   O("BIT",IMM,IMMS_M), O("TXA",IMP,0),O("PHB",IMP,0),
             O("STY",ABS,0),   O("STA",ABS,0),  O("STX",ABS,0),   O("STA",LONG,0),
    /* 90 */ O("BCC",REL,0),   O("STA",IDPY,0), O("STA",IDP,0),   O("STA",SRY,0),
             O("STY",DPX,0),   O("STA",DPX,0),  O("STX",DPY,0),   O("STA",ILDPY,0),
             O("TYA",IMP,0),   O("STA",ABSY,0), O("TXS",IMP,0),   O("TXY",IMP,0),
             O("STZ",ABS,0),   O("STA",ABSX,0), O("STZ",ABSX,0),  O("STA",LONGX,0),
    /* A0 */ O("LDY",IMM,IMMS_X), O("LDA",IDP,0), O("LDX",IMM,IMMS_X), O("LDA",SR,0),
             O("LDY",DP,0),    O("LDA",DP,0),   O("LDX",DP,0),    O("LDA",ILDP,0),
             O("TAY",IMP,0),   O("LDA",IMM,IMMS_M), O("TAX",IMP,0),O("PLB",IMP,0),
             O("LDY",ABS,0),   O("LDA",ABS,0),  O("LDX",ABS,0),   O("LDA",LONG,0),
    /* B0 */ O("BCS",REL,0),   O("LDA",IDPY,0), O("LDA",IDP,0),   O("LDA",SRY,0),
             O("LDY",DPX,0),   O("LDA",DPX,0),  O("LDX",DPY,0),   O("LDA",ILDPY,0),
             O("CLV",IMP,0),   O("LDA",ABSY,0), O("TSX",IMP,0),   O("TYX",IMP,0),
             O("LDY",ABSX,0),  O("LDA",ABSX,0), O("LDX",ABSY,0),  O("LDA",LONGX,0),
    /* C0 */ O("CPY",IMM,IMMS_M), O("CMP",IDP,0), O("REP",IMM8,0),O("CMP",SR,0),
             O("CPY",DP,0),    O("CMP",DP,0),   O("DEC",DP,0),    O("CMP",ILDP,0),
             O("INY",IMP,0),   O("CMP",IMM,IMMS_M), O("DEX",IMP,0),O("WAI",IMP,0),
             O("CPY",ABS,0),   O("CMP",ABS,0),  O("DEC",ABS,0),   O("CMP",LONG,0),
    /* D0 */ O("BNE",REL,0),   O("CMP",IDPY,0), O("CMP",IDP,0),   O("CMP",SRY,0),
             O("PEI",IDP,0),   O("CMP",DPX,0),  O("DEC",DPX,0),   O("CMP",ILDPY,0),
             O("CLD",IMP,0),   O("CMP",ABSY,0), O("PHX",IMP,0),   O("STP",IMP,0),
             O("JML",ILABS,0), O("CMP",ABSX,0), O("DEC",ABSX,0),  O("CMP",LONGX,0),
    /* E0 */ O("CPX",IMM,IMMS_X), O("SBC",IDP,0), O("SEP",IMM8,0),O("SBC",SR,0),
             O("CPX",DP,0),    O("SBC",DP,0),   O("INC",DP,0),    O("SBC",ILDP,0),
             O("INX",IMP,0),   O("SBC",IMM,IMMS_M), O("NOP",IMP,0),O("XBA",IMP,0),
             O("CPX",ABS,0),   O("SBC",ABS,0),  O("INC",ABS,0),   O("SBC",LONG,0),
    /* F0 */ O("BEQ",REL,0),   O("SBC",IDPY,0), O("SBC",IDP,0),   O("SBC",SRY,0),
             O("PEA",ABS,0),   O("SBC",DPX,0),  O("INC",DPX,0),   O("SBC",ILDPY,0),
             O("SED",IMP,0),   O("SBC",ABSY,0), O("PLX",IMP,0),     O("XCE",IMP,0),
             O("JSR",IABSX,0), O("SBC",ABSX,0), O("INC",ABSX,0),  O("SBC",LONGX,0),
};

#undef O

// ------------------------------------------------------------- instruction repr
struct Insn {
    uint32_t    addr      = 0;
    uint8_t     bytes[6]  = {};
    int         len       = 1;
    uint32_t    operand   = 0;
    uint32_t    target    = 0;
    bool        hasTarget = false;
    bool        isBranch  = false;   // conditional transfer
    bool        isJump    = false;   // unconditional transfer, no fallthrough
    bool        isCall    = false;   // transfer with a return, fallthrough kept
    bool        isStop    = false;   // RTS/RTL/RTI/STP/WAI
    bool        m16       = false;   // width flags in effect at this instruction
    bool        x16       = false;
    std::string text;                // "LDA #$1234"
    std::string note;                // trailing comment
};

inline std::string hx(uint32_t v, int digits) {
    char b[16];
    std::snprintf(b, sizeof b, "%0*X", digits, (unsigned)v);
    return b;
}

inline int operandBytes(Am am) {
    switch (am) {
    case Am::IMP:  case Am::ACC:                        return 0;
    case Am::IMM8: case Am::DP:  case Am::DPX: case Am::DPY:
    case Am::IDP:  case Am::IDPX: case Am::IDPY:
    case Am::ILDP: case Am::ILDPY: case Am::SR:  case Am::SRY:
    case Am::REL:                                        return 1;
    case Am::ABS:  case Am::ABSX: case Am::ABSY:
    case Am::IABS: case Am::IABSX:case Am::ILABS:
    case Am::RELL: case Am::BLK:                         return 2;
    case Am::LONG: case Am::LONGX:                       return 3;
    case Am::IMM:                                        return -1; // width-dependent
    }
    return 0;
}

// Decode one instruction at a 24-bit address using the given width flags.
inline Insn decode(const Rom& rom, uint32_t addr, bool m16, bool x16) {
    Insn in;
    in.addr = addr;
    in.m16 = m16;
    in.x16 = x16;

    const uint8_t bank = (uint8_t)(addr >> 16);
    const uint32_t a   = addr & 0xFFFF;
    const uint8_t op   = rom.read8(bank, a);
    const Op&     o    = kOps[op];

    in.bytes[0] = op;
    int osz = operandBytes(o.am);
    if (osz < 0) osz = (o.imms == IMMS_M) ? (m16 ? 2 : 1) : (x16 ? 2 : 1);

    in.len = 1 + osz;
    for (int i = 0; i < osz && i < 5; i++)
        in.bytes[1 + i] = rom.read8(bank, (a + 1 + i) & 0xFFFF);
    for (int i = 0; i < osz; i++)
        in.operand |= (uint32_t)in.bytes[1 + i] << (8 * i);

    const std::string mn = o.mn;
    std::string txt = mn;
    char buf[64];

    switch (o.am) {
    case Am::IMP:  break;
    case Am::ACC:  txt += " A"; break;
    case Am::IMM:
        if (osz == 2) std::snprintf(buf, sizeof buf, " #$%04X", (unsigned)in.operand);
        else          std::snprintf(buf, sizeof buf, " #$%02X", (unsigned)in.operand);
        txt += buf;
        break;
    case Am::IMM8:
    case Am::REL:
    case Am::RELL:
        std::snprintf(buf, sizeof buf, " #$%02X", (unsigned)in.operand);
        txt += buf;
        break;
    case Am::DP:   std::snprintf(buf, sizeof buf, " $%02X", in.operand); txt += buf; break;
    case Am::DPX:  std::snprintf(buf, sizeof buf, " $%02X,X", in.operand); txt += buf; break;
    case Am::DPY:  std::snprintf(buf, sizeof buf, " $%02X,Y", in.operand); txt += buf; break;
    case Am::IDP:  std::snprintf(buf, sizeof buf, " ($%02X)", in.operand); txt += buf; break;
    case Am::IDPX: std::snprintf(buf, sizeof buf, " ($%02X,X)", in.operand); txt += buf; break;
    case Am::IDPY: std::snprintf(buf, sizeof buf, " ($%02X),Y", in.operand); txt += buf; break;
    case Am::ILDP: std::snprintf(buf, sizeof buf, " [$%02X]", in.operand); txt += buf; break;
    case Am::ILDPY:std::snprintf(buf, sizeof buf, " [$%02X],Y", in.operand); txt += buf; break;
    case Am::SR:   std::snprintf(buf, sizeof buf, " $%02X,S", in.operand); txt += buf; break;
    case Am::SRY:  std::snprintf(buf, sizeof buf, " ($%02X,S),Y", in.operand); txt += buf; break;
    case Am::ABS:  std::snprintf(buf, sizeof buf, " $%04X", in.operand); txt += buf; break;
    case Am::ABSX: std::snprintf(buf, sizeof buf, " $%04X,X", in.operand); txt += buf; break;
    case Am::ABSY: std::snprintf(buf, sizeof buf, " $%04X,Y", in.operand); txt += buf; break;
    case Am::IABS: std::snprintf(buf, sizeof buf, " ($%04X)", in.operand); txt += buf; break;
    case Am::IABSX:std::snprintf(buf, sizeof buf, " ($%04X,X)", in.operand); txt += buf; break;
    case Am::ILABS:std::snprintf(buf, sizeof buf, " [$%04X]", in.operand); txt += buf; break;
    case Am::LONG: std::snprintf(buf, sizeof buf, " $%06X", in.operand); txt += buf; break;
    case Am::LONGX:std::snprintf(buf, sizeof buf, " $%06X,X", in.operand); txt += buf; break;
    case Am::BLK:  // operand is stored dest-bank first; assembly prints src,dest
        std::snprintf(buf, sizeof buf, " $%02X,$%02X",
                      (unsigned)((in.operand >> 8) & 0xFF), (unsigned)(in.operand & 0xFF));
        txt += buf;
        break;
    }
    in.text = txt;

    // ---------------------------------------------------------- control flow
    const bool relBranch = (o.am == Am::REL && mn != "BRA");
    if (relBranch) {
        int32_t off = (int8_t)(in.operand & 0xFF);
        in.isBranch = in.hasTarget = true;
        in.target = (addr & 0xFF0000u) | ((a + 2 + off) & 0xFFFF);
    } else if (mn == "BRA" || mn == "BRL") {
        int32_t off = (mn == "BRA") ? (int8_t)(in.operand & 0xFF) : (int16_t)(in.operand & 0xFFFF);
        in.isJump = in.hasTarget = true;
        in.target = (addr & 0xFF0000u) | ((a + in.len + off) & 0xFFFF);
    } else if (mn == "PER") {
        // pushes a relative address then falls through; not a transfer
    } else if (mn == "JMP" || mn == "JML") {
        in.isJump = true;
        if (o.am == Am::ABS)   { in.hasTarget = true; in.target = (addr & 0xFF0000u) | in.operand; }
        if (o.am == Am::LONG)  { in.hasTarget = true; in.target = in.operand; }
    } else if (mn == "JSR" || mn == "JSL") {
        in.isCall = true;
        if (o.am == Am::ABS)   { in.hasTarget = true; in.target = (addr & 0xFF0000u) | in.operand; }
        if (o.am == Am::LONG)  { in.hasTarget = true; in.target = in.operand; }
    } else if (mn == "RTS" || mn == "RTL" || mn == "RTI" || mn == "STP" || mn == "WAI") {
        in.isStop = true;
    }

    // annotate jump/call targets with their ROM file offset
    if (in.hasTarget) {
        int64_t fo = rom.offsetOf((uint8_t)(in.target >> 16), in.target & 0xFFFF);
        if (fo >= 0) in.note = "rom " + hx((uint32_t)fo, 6);
        else         in.note = "not in rom";
    } else if (in.isJump) {
        in.note = "indirect";
    }
    return in;
}

// ------------------------------------------------------------------- traversal
class Disassembler {
public:
    const Rom* rom        = nullptr;
    size_t     limit      = 20000;
    bool       followFlow = true;
    bool       stopOnZeros = true;
    // When a JSR/JMP (abs,X) or JML [abs] is hit, treat `abs` as a dispatch
    // table and follow this many 16-bit (or 24-bit for JML) pointers out of it.
    int        followTables = 0;

    std::map<uint32_t, Insn> insns;             // address -> decoded instruction
    std::set<uint32_t>       labels;            // addresses that deserve a label
    std::vector<std::string> warnings;
    std::vector<uint32_t>    entries;

    void addEntry(uint32_t addr, bool m16 = true, bool x16 = true) {
        entries.push_back((addr & 0xFFFFFF) | ((uint32_t)(m16 ? 1 : 0) << 24)
                                            | ((uint32_t)(x16 ? 1 : 0) << 25));
        labels.insert(addr & 0xFFFFFF);
    }

    // Register every 16-bit pointer in a dispatch table as an entry point.
    void addTable(uint32_t tableAddr, int count) {
        const uint8_t bank = (uint8_t)(tableAddr >> 16);
        for (int i = 0; i < count; i++) {
            uint16_t lo = rom->read16(bank, (tableAddr + i * 2) & 0xFFFF);
            if (lo == 0 || lo == 0xFFFF) continue;
            addEntry(((uint32_t)bank << 16) | lo, true, true);
        }
    }

    void run() {
        struct Work { uint32_t addr; bool m16, x16; };
        std::queue<Work> q;
        for (uint32_t e : entries)
            q.push({e & 0xFFFFFF, ((e >> 24) & 1) != 0, ((e >> 25) & 1) != 0});

        while (!q.empty()) {
            Work w = q.front(); q.pop();
            while (true) {
                if (insns.count(w.addr)) break;                  // already decoded
                if (insns.size() >= limit) { warnings.push_back("instruction limit reached"); return; }
                if (rom->offsetOf((uint8_t)(w.addr >> 16), w.addr & 0xFFFF) < 0) {
                    warnings.push_back("$" + hx(w.addr, 6) + ": address is not ROM-mapped");
                    break;
                }
                // bail out of long zero runs (alignment padding)
                if (stopOnZeros && rom->read8((uint8_t)(w.addr >> 16), w.addr & 0xFFFF) == 0x00) {
                    bool allZero = true;
                    for (int i = 0; i < 8; i++)
                        if (rom->read8((uint8_t)(w.addr >> 16), (w.addr + i) & 0xFFFF) != 0x00) {
                            allZero = false; break;
                        }
                    if (allZero) break;
                }

                Insn in = decode(*rom, w.addr, w.m16, w.x16);
                insns[w.addr] = in;

                // width flags evolve through REP/SEP
                bool nm = w.m16, nx = w.x16;
                if (in.bytes[0] == 0xC2 || in.bytes[0] == 0xE2) {
                    const uint8_t mask = in.bytes[1];
                    const bool set = (in.bytes[0] == 0xC2);
                    if (mask & 0x20) nm = set;
                    if (mask & 0x10) nx = set;
                }

                if (!followFlow) {
                    w.addr = (w.addr & 0xFF0000u) | ((w.addr + in.len) & 0xFFFF);
                    w.m16 = nm; w.x16 = nx;
                    if ((w.addr & 0xFFFF) < (in.addr & 0xFFFF)) break;   // bank wrap
                    continue;
                }

                if (in.hasTarget) labels.insert(in.target);
                if (in.hasTarget && (in.isBranch || in.isJump || in.isCall))
                    q.push({in.target, nm, nx});

                // Indirect dispatch: follow the pointer table if asked.
                if (followTables > 0 && in.bytes[0] != 0x00) {
                    const Op& o = kOps[in.bytes[0]];
                    const bool isIndirect =
                        (o.am == Am::IABSX || o.am == Am::IABS || o.am == Am::ILABS) &&
                        (std::string(o.mn) == "JSR" || std::string(o.mn) == "JMP" ||
                         std::string(o.mn) == "JML");
                    if (isIndirect && rom->offsetOf((uint8_t)(w.addr >> 16), in.operand) >= 0) {
                        const uint8_t tb = (uint8_t)(w.addr >> 16);
                        for (int i = 0; i < followTables; i++) {
                            uint32_t t;
                            if (o.am == Am::ILABS) {
                                t = (uint32_t)rom->read8(tb, in.operand + i * 3) |
                                    ((uint32_t)rom->read8(tb, in.operand + i * 3 + 1) << 8) |
                                    ((uint32_t)rom->read8(tb, in.operand + i * 3 + 2) << 16);
                            } else {
                                t = ((uint32_t)tb << 16) |
                                    rom->read16(tb, (in.operand + i * 2) & 0xFFFF);
                            }
                            if ((t & 0xFFFF) == 0 || (t & 0xFFFF) == 0xFFFF) continue;
                            if (rom->offsetOf((uint8_t)(t >> 16), t & 0xFFFF) < 0) continue;
                            if (insns.count(t)) continue;
                            labels.insert(t);
                            q.push({t, nm, nx});
                        }
                    }
                }

                if (in.isStop || in.isJump) break;                        // no fallthrough

                uint32_t next = (w.addr & 0xFF0000u) | ((w.addr + in.len) & 0xFFFF);
                if (next <= w.addr) break;                                // bank wrap
                w.addr = next; w.m16 = nm; w.x16 = nx;
            }
        }
    }

    std::string labelFor(uint32_t addr) const {
        return (addr == (entries.empty() ? 0xFFFFFFFFu : (entries[0] & 0xFFFFFF)))
                   ? "entry_" + hx(addr, 6)
                   : "loc_" + hx(addr, 6);
    }

    // Human-readable listing with labels and data gaps.
    std::string listing() const {
        std::string out;
        char line[256];

        if (!warnings.empty()) {
            for (const auto& w : warnings) out += "; warning: " + w + "\n";
            out += "\n";
        }

        uint32_t prevEnd = 0;
        bool first = true;
        for (const auto& kv : insns) {
            const Insn& in = kv.second;
            // Only show a gap when it is plausible: same 64 KB bank and not huge.
            bool showGap = !first && in.addr > prevEnd &&
                           (in.addr >> 16) == (prevEnd >> 16) &&
                           (in.addr - prevEnd) <= 0x1000;
            if (!first && in.addr > prevEnd && !showGap) {
                out += "; ---- " + std::to_string(in.addr - prevEnd) +
                       " bytes skipped, next code at $" + hx(in.addr, 6) + " ----\n";
            }
            if (showGap) {
                uint32_t gapStart = prevEnd, n = in.addr - prevEnd;
                out += "; ---- " + std::to_string(n) + " data bytes @ $" + hx(gapStart, 6) + " ----\n";
                for (uint32_t o = 0; o < n && o < 128; o += 8) {
                    std::string db = "$" + hx(gapStart + o, 6) + ": .byte ";
                    for (uint32_t k = 0; k < 8 && o + k < n; k++) {
                        uint8_t b = rom->read8((uint8_t)((gapStart + o + k) >> 16),
                                               (gapStart + o + k) & 0xFFFF);
                        db += "$" + hx(b, 2);
                        if (o + k + 1 < n && o + k + 1 < 128) db += ",";
                    }
                    out += db + "\n";
                }
                if (n > 128) out += "; ... (" + std::to_string(n - 128) + " more)\n";
            }
            first = false;
            prevEnd = in.addr + in.len;

            if (labels.count(in.addr)) out += labelFor(in.addr) + ":\n";

            std::string bytes;
            for (int i = 0; i < in.len; i++) bytes += hx(in.bytes[i], 2) + " ";
            for (int i = in.len; i < 5; i++) bytes += "   ";

            std::snprintf(line, sizeof line, "$%06X:  %s  %-28s", (unsigned)in.addr,
                          bytes.c_str(), in.text.c_str());
            out += line;
            if (in.hasTarget) out += "  ; -> " + labelFor(in.target) + "  " + in.note;
            else if (!in.note.empty()) out += "  ; " + in.note;
            out += "\n";
        }
        return out;
    }
};

// ------------------------------------------------------------------- helpers
// Parse "$BB:AAAA", "BB:AAAA", "0xBBBBAAAA" or "BBBBAAAA" into a 24-bit address.
inline bool parseAddr(const std::string& s, uint32_t& out) {
    std::string t;
    for (char c : s) {
        if (std::isspace((unsigned char)c) || c == '$') continue;
        t.push_back(c);
    }
    if (t.empty()) return false;
    size_t colon = t.find(':');
    unsigned long hi = 0, lo = 0;
    if (colon != std::string::npos) {
        hi = std::strtoul(t.substr(0, colon).c_str(), nullptr, 16);
        lo = std::strtoul(t.substr(colon + 1).c_str(), nullptr, 16);
        out = (uint32_t)(((hi & 0xFF) << 16) | (lo & 0xFFFF));
        return true;
    }
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    for (char c : t)
        if (!isxdigit((unsigned char)c)) return false;
    lo = std::strtoul(t.c_str(), nullptr, 16);
    out = (uint32_t)(lo & 0xFFFFFF);
    return t.size() <= 6;
}

} // namespace snes
