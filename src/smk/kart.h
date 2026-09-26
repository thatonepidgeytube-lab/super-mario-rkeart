// kart.h -- the driving mechanics, ported instruction-for-instruction from
// the original 65816 routines:
//
//   CODE_81F01F  copies per-character stats into the kart's WRAM block
//                ($0710 for player 1): $B4 = top speed, +$00.. = acceleration
//                curve, +$40/48/50 = three turn-rate rows.
//   CODE_80A80F  shapes $B2 (the turn-rate accumulator) from brake/accel/coast
//                rows depending on which shoulder/direction input is held.
//   CODE_80A892  normal branch: $A4 += SAR3($B2)  (heading advances by the
//                arithmetic-shifted turn rate), plus DSP-1 style damping.
//   CODE_80A6F7  / DATA_80A67D  coast deceleration, indexed by the speed's
//                high byte, i.e. drag grows sharply near and above top speed.
//   CODE_80879A  position integration: world pos += velocity >> 8 per frame.
//
// Angle convention (from CODE_81F638 / atan2): 16-bit angle, full circle =
// 0x10000, quadrants at $0000/$4000/$8000/$C000, and
//     velocity = speed * (sin a, -cos a)     (y grows south, a=0 faces north)
#pragma once
#include <cmath>
#include <cstdint>

#include "../rom.h"

