// grid.h -- finds the start/finish line and the eight grid slots in a track
// picture, so the kart can start on the line like the original instead of at an
// arbitrary point.
//
// The start line is the chequered (black and white) band drawn across the
// track, so it is found by looking for the longest run of alternating black and
// white pixels.  The eight grid slots are then placed on the road just behind
// it, staggered four per row the way the original lays them out, and the player
// starts in slot 8.
#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "../png_read.h"

namespace smk {

struct Grid {
    bool found = false;
    int lineX = 0, lineY = 0;          // centre of the chequered band
    bool vertical = false;             // band runs up/down rather than across
    int slot[8] = { 0 };               // x,y of each slot, 0 = unused
    int count = 0;                     // slots actually on road
    std::string note;
};

// near-black or near-white, i.e. a chequered-flag pixel
inline bool chequerPixel(const pngread::Image& im, int x, int y, int& black) {
    if (x < 0 || y < 0 || x >= im.w || y >= im.h) return false;
    size_t o = ((size_t)y * im.w + x) * 4;
    int r = im.rgba[o], g = im.rgba[o+1], b = im.rgba[o+2];
    if (r < 40 && g < 40 && b < 40) { black = 1; return true; }
    if (r > 215 && g > 215 && b > 215) { black = 0; return true; }
    return false;
}

// longest alternating black/white run along a row or a column
inline Grid findGrid(const pngread::Image& im) {
    Grid g;
    if (!im.ok) { g.note = "no track picture"; return g; }

    int bestLen = 0, bestX = 0, bestY = 0;
    bool bestVert = false;

    // horizontal runs
    for (int y = 0; y < im.h; y++) {
        int run = 0, startX = 0, prev = -1;
        for (int x = 0; x < im.w; x++) {
            int black;
            if (chequerPixel(im, x, y, black)) {
                if (black == prev) { run = 1; startX = x; }        // break in the alternation
                else if (run == 0) { run = 1; startX = x; }
                else run++;
                prev = black;
                if (run > bestLen) { bestLen = run; bestX = (startX + x) / 2; bestY = y; bestVert = false; }
            } else { run = 0; prev = -1; }
        }
    }
    // vertical runs
    for (int x = 0; x < im.w; x++) {
        int run = 0, startY = 0, prev = -1;
        for (int y = 0; y < im.h; y++) {
            int black;
            if (chequerPixel(im, x, y, black)) {
                if (black == prev) { run = 1; startY = y; }
                else if (run == 0) { run = 1; startY = y; }
                else run++;
                prev = black;
                if (run > bestLen) { bestLen = run; bestX = x; bestY = (startY + y) / 2; bestVert = true; }
            } else { run = 0; prev = -1; }
        }
    }

    if (bestLen < 6) { g.note = "no chequered start line found"; return g; }
    g.found = true;
    g.lineX = bestX; g.lineY = bestY; g.vertical = bestVert;
    g.note = bestVert ? "vertical" : "horizontal";
    return g;
}

} // namespace smk