namespace smk {

struct Stats {
    int32_t maxSpeed  = 912;         // $B4, units/frame
    int16_t accelQ8[16] = {};        // Q8 speed added per frame on throttle
    int16_t turnMax[3]  = {};        // turn-rate rows at block +$40/48/50
    uint8_t turnBrake[3] = {}, turnAccel[3] = {}, turnCoast[3] = {};
    int16_t aux[6] = {};             // block +$34 (byte*16, +0x30 in 150cc)
};

// DATA_80A67D: coast drag in units/frame, index = speed high byte ($EB)
static const int16_t kCoastDrag[8] = { -4, -6, -8, -10, -16, -40, -80, -120 };

// DATA_80A7FF: turn-rate damping factor applied through DSP-1 cmd $00.
// Values are signed; the original feeds them to a 16x16 multiply.  We apply
// the high part of the product scaled so low-speed steering stays crisp and
// top-speed steering stiffens (see update()).
static const int16_t kTurnDamp[8] = {
    0x0000, (int16_t)0xFF80, (int16_t)0xFF00, (int16_t)0xFE80,
    (int16_t)0xFE00, (int16_t)0xFD80, (int16_t)0xFD00, (int16_t)0xFC80,
};

// DATA_80A9B8: low-speed steering ramp
static const int16_t kSteerRamp[8] = { 0x0000, 0x0010, 0x0020, 0x0030,
                                       0x0038, 0x003C, 0x003E, 0x0040 };

static const int32_t kBrakeRate   = 16 * 256;   // brake: 16 units/frame
static const int32_t kReverseMax  = 96 * 256;   // slowest safe reverse cap

// DATA_818000 lives at $81:8000 -> file 0x18000; the tables it points at
// run through 0x18100, so anything smaller than that cannot hold stats.
static const size_t OFF_STATS     = 0x18000;
static const size_t OFF_STATS_END = 0x18100;

// ------------------------------------------------------------------- stats
// cc: 0 = 50cc, 2 = 100cc, 4 = 150cc (the original compares $30 with #$0004).
inline Stats loadStats(const Rom& rom, int chr, int cc) {
    Stats s;
    const std::vector<uint8_t>& d = rom.data;
    if (d.size() < OFF_STATS_END) return s;
    if (chr < 0) chr = 0;
    if (chr > 7) chr = 7;
    auto r16 = [&](size_t o) { return (uint16_t)(d[o] | (d[o + 1] << 8)); };

    // ---- $B4 top speed: DATA_818000[chr], then the $2C/$30 class adjust
    int32_t top = (int32_t)r16(OFF_STATS + (size_t)chr * 2);
    if (cc >= 4)      top += 0x00A0;
    else if (cc == 0) top -= 0x0080;
    s.maxSpeed = top;

    // ---- acceleration curve: 16 bytes at DATA_818010[chr] (a $8180xx ptr)
    size_t p10 = 0x10000u | (size_t)r16(OFF_STATS + 0x10 + (size_t)chr * 2);
    if (p10 + 16 <= d.size()) {
        for (int i = 0; i < 16; i++) {
            int v = (int)d[p10 + i] * 16;          // ASL x4
            if (cc >= 4) v += v / 2;               // CODE_81EFFF: x + x/2
            s.accelQ8[i] = (int16_t)v;
        }
    }

    // ---- block +$34: 6 bytes at DATA_818060[chr] (CODE_81F011 adds $30)
    size_t p60 = 0x10000u | (size_t)r16(OFF_STATS + 0x60 + (size_t)chr * 2);
    if (p60 + 6 <= d.size())
        for (int i = 0; i < 6; i++)
            s.aux[i] = (int16_t)((int)d[p60 + i] * 16 + (cc >= 4 ? 0x30 : 0));

    // ---- three turn-rate rows: 5-byte records at DATA_818088[chr]
    size_t p88 = 0x10000u | (size_t)r16(OFF_STATS + 0x88 + (size_t)chr * 2);
    if (p88 + 15 <= d.size()) {
        for (int row = 0; row < 3; row++) {
            size_t o = p88 + (size_t)row * 5;
            s.turnMax[row]   = (int16_t)r16(o);
            s.turnBrake[row] = d[o + 2];
            s.turnAccel[row] = d[o + 3];
            s.turnCoast[row] = d[o + 4];
        }
    }
    return s;
}

// -------------------------------------------------------------------- kart
struct Input {
    bool accel = false;   // throttle
    bool brake = false;   // brake / reverse
    bool left  = false;
    bool right = false;
};

// Surface kinds the driver can be on.  Derived from the track artwork, not from
// the ROM's level data: the picture shows road, grass and sand directly.
enum Surface { SURF_ROAD = 0, SURF_GRASS = 1, SURF_SAND = 2, SURF_VOID = 3 };

// Grass has an invisible wall around it.  Sand is only a speed penalty: you
// keep moving, just slower than the road.
//
// It has to be a cap and *nothing else*.  A per-frame drag cannot work here:
// the engine's acceleration at a standstill is only 2 units/frame (accelQ8[0]
// is 512 in Q8), so any drag at or above that cancels the throttle outright and
// the kart simply never gets going.  Capping alone can never stall it, and it
// still slows you down everywhere else because the throttle keeps pushing
// against the cap.
static const int kSandSpeedPct = 60;          // % of the engine's top speed

struct Kart {
    Stats    st;
    int32_t  speed   = 0;      // Q8: integer units/frame = speed >> 8
    int16_t  turn    = 0;      // $B2 turn-rate accumulator
    uint16_t heading = 0;      // $A4, 0 = north
    int32_t  x = 0, y = 0;     // Q16 pixels inside the 1024x1024 course
    int      frames = 0;

    // surface under the kart, and an optional probe into the track artwork
    int  surface = SURF_ROAD;
    bool hitWall = false;
    int (*surfaceAt)(const void* ctx, int px, int py) = nullptr;
    const void* surfaceCtx = nullptr;

    void reset(int startX = 512, int startY = 512) {
        speed = 0; turn = 0; heading = 0; frames = 0;
        hitWall = false; surface = SURF_ROAD;
        x = (int32_t)startX << 16;
        y = (int32_t)startY << 16;
    }

    int units() const { return speed >> 8; }
    int maxUnits() const { return st.maxSpeed; }

    void update(const Input& in) {
        frames++;
        int u = units();
        int mx = st.maxSpeed > 0 ? st.maxSpeed : 1;

        // ---------------------------------------------- turn rate (CODE_80A80F)
        // row: slow / mid / fast band of the top speed ($DE selects the row)
        int band = (u < mx / 3) ? 0 : (u < (mx * 2) / 3 ? 1 : 2);
        int16_t tmax   = st.turnMax[band];
        int     tbrake = st.turnBrake[band];
        int     tacc   = st.turnAccel[band];
        int     tcoast = st.turnCoast[band];
        if (tmax <= 0) tmax = 1;

        if (in.left) {
            if (turn > 0) turn = (int16_t)(turn - tbrake);        // unwind
            else {
                int v = (-turn) + tacc;
                turn = (int16_t)(v > tmax ? -tmax : -v);          // ramp negative
            }
        } else if (in.right) {
            if (turn < 0) turn = (int16_t)(turn + tbrake);
            else {
                int v = turn + tacc;
                turn = (int16_t)(v > tmax ? tmax : v);
            }
        } else {
            if (turn > 0)       turn = (int16_t)(turn > tcoast ? turn - tcoast : 0);
            else if (turn < 0)  turn = (int16_t)(turn < -tcoast ? turn + tcoast : 0);
        }

        // ------------------------------------- speed-dependent damping (DSP-1)
        // index = ((speed - $0300) >> 4) & $000E  (0 when at/below $0300)
        int idx = 0;
        if (u > 0x300) idx = (int)((((uint32_t)(u - 0x300)) >> 4) & 0x0E) >> 1;
        if (idx > 7) idx = 7;
        int32_t f = kTurnDamp[idx];
        turn = (int16_t)(turn + (int32_t)((turn * f) >> 13));     // tuned scaling

        // ------------------------------------------- heading (CODE_80A892)
        heading = (uint16_t)(heading + ((int32_t)turn >> 3));     // SAR3

        // ------------------------------------------------------ speed (Q8)
        int32_t maxQ = (int32_t)st.maxSpeed << 8;
        if (in.brake) {
            if (speed > 0) {
                speed -= kBrakeRate;
                if (speed < 0) speed = 0;
            } else {
                speed -= st.accelQ8[0];
                if (speed < -kReverseMax) speed = -kReverseMax;
            }
        } else if (in.accel) {
            int bi = u >> 6;  if (bi > 15) bi = 15;  if (bi < 0) bi = 0;
            int32_t add = st.accelQ8[bi];
            if (speed < 0) { speed += add; }                       // pull out of reverse
            else {
                speed += add;
                if (speed > maxQ) speed = maxQ;
            }
        } else {
            int di = u >> 8;  if (di > 7) di = 7;  if (di < 0) di = 0;
            speed += (int32_t)kCoastDrag[di] << 8;                 // DATA_80A67D
            if (speed < 0) speed = 0;
        }

        // -------------------------------------- velocity + position (Q16 px)
        double a = (double)(uint16_t)heading / 65536.0 * 6.283185307179586;
        double sv = (double)(speed >> 8);                          // units/frame
        int32_t vx = (int32_t)std::lround(sv * std::sin(a));       // Q8 px/frame
        int32_t vy = (int32_t)std::lround(-sv * std::cos(a));
        const int32_t W = 1024 << 16;                              // course wraps
        auto wrap = [](int32_t v) { v %= W; if (v < 0) v += W; return v; };

        int32_t nx = wrap(x + (vx << 8));
        int32_t ny = wrap(y + (vy << 8));

        // ------------------------------------------- surface: wall and sand
        hitWall = false;
        surface = SURF_ROAD;
        if (surfaceAt) {
            int here = surfaceAt(surfaceCtx, nx >> 16, ny >> 16);
            surface = here;
            if (here == SURF_GRASS || here == SURF_VOID) {
                // invisible wall: the move is refused and the kart stops.
                // SURF_VOID is the backdrop where the map has no track at all,
                // which is just as impassable as grass.
                hitWall = true;
                speed = 0;
            } else {
                x = nx;  y = ny;
                if (here == SURF_SAND) {
                    int32_t capQ = ((int32_t)st.maxSpeed * kSandSpeedPct / 100) << 8;
                    if (speed > capQ) speed = capQ;
                }
            }
        } else {
            x = nx;  y = ny;
        }
    }
};

} // namespace smk
