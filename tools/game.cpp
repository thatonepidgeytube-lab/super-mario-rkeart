// game.cpp -- "Super Mario Kart, recompiled": a from-scratch C++ runtime.
//
// Nintendo owns every graphic, sound and byte of game data in Super Mario
// Kart, so this executable ships ZERO of it.  You must point it at your own
// copy of the ROM; all tiles, tilemaps, palettes and stat tables are read
// from that file at runtime (see src/smk/course.h and src/smk/kart.h for the
// routines each piece was decoded from).
//
//   smk-game.exe [path\to\rom.sfc] [options]
//     --course N     course index 0..23 (default 7 = Mario Circuit 1)
//     --char N       driver index 0..7 (default 0 = Mario)
//     --cc N         0 = 50cc, 2 = 100cc, 4 = 150cc (default 2)
//     --start X Y    start position in course pixels (default 512 512)
//     --frames N     frames to simulate in headless modes (default 240)
//     --shot FILE    simulate then save one 256x224 frame as PNG, no window
//     --selftest     simulate and print telemetry, no window
//
// Controls: arrows / WASD steer and drive, ESC quits.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/png.h"
#include "../src/png_read.h"
#include "../src/rom.h"
#include "../src/smk/course.h"
#include "../src/smk/kart.h"
#include "../src/smk/sprite.h"
#include "../src/smk/titlescreen_win.h"
#include "../src/smk/menus_win.h"
#include "../src/smk/grid.h"

static const int VW = 256, VH = 224;   // SNES output
static const int SCALE = 3;            // window magnification

// mode-7 camera: ground plane below the horizon line
static const double HORIZON = 84.5;
static const double FOCAL   = 110.0;
static const double CAM_H   = 64.0;

struct Cfg {
    std::string romPath, shot, mapshot, sheet, blocks, titleShot, titleArt;
    bool stats = false;
    bool palDump = false;
    bool surfTest = false;
    bool noAutoStart = false;
    bool noTitle = false;
    bool gridTest = false;
    bool levelDump = false;
    int frames  = 240;
    int course  = 7;      // Mario Circuit 1
    int chr     = 0;      // Mario
    int cc      = 2;      // 100cc
    int startX  = 512, startY = 512;
    int tileBase = 48;    // first tile of the kart sprite (see docs/sprites.md)
    int sprCols = 4, sprRows = 2;
    int palIdx  = -1;    // -1 = pick the driver's own block from the palette
    int sprScale = 1;
    int cup = 0;
    int blockCount = 16;
    bool selftest = false;
    bool help = false;
};

static void usage() {
    std::printf(
        "smk-game -- Super Mario Kart recompiled\n"
        "usage: smk-game.exe [rom.sfc] [options]\n"
        "  --course N     course 0..23 (default 7 = Mario Circuit 1)\n"
        "  --char N       driver 0..7 (default 0 = Mario)\n"
        "  --cc N         0=50cc 2=100cc 4=150cc (default 2)\n"
        "  --start X Y    start position in course pixels (default 512 512)\n"
        "  --frames N     frames for headless modes (default 240)\n"
        "  --shot FILE    simulate, then write one frame as PNG (no window)\n"
        "  --mapshot FILE write the whole course top-down as a PNG (debug)\n"
        "  --sheet FILE   write the driver's whole tile sheet as a PNG (debug)\n"
        "  --tilebase N   first tile of the kart sprite (default 0)\n"
        "  --sprcols N --sprrows N   kart sprite grid (default 4 2)\n"
        "  --stats        audit every course's tiles/map/palette from the ROM\n"
        "  --selftest     simulate and print telemetry (no window)\n"
        "\n"
        "No game assets are embedded in this executable; you must provide\n"
        "your own Super Mario Kart (USA) ROM -- Nintendo owns them.\n");
}

// ------------------------------------------------------------------ ROM find
static bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ------------------------------------------------------------- import screen
// Nothing but "Import ROM" on a window.  Click it or press any key to pick a
// file; once one is chosen the game starts.  This is the only place the ROM is
// ever asked for, and the game will not run without it.
static std::string browseRom();
static std::string g_importedRom;
static DWORD g_importBorn = 0;      // ignore clicks during startup

static void paintImportScreen(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(16, 18, 24));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 245));
    HFONT f = CreateFontA(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, f);
    const char* msg = "Import ROM";
    RECT tr = rc;
    DrawTextA(dc, msg, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(f);
    EndPaint(hwnd, &ps);
}

static void startImportScreen(HINSTANCE inst) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        switch (m) {
            case WM_PAINT:   paintImportScreen(h); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_LBUTTONDOWN: {
                // A click can arrive while the window is still coming up, which
                // would open the file dialog before the import screen is ever
                // seen, so ignore anything in the first moment.
                if (GetTickCount() - g_importBorn < 700) return 0;
                std::string p = browseRom();
                if (p.empty()) return 0;               // cancel: stay here
                g_importedRom = p;
                DestroyWindow(h);
                return 0;
            }
            case WM_CLOSE:  DestroyWindow(h); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);   // clickable
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SMKImport";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"SMKImport", L"Super Mario Kart", WS_POPUP | WS_CAPTION,
                                CW_USEDEFAULT, CW_USEDEFAULT, 520, 260,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return;
    g_importBorn = GetTickCount();
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static std::string exeDir() {
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string s(buf);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? std::string() : s.substr(0, p + 1);
}

static std::string findRom(const std::string& hint) {
    if (!hint.empty()) { if (fileExists(hint)) return hint; return std::string(); }
    static const char* names[] = {
        "Super Mario Kart (USA).sfc", "Super Mario Kart (USA).smc",
        "Super Mario Kart (USA).fig", "smk.sfc", "rom.sfc",
    };
    std::string dir = exeDir();
    for (const char* n : names) {
        if (!dir.empty() && fileExists(dir + n)) return dir + n;
        if (fileExists(n)) return n;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.sfc", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (fileExists(fd.cFileName)) { FindClose(h); return fd.cFileName; } }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return std::string();
}

static std::string browseRom() {
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFilter  = "SNES ROMs (*.sfc;*.smc;*.fig)\0*.sfc;*.smc;*.fig\0All files (*.*)\0*.*\0";
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = MAX_PATH;
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle   = "Select your Super Mario Kart (USA) ROM";
    return GetOpenFileNameA(&ofn) ? std::string(file) : std::string();
}

static void noRomMessage() {
    const char* msg =
        "No Super Mario Kart ROM found.\n\n"
        "This build contains no Nintendo game data -- the graphics, music and\n"
        "stat tables are read from your own copy of the game at runtime.\n\n"
        "Choose your Super Mario Kart (USA).sfc to continue.";
    std::fprintf(stderr, "%s\n", msg);
    MessageBoxA(nullptr, msg, "smk-game -- ROM required", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------- reference assets
// The reference renders in the project root ("SNES - Super Mario Kart -
// Tracks - <name>.png" and "... - Racers - <name>.png") are the artwork the
// game now draws: the track image is a 1022x1022 top-down picture of the same
// 1024x1024 world the mode-7 camera walks, and the racer sheet supplies the
// kart.  They are development material supplied with the project, not data in
// the ROM, so the ROM is still required for the tilemap, palettes, physics and
// course list -- and two race courses have no reference image and fall back to
// the ROM's own tiles.
struct RefAssets {
    pngread::Image track;          // course ground texture, w == h
    pngread::Image racers;         // kart sheet
    bool haveTrack = false, haveRacers = false;
    // first kart cell found in the racer sheet: x, y, w, h
    int kx = 0, ky = 0, kw = 0, kh = 0;
    std::string trackName, racerName;
};

// The racer sheets are a grid of kart pictures on a flat background.  Find the
// first one by taking the top-left pixel as the background colour and scanning
// right and down to the first run of background.
static bool findFirstKart(const pngread::Image& im, int& kx, int& ky, int& kw, int& kh) {
    if (!im.ok || im.w < 16 || im.h < 16) return false;
    auto px = [&](int x, int y, uint32_t& c) {
        size_t o = ((size_t)y * im.w + x) * 4;
        c = ((uint32_t)im.rgba[o] << 16) | ((uint32_t)im.rgba[o+1] << 8) | im.rgba[o+2];
    };
    uint32_t bg;
    px(0, 0, bg);
    auto isBg = [&](int x, int y) { uint32_t c; px(x, y, c); return c == bg; };
    if (!isBg(0, 0)) return false;                       // top-left must be background

    // The sheets share one layout, so the first kart is a fixed cell in the
    // top-left corner.  Verify the cell actually contains a kart (more than one
    // non-background colour) before trusting it, and keep a generous ink count
    // so a blank or misaligned crop is rejected rather than drawn.
    int x0 = 2, y0 = 2, w = 36, h = 32;
    if (x0 + w > im.w || y0 + h > im.h) return false;
    uint32_t cellBg = bg;
    int ink = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            uint32_t c; px(x, y, c);
            if (c != cellBg) ink++;
        }
    if (ink < w * h / 12) return false;              // mostly empty: not a kart
    kx = x0; ky = y0; kw = w; kh = h;
    return true;
}

static std::string refDir() {
    // the folder the reference PNGs live in: next to the exe, or the cwd
    static const char* names[] = { ".", "..", "../.." };
    std::string dir = exeDir();
    for (const char* n : names) {
        std::string p = (n[0] == '.' && n[1] == 0) ? std::string()
                                                   : std::string(n) + "/";
        if (!dir.empty() && fileExists(dir + p + "SNES - Super Mario Kart - Tracks - Mario Circuit 1.png"))
            return dir + p;
    }
    return std::string();
}

// "MARIO CIRCUIT 1" -> "SNES - Super Mario Kart - Tracks - Mario Circuit 1.png"
static std::string trackFileName(const std::string& courseUpper) {
    std::string s = courseUpper;
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return "SNES - Super Mario Kart - Tracks - " + s + ".png";
}

static const char* kRacerFile[8] = {
    "Mario", "Luigi", "Princess Toadstool _ Peach", "Donkey Kong Jr.",
    "Yoshi", "Toad", "Koopa Troopa", "Bowser",
};

static void loadRefAssets(RefAssets& ref, int course, int chr) {
    std::string dir = refDir();
    if (dir.empty()) return;

    std::string tf = dir + trackFileName(smk::kCourseNames[course]);
    if (fileExists(tf)) {
        pngread::Image im = pngread::read(tf);
        // the track maps carry a white credit strip under the picture; the
        // track itself is the top square
        if (im.ok && im.h > im.w) {
            im.h = im.w;
            im.rgba.resize((size_t)im.w * im.h * 4);
            im.ok = true;
        }
        if (im.ok && im.w == im.h) { ref.track = im; ref.haveTrack = true; ref.trackName = tf; }
    }

    std::string rf = dir + std::string("SNES - Super Mario Kart - Racers - ") + kRacerFile[chr & 7] + ".png";
    if (fileExists(rf)) {
        pngread::Image im = pngread::read(rf);
        if (im.ok) {
            ref.racers = im;
            ref.haveRacers = findFirstKart(im, ref.kx, ref.ky, ref.kw, ref.kh);
            ref.racerName = rf;
        }
    }
}

// Classify a point on the track picture into road / grass / sand.  The artwork
// already separates them by colour family, so this needs no ROM data:
//   grass  green dominant
//   sand   warm tan: r > g >= b and clearly brighter than a kerb
//   road   everything else, including greys, white and the red/white kerbs
static int surfaceAt(const void* ctx, int px, int py) {
    const pngread::Image* im = (const pngread::Image*)ctx;
    if (!im || !im->ok || im->w == 0 || im->h == 0) return smk::SURF_ROAD;
    int x = px & 1023, y = py & 1023;
    int tx = (int)((int64_t)x * im->w / 1024);
    int ty = (int)((int64_t)y * im->h / 1024);
    if (tx < 0) tx = 0; if (tx >= im->w) tx = im->w - 1;
    if (ty < 0) ty = 0; if (ty >= im->h) ty = im->h - 1;
    size_t o = ((size_t)ty * im->w + tx) * 4;
    int r = im->rgba[o], g = im->rgba[o + 1], b = im->rgba[o + 2];

    if (g > r + 18 && g > b + 18) return smk::SURF_GRASS;         // grass

    // kerbs and the white grid lines are drivable
    if (r > 150 && g < 110 && b < 110) return smk::SURF_ROAD;
    if (r > 200 && g > 200 && b > 200) return smk::SURF_ROAD;

    // road is grey: channels close together, and not black
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    if (mx - mn <= 26 && mx > 45) return smk::SURF_ROAD;

    if (r > 118 && g > b && (r - b) > 28 && g >= b + 12) return smk::SURF_SAND;

    // Anything else is the backdrop showing through where the map has no
    // track -- the blue "ocean" on Donut Plains, for one.  Calling that road is
    // what used to park the kart out in the middle of it.
    return smk::SURF_VOID;
}

// Pick a start the kart can actually drive away from: road under the wheels
// and a long clear run of road ahead, and report which way that run goes so the
// kart starts facing along the track rather than into a wall.
static int drivableStart(const void* ctx, int px, int py, uint16_t* heading) {
    // heading 0 = north, increasing clockwise (same as the physics)
    static const int8_t kDirX[8] = {  0,  1,  1,  1,  0, -1, -1, -1 };
    static const int8_t kDirY[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };
    int bestX = px, bestY = py, bestLen = 0, bestDir = 0;
    auto tryPoint = [&](int x, int y) {
        if (surfaceAt(ctx, x, y) != smk::SURF_ROAD) return;
        for (int d = 0; d < 8; d++) {
            int len = 0;
            for (int s = 8; s <= 220; s += 8) {
                int sx = (x + kDirX[d] * s) & 1023, sy = (y + kDirY[d] * s) & 1023;
                if (surfaceAt(ctx, sx, sy) != smk::SURF_ROAD) break;
                len = s;
            }
            if (len > bestLen) { bestLen = len; bestX = x; bestY = y; bestDir = d; }
        }
    };
    tryPoint(px, py);
    for (int r = 4; bestLen < 120 && r < 500; r += 4) {
        for (int a = 0; a < 48; a++) {
            double t = a * 6.283185307179586 / 48.0;
            tryPoint((px + (int)std::lround(r * std::cos(t))) & 1023,
                     (py + (int)std::lround(r * std::sin(t))) & 1023);
        }
    }
    if (heading) *heading = (uint16_t)(bestDir * 0x2000u);   // 8 directions
    return (bestX << 16) | (bestY & 0xFFFF);
}

// ------------------------------------------------------------------- drawing
static void fillRect(uint32_t* fb, int x, int y, int w, int h, uint32_t c) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(VW, x + w), y1 = std::min(VH, y + h);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) fb[j * VW + i] = c;
}

// ------------------------------------------------------------------- --stats
// Audits every course straight from the ROM: how many of the 128x128 tilemap's
// tile numbers the family's decompressed stream can actually supply, how many
// pixels end up transparent, and how many distinct colours the map uses.
// This is the substitute for eyeballing a screenshot.
static int runStats(const Rom& rom) {
    const std::vector<uint8_t>& d = rom.data;
    std::printf("%-3s %-18s %-3s %-7s %-6s %-8s %-8s %-6s %-5s %s\n",
                "id", "course", "fam", "stream", "tiles", "shared%", "beyond%", "clear%", "cols", "notes");
    for (int id = 0; id < smk::kNumCourses; id++) {
        int fam = smk::familyForCourse(d, id);
        if (fam < 0) {
            std::printf("%-3d %-18s %-3s %-7s %-6s %-8s %-8s %-6s %-5s %s\n",
                        id, smk::kCourseNames[id], "-", "-", "-", "-", "-", "-", "-",
                        "battle course (WIP)");
            continue;
        }
        uint32_t pTiles = smk::rd24(d, smk::TBL_M7_TILES + (size_t)fam * 3);
        smk::Decomp t = smk::decompSmk(d, smk::hiromOffset(pTiles), 0x20000);
        int valid = 0;
        if (t.ok && t.out.size() > 0x100) {
            valid = (int)((t.out.size() - 0x100) / 32);
            if (valid > 192) valid = 192;
        }

        smk::Course co = smk::loadCourse(rom, id);
        if (!co.ok) {
            std::string why = "LOAD FAILED: " + co.err;
            std::printf("%-3d %-18s %-3d %-7s %-6s %-8s %-8s %-6s %-5s %s\n",
                        id, smk::kCourseNames[id], fam, "-", "-", "-", "-", "-", "-",
                        why.c_str());
            continue;
        }

        long shared = 0, beyond = 0, total = (long)co.map.size();
        for (size_t i = 0; i < co.map.size(); i++) {
            uint8_t m = co.map[i];
            if (m >= 192) shared++;
            else if (m >= valid) beyond++;
        }
        // colour usage over the whole map, plus how much is transparent
        bool used[256] = {};
        long clear = 0, seen = 0;
        for (int ty = 0; ty < 128; ty += 2) {
            for (int tx = 0; tx < 128; tx += 2) {
                uint8_t m = co.map[(size_t)ty * 128 + tx];
                for (int py = 0; py < 8; py++) {
                    for (int px = 0; px < 8; px++) {
                        uint8_t c = co.tiles[(size_t)m * 64 + py * 8 + px];
                        if (!c) clear++;
                        else used[c] = true;
                        seen++;
                    }
                }
            }
        }
        int cols = 0;
        for (int i = 0; i < 256; i++) if (used[i]) cols++;

        char note[96] = "";
        if (!t.ok) std::snprintf(note, sizeof(note), "tile stream failed");
        else if (beyond * 100 / (total ? total : 1) > 2)
            std::snprintf(note, sizeof(note), "%ld cells use tiles the stream lacks", beyond);
        else if (cols < 4)
            std::snprintf(note, sizeof(note), "palette looks empty");

        std::printf("%-3d %-18s %-3d %-7zX %-6d %-8.1f %-8.1f %-6.1f %-5d %s\n",
                    id, smk::kCourseNames[id], fam, t.out.size(), valid,
                    100.0 * shared / (double)total, 100.0 * beyond / (double)total,
                    100.0 * clear / (double)seen, cols, note);
    }

    // ---- the 8 tile sets themselves, plus the 64 shared tiles
    {
        smk::Decomp g = smk::decompSmk(d, smk::OFF_GLOBAL_TILES, 0x20000);
        std::vector<uint8_t> sh(64 * 64, 0);
        if (g.ok) smk::expandMode7Tiles(g.out, 64, sh.data());
        int shBlank = 0;
        for (int i = 0; i < 64; i++) {
            bool nz = false;
            for (int k = 0; k < 64; k++) if (sh[i * 64 + k]) { nz = true; break; }
            if (!nz) shBlank++;
        }
        std::printf("\n%-3s %-16s %-7s %-6s %-7s %-8s %s\n", "fam", "family", "stream",
                    "tiles", "blank", "shblank", "palette");
        for (int f = 0; f < smk::kNumFamilies; f++) {
            uint32_t p = smk::rd24(d, smk::TBL_M7_TILES + (size_t)f * 3);
            smk::Decomp t = smk::decompSmk(d, smk::hiromOffset(p), 0x20000);
            std::vector<uint8_t> ex(192 * 64, 0);
            int valid = 0, blank = 0;
            if (t.ok) {
                smk::expandMode7Tiles(t.out, 192, ex.data());
                valid = (int)((t.out.size() - 0x100) / 32);
                if (valid > 192) valid = 192;
            }
            for (int i = 0; i < valid; i++) {
                bool nz = false;
                for (int k = 0; k < 64; k++) if (ex[i * 64 + k]) { nz = true; break; }
                if (!nz) blank++;
            }
            uint32_t pp = smk::rd24(d, smk::TBL_PALETTE + (size_t)f * 3);
            smk::Decomp pd = smk::decompSmk(d, smk::hiromOffset(pp), 0x4000);
            int cols = 0;
            bool seen[0x10000] = {};
            for (size_t i = 0; i + 1 < pd.out.size() && i / 2 < 256; i += 2) {
                uint16_t w = (uint16_t)(pd.out[i] | (pd.out[i + 1] << 8));
                if (w && !seen[w]) { seen[w] = true; cols++; }
            }
            std::printf("%-3d %-16s %-7zX %-6d %-7d %-8d %d colours\n", f,
                        smk::kFamilyNames[f], t.out.size(), valid, blank, shBlank, cols);
        }
        std::printf("\nshared tile stream: %zX bytes (0x100 + 64*32 = 0x900)\n", g.out.size());
    }
    return 0;
}

// 3x5 font: rows are 3-bit masks (bit 2 = left column)
static const uint8_t kFont[17][5] = {
    /* 0 */ {7,5,5,5,7}, /* 1 */ {2,6,2,2,7}, /* 2 */ {7,1,7,4,7},
    /* 3 */ {7,1,7,1,7}, /* 4 */ {5,5,7,1,1}, /* 5 */ {7,4,7,1,7},
    /* 6 */ {7,4,7,5,7}, /* 7 */ {7,1,1,1,1}, /* 8 */ {7,5,7,5,7},
    /* 9 */ {7,5,7,1,7}, /* C */ {7,4,4,4,7}, /* S */ {7,4,7,1,7},
    /* P */ {7,5,7,4,4}, /* E */ {7,4,7,4,7}, /* D */ {7,5,5,5,7},
    /* W */ {5,5,5,7,5}, /* I */ {7,2,2,2,7},
};

static int glyphIndex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    switch (ch) {
        case 'C': return 10; case 'S': return 11; case 'P': return 12;
        case 'E': return 13; case 'D': return 14; case 'W': return 15;
        case 'I': return 16; default: return -1;
    }
}

static void drawText(uint32_t* fb, int x, int y, const char* s, int scale, uint32_t c) {
    for (const char* p = s; *p; p++) {
        int g = glyphIndex(*p);
        if (g >= 0) {
            for (int r = 0; r < 5; r++)
                for (int col = 0; col < 3; col++)
                    if ((kFont[g][r] >> (2 - col)) & 1)
                        fillRect(fb, x + col * scale, y + r * scale, scale, scale, c);
        }
        x += 4 * scale;
    }
}

static void drawNumber(uint32_t* fb, int x, int y, long v, int scale, uint32_t c) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%ld", v);
    drawText(fb, x, y, buf, scale, c);
}

// ------------------------------------------------------------------ renderer
static void makePalette(const smk::Course& co, uint32_t* pal) {
    for (int i = 0; i < 256; i++) {
        uint16_t w = co.pal[i];
        uint32_t r = (uint32_t)(w & 31) * 255 / 31;
        uint32_t g = (uint32_t)((w >> 5) & 31) * 255 / 31;
        uint32_t b = (uint32_t)((w >> 10) & 31) * 255 / 31;
        pal[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

// Debug view: the whole course from above, for checking tilemap orientation.
static void renderTopdown(const smk::Course& co, std::vector<uint32_t>& out, int w, int h) {
    uint32_t pal[256];
    makePalette(co, pal);
    uint32_t back = co.pal[0] ? pal[0] : 0xFF3A6ED8u;
    out.assign((size_t)w * h, back);
    for (int y = 0; y < h; y++) {
        int iy = y * 1024 / h;
        for (int x = 0; x < w; x++) {
            int ix = x * 1024 / w;
            int tile = co.map[((iy >> 3) & 127) * 128 + ((ix >> 3) & 127)];
            uint8_t ci = co.tiles[(size_t)tile * 64 + (size_t)(iy & 7) * 8 + (ix & 7)];
            out[(size_t)y * w + x] = ci ? pal[ci] : back;
        }
    }
}

struct View {
    double camX, camY, fX, fY, rX, rY;
    // kart sprite placement (overridable from the command line while
    // working out which tiles the original uses)
    int tileBase = 0, spriteCols = 4, spriteRows = 2, palBlock = -1, sprScale = 3, racer = 0;
    const pngread::Image* tex = nullptr;   // reference track picture, if loaded
    const pngread::Image* racers = nullptr;
    int kx = 0, ky = 0, kw = 0, kh = 0; bool flip = false;
};

static View makeView(const smk::Kart& k, const Cfg& cfg, const RefAssets& ref) {
    double a = (double)(uint16_t)k.heading / 65536.0 * 6.283185307179586;
    View v;
    v.fX = std::sin(a); v.fY = -std::cos(a);      // forward = (sin, -cos)
    v.rX = std::cos(a); v.rY = std::sin(a);       // right of forward
    double kx = k.x / 65536.0, ky = k.y / 65536.0;
    // sit the camera back so the kart lands on the row where we draw it
    double t = FOCAL * CAM_H / (180.5 - HORIZON);
    v.camX = kx - v.fX * t;
    v.camY = ky - v.fY * t;
    v.tileBase  = cfg.tileBase;
    v.spriteCols = cfg.sprCols;
    v.spriteRows = cfg.sprRows;
    v.palBlock = cfg.palIdx;
    v.racer = cfg.chr & 7;
    v.tex = ref.haveTrack ? &ref.track : nullptr;
    v.racers = ref.haveRacers ? &ref.racers : nullptr;
    v.kx = ref.kx; v.ky = ref.ky; v.kw = ref.kw; v.kh = ref.kh;
    v.flip = v.fX < 0.0;
    v.sprScale = cfg.sprScale;
    return v;
}

static void render(uint32_t* fb, const smk::Course& co, const View& v,
                   const smk::DriverSprites& drv) {
    uint32_t pal[256];
    makePalette(co, pal);
    // backdrop = CGRAM colour 0; the sky above the horizon is a BG layer in
    // the original, so we approximate it with the backdrop colour.
    uint32_t backdrop = co.pal[0] ? pal[0] : 0xFF3A6ED8u;
    uint32_t sky = co.pal[0] ? pal[0] : 0xFF3A6ED8u;

    const uint8_t* map = co.map.data();
    const uint8_t* tiles = co.tiles.data();

    int horizon = (int)HORIZON;
    for (int y = 0; y < horizon; y++)
        for (int x = 0; x < VW; x++) fb[y * VW + x] = sky;

    for (int y = horizon; y < VH; y++) {
        double denom = (y - HORIZON) + 0.5;
        double t = FOCAL * CAM_H / denom;         // distance ahead, in pixels
        double s = t / FOCAL;                     // world pixels per screen pixel
        uint32_t* row = fb + y * VW;
        for (int x = 0; x < VW; x++) {
            double u = (x - (VW - 1) * 0.5) * s;
            double wx = v.camX + v.fX * t + v.rX * u;
            double wy = v.camY + v.fY * t + v.rY * u;
            int ix = (int)std::floor(wx) & 1023;   // course wraps at 1024 px
            int iy = (int)std::floor(wy) & 1023;
            if (v.tex && v.tex->ok) {
                // the reference track picture is a top-down view of the same
                // 1024x1024 world, so it samples straight into the mode-7 pass
                const std::vector<uint8_t>& t = v.tex->rgba;
                int tw = v.tex->w;
                size_t o = ((size_t)iy * tw + ix) * 4;
                if (o + 3 < t.size()) {
                    row[x] = 0xFF000000u | ((uint32_t)t[o] << 16) |
                             ((uint32_t)t[o + 1] << 8) | t[o + 2];
                    continue;
                }
            }
            int tile = map[((iy >> 3) & 127) * 128 + ((ix >> 3) & 127)];
            uint8_t ci = tiles[(size_t)tile * 64 + (size_t)(iy & 7) * 8 + (ix & 7)];
            row[x] = ci ? pal[ci] : backdrop;
        }
    }

    // Draw the player kart sprite (mode-7 style: at fixed screen position,
    // scaled by distance). For now just draw at a fixed position with the
    // first few tiles from the driver sheet.
    if (v.racers && v.racers->ok && v.kw > 0) {
        // kart cropped straight out of the reference racer sheet
        int kartX = VW / 2, kartY = 180;
        int scale = v.sprScale;
        int w = v.kw * scale, h = v.kh * scale;
        int left = kartX - w / 2, top = kartY - h;
        const std::vector<uint8_t>& t = v.racers->rgba;
        for (int dy = 0; dy < h; dy++) {
            int sy = v.ky + dy / scale;
            if (sy < 0 || sy >= v.racers->h) continue;
            for (int dx = 0; dx < w; dx++) {
                int sx = v.kx + (v.flip ? (v.kw - 1 - dx / scale) : dx / scale);
                if (sx < 0 || sx >= v.racers->w) continue;
                size_t o = ((size_t)sy * v.racers->w + sx) * 4;
                if (t[o+3] == 0) continue;
                int fx = left + dx, fy = top + dy;
                if (fx >= 0 && fx < VW && fy >= 0 && fy < VH)
                    fb[(size_t)fy * VW + fx] = 0xFF000000u | ((uint32_t)t[o] << 16) |
                                               ((uint32_t)t[o+1] << 8) | t[o+2];
            }
        }
    } else if (drv.ok) {
        // fall back to the ROM's own sprite sheet
        int kartX = VW / 2;
        int kartY = 180; // matches the perspective projection
        uint16_t kpal[16];
        int pb = (v.palBlock >= 0) ? v.palBlock : smk::driverPalBlock(co, v.racer);
        for (int i = 0; i < 16; i++) kpal[i] = smk::driverPal(co, pb, i);
        bool flip = v.fX < 0.0;
        smk::drawKartSprite(fb, VW, VH, kartX, kartY, v.sprScale, drv, kpal,
                            v.tileBase, v.spriteCols, v.spriteRows, flip);
    }
}

static const uint32_t kCharColor[8] = {
    0xFFD82818, // Mario red
    0xFF28A828, // Luigi green
    0xFFF898C0, // Peach pink
    0xFFF8D848, // Daisy yellow
    0xFF48C828, // Yoshi green
    0xFFF0F0F0, // Toad white
    0xFF40A840, // Koopa green
    0xFFF88820, // Bowser orange
};

static bool saveFrame(const std::string& path, int w, int h, const std::vector<uint32_t>& fb) {
    std::vector<uint8_t> rgba((size_t)w * (size_t)h * 4);
    for (size_t i = 0; i < fb.size(); i++) {
        uint32_t c = fb[i];
        rgba[i * 4 + 0] = (uint8_t)(c >> 16);
        rgba[i * 4 + 1] = (uint8_t)(c >> 8);
        rgba[i * 4 + 2] = (uint8_t)c;
        rgba[i * 4 + 3] = 0xFF;
    }
    return png::write(path, w, h, rgba);
}

static void drawHud(uint32_t* fb, const smk::Course& co, const smk::Kart& k, const Cfg& cfg) {
    uint32_t white = 0xFFFFFFFFu, yellow = 0xFFFFD840u;
    drawText(fb, 6, 6, "SPEED", 2, white);
    long u = k.units();
    drawNumber(fb, 6, 18, u, 2, yellow);
    int mx = k.st.maxSpeed > 0 ? k.st.maxSpeed : 1;
    double f = (double)u / (double)mx;
    if (f < 0) f = 0; if (f > 1) f = 1;
    fillRect(fb, 6, 30, 84, 6, 0xC0000000u);
    fillRect(fb, 7, 31, (int)(82 * f), 4, 0xFF40C040u);

    char buf[16];
    std::snprintf(buf, sizeof(buf), "C%02d", cfg.course);
    drawText(fb, 6, 40, buf, 2, white);
    std::snprintf(buf, sizeof(buf), "%02d", cfg.chr);
    drawText(fb, 6, 52, buf, 2, kCharColor[cfg.chr & 7]);
}

// -------------------------------------------------------------- headless run
static int runHeadless(const Cfg& cfg, smk::Course& course, smk::Kart& kart,
                       const smk::DriverSprites& drv, const RefAssets& ref) {
    smk::Input in;
    in.accel = true;                            // scripted: hold throttle
    for (int i = 0; i < cfg.frames; i++) {
        kart.update(in);
        if (cfg.selftest && ((i + 1) % 60 == 0)) {
            double x = kart.x / 65536.0, y = kart.y / 65536.0;
            const char* surf = kart.hitWall ? "WALL"
                             : kart.surface == smk::SURF_SAND ? "sand"
                             : kart.surface == smk::SURF_GRASS ? "grass" : "road";
            std::printf("t=%4.1fs speed=%4d units  heading=%5.1f deg  "
                        "pos=(%6.1f,%6.1f)  %s\n",
                        (i + 1) / 60.0, kart.units(),
                        kart.heading * (360.0 / 65536.0), x, y, surf);
        }
    }
    std::printf("frames=%d speed=%d units (top %d) heading=0x%04X pos=(%.1f,%.1f) turn=%d\n",
                cfg.frames, kart.units(), kart.st.maxSpeed, kart.heading,
                kart.x / 65536.0, kart.y / 65536.0, kart.turn);

    if (!cfg.shot.empty()) {
        std::vector<uint32_t> fb((size_t)VW * VH);
        render(fb.data(), course, makeView(kart, cfg, ref), drv);
        drawHud(fb.data(), course, kart, cfg);
        if (!saveFrame(cfg.shot, VW, VH, fb)) {
            std::fprintf(stderr, "error: could not write %s\n", cfg.shot.c_str());
            return 1;
        }
        // numeric sanity check on the mode-7 pass, since screenshots aren't
        // always eyeballed: a working track shows many colours and a skyline
        std::vector<uint8_t> seen(1u << 15, 0);
        int colours = 0, horizon = 0, above = 0;
        for (int j = 0; j < VH; j++) {
            bool rowDiffers = false;
            for (int i = 0; i < VW; i++) {
                uint32_t c = fb[(size_t)j * VW + i] & 0x7FFF;
                if (i && c != (fb[(size_t)j * VW + i - 1] & 0x7FFF)) rowDiffers = true;
                if (!seen[c]) { seen[c] = 1; colours++; }
            }
            if (j == 84) horizon = rowDiffers;
            if (j < 84) {
                for (int i = 0; i < VW; i++)
                    if ((fb[(size_t)j * VW + i] & 0x7FFF) != (fb[i] & 0x7FFF)) { above++; break; }
            }
        }
        // and on the kart: how many of its pixels land on each palette entry
        int kartPix = 0, matched = 0;
        int pb = (cfg.palIdx >= 0) ? cfg.palIdx : smk::driverPalBlock(course, cfg.chr & 7);
        std::printf("kart palette block 0x%02X:", pb);
        uint32_t want[16];
        for (int i = 0; i < 16; i++) {
            want[i] = smk::bgr555(smk::driverPal(course, pb, i));
            std::printf(" %04X", smk::driverPal(course, pb, i));
        }
        int kx = VW / 2, ky = 180, kw = cfg.sprCols * 8 * cfg.sprScale, kh = cfg.sprRows * 8 * cfg.sprScale;
        int per[16] = {};
        for (int y = ky - kh; y < ky; y++)
            for (int x = kx - kw / 2; x < kx + kw / 2; x++) {
                if (x < 0 || x >= VW || y < 0 || y >= VH) continue;
                uint32_t c = fb[(size_t)y * VW + x];
                if (c == 0xFF101010u || c == 0xFF505050u) continue;  // not ours
                kartPix++;
                for (int i = 1; i < 16; i++) if (c == want[i]) { matched++; per[i]++; break; }
            }
        std::printf("kart pixels by palette index:");
        for (int i = 1; i < 16; i++) if (per[i]) std::printf(" %d=%d", i, per[i]);
        std::printf("\n");
        std::printf("\n");
        std::printf("wrote %s (%dx%d): %d colours, horizon row varies=%s, sky rows vary=%d\n",
                    cfg.shot.c_str(), VW, VH, colours, horizon ? "yes" : "no", above);
        std::printf("kart area: %d pixels, %d match the palette (%.0f%%)\n",
                    kartPix, matched, kartPix ? 100.0 * matched / kartPix : 0.0);
    }
    return 0;
}

// ------------------------------------------------------------------ windowed
static smk::Input readInput() {
    smk::Input in;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool up    = down(VK_UP)    || down('W');
    bool dwn   = down(VK_DOWN)  || down('S');
    bool lft   = down(VK_LEFT)  || down('A');
    bool rgt   = down(VK_RIGHT) || down('D');
    in.accel = up;
    in.brake = dwn;
    in.left  = lft;
    in.right = rgt;
    return in;
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE)    { DestroyWindow(h); return 0; }
    if (m == WM_DESTROY)  { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { DestroyWindow(h); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static void present(HWND hwnd, const uint32_t* fb) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = VW;
    bmi.bmiHeader.biHeight      = -VH;          // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    RECT rc; GetClientRect(hwnd, &rc);
    HDC dc = GetDC(hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, VW, VH,
                  fb, &bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(hwnd, dc);
}

static int runWindowed(const Cfg& cfg, smk::Course& course, smk::Kart& kart,
                       const smk::DriverSprites& drv, const RefAssets& ref) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SMKRecomp";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, VW * SCALE, VH * SCALE };
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(L"SMKRecomp",
                              L"Super Mario Kart -- recompiled",
                              style, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, nullptr);
    if (!hwnd) { std::fprintf(stderr, "error: CreateWindow failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    std::vector<uint32_t> fb((size_t)VW * VH);
    LARGE_INTEGER freq, now, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    double acc = 0.0;
    const double step = 1.0 / 60.0;
    bool running = true;

    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        QueryPerformanceCounter(&now);
        acc += (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (acc > 0.25) acc = 0.25;             // avoid spiral after a stall

        bool stepped = false;
        while (acc >= step) {
            kart.update(readInput());
            acc -= step;
            stepped = true;
        }
        if (stepped) {
            render(fb.data(), course, makeView(kart, cfg, ref), drv);
            drawHud(fb.data(), course, kart, cfg);
            present(hwnd, fb.data());
        } else {
            Sleep(1);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------- main
int main(int argc, char** argv) {
    Cfg cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") cfg.help = true;
        else if (a == "--shot")        cfg.shot = next("--shot");
        else if (a == "--mapshot")     cfg.mapshot = next("--mapshot");
        else if (a == "--sheet")       cfg.sheet = next("--sheet");
        else if (a == "--blocks")      cfg.blocks = next("--blocks");
        else if (a == "--titleshot")   cfg.titleShot = next("--titleshot");
        else if (a == "--titleart")     cfg.titleArt = next("--titleart");
        else if (a == "--blockcount")   cfg.blockCount = std::atoi(next("--blockcount"));
        else if (a == "--tilebase")    cfg.tileBase = std::atoi(next("--tilebase"));
        else if (a == "--sprcols")     cfg.sprCols = std::atoi(next("--sprcols"));
        else if (a == "--sprrows")     cfg.sprRows = std::atoi(next("--sprrows"));
        else if (a == "--palidx")      cfg.palIdx = (int)std::strtol(next("--palidx"), nullptr, 0);
        else if (a == "--sprscale")    cfg.sprScale = std::atoi(next("--sprscale"));
        else if (a == "--stats")       cfg.stats = true;
        else if (a == "--surftest")     cfg.surfTest = true;
        else if (a == "--noautostart")  cfg.noAutoStart = true;
        else if (a == "--notitle")      cfg.noTitle = true;
        else if (a == "--gridtest")     cfg.gridTest = true;
        else if (a == "--leveldump")    cfg.levelDump = true;
        else if (a == "--paldump")     cfg.palDump = true;
        else if (a == "--selftest")    cfg.selftest = true;
        else if (a == "--frames")      cfg.frames = std::atoi(next("--frames"));
        else if (a == "--course")      cfg.course = std::atoi(next("--course"));
        else if (a == "--char")        cfg.chr = std::atoi(next("--char"));
        else if (a == "--cc")          cfg.cc = std::atoi(next("--cc"));
        else if (a == "--start") {
            cfg.startX = std::atoi(next("--start"));
            cfg.startY = std::atoi(next("--start"));
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            return 2;
        } else {
            cfg.romPath = a;
        }
    }
    if (cfg.help) { usage(); return 0; }
    if (cfg.course < 0 || cfg.course >= smk::kNumCourses) {
        std::fprintf(stderr, "error: --course must be 0..%d\n", smk::kNumCourses - 1);
        return 2;
    }
    if (smk::kCourseFamily[cfg.course] < 0) {   // same values the ROM's table holds
        std::fprintf(stderr, "error: course %d (%s) has no mode-7 family yet\n",
                     cfg.course, smk::kCourseNames[cfg.course]);
        return 2;
    }

    // ---- you must supply your own ROM: nothing is bundled here.
    // A path on the command line skips the import screen; otherwise the screen
    // is the only thing shown until a file is chosen.
    std::string romPath = cfg.romPath;
    bool headless = !cfg.shot.empty() || !cfg.mapshot.empty() || cfg.stats ||
                    cfg.palDump || !cfg.blocks.empty() || !cfg.sheet.empty() ||
                    cfg.selftest || cfg.surfTest || cfg.gridTest || cfg.levelDump || !cfg.titleShot.empty() || !cfg.titleArt.empty();
    if (romPath.empty() && headless) romPath = findRom(std::string());
    if (romPath.empty()) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        startImportScreen(inst);
        romPath = g_importedRom;
    }
    if (romPath.empty()) { noRomMessage(); return 1; }
    if (!headless && !cfg.noTitle) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        MenuResult pick = runMenus(inst);
        cfg.chr = pick.chr;
        cfg.chr &= 7;
        cfg.cup = pick.cup;
        if (pick.course >= 0 && pick.course < smk::kNumCourses) cfg.course = pick.course;
    }

    Rom rom;
    std::string err;
    if (!rom.load(romPath, &err)) {
        std::fprintf(stderr, "error: %s: %s\n", romPath.c_str(), err.c_str());
        return 1;
    }
    if (rom.hdr.title.find("MARIO KART") == std::string::npos) {
        std::fprintf(stderr,
            "error: '%s' is \"%s\" -- this runtime only understands "
            "Super Mario Kart (USA).\n", romPath.c_str(), rom.hdr.title.c_str());
        return 1;
    }
    std::printf("ROM: %s\n     (exe dir '%s', cwd '", romPath.c_str(), exeDir().c_str());
    {
        char cwd[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, cwd);
        std::printf("%s')\n", cwd);
    }
    std::printf("     title \"%s\", %zu bytes, %s%s, header checksum 0x%04X\n",
                rom.hdr.title.c_str(), rom.data.size(),
                rom.hdr.hirom ? "HiROM" : "LoROM",
                rom.hdr.fastrom ? "+FastROM" : "", rom.hdr.checksum);

    smk::Course course = smk::loadCourse(rom, cfg.course);
    if (!course.ok) {
        std::fprintf(stderr, "error: course load failed: %s\n", course.err.c_str());
        return 1;
    }

    // Load driver sprite sheet for the chosen character
    smk::DriverSprites drv = smk::loadDriverSprites(rom, cfg.chr & 7);
    if (!drv.ok) {
        std::fprintf(stderr, "warning: driver sprite load failed: %s\n", drv.err.c_str());
    } else {
        std::printf("driver sprites: %zu tiles, %d colours\n",
                    drv.tiles.size() / 64, 16);
    }

    // Reference artwork from the project folder
    RefAssets ref;
    loadRefAssets(ref, cfg.course, cfg.chr & 7);
    if (ref.haveTrack)
        std::printf("track art:  %s (%dx%d)\n", ref.trackName.c_str(), ref.track.w, ref.track.h);
    else
        std::printf("track art:  none for this course, using the ROM's own tiles\n");
    if (ref.haveRacers)
        std::printf("kart art:   %s (%dx%d), first kart cell %d,%d %dx%d\n", ref.racerName.c_str(), ref.racers.w, ref.racers.h, ref.kx, ref.ky, ref.kw, ref.kh);
    else
        std::printf("kart art:   none, using the ROM's sprite sheet\n");

    if (!cfg.titleArt.empty()) {
        // render the title screen exactly as the window does, into a PNG
        TitleArt art = loadTitleArt();
        if (!art.ok) { std::fprintf(stderr, "error: %s\n", art.err.c_str()); return 1; }
        const int W = 660, H = 400;
        std::vector<uint32_t> fb((size_t)W * H, 0xFF000000u);
        auto blit = [&](int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
            for (int y = 0; y < dh; y++) {
                int iy = sy + (int)((int64_t)y * sh / dh);
                if (iy < 0 || iy >= art.sheet.h) continue;
                for (int x = 0; x < dw; x++) {
                    int ix = sx + (int)((int64_t)x * sw / dw);
                    if (ix < 0 || ix >= art.sheet.w) continue;
                    size_t o = ((size_t)iy * art.sheet.w + ix) * 4;
                    if (!art.sheet.rgba[o] && !art.sheet.rgba[o+1] && !art.sheet.rgba[o+2]) continue;
                    int fx = dx + x, fy = dy + y;
                    if (fx >= 0 && fx < W && fy >= 0 && fy < H)
                        fb[(size_t)fy * W + fx] = 0xFF000000u |
                            ((uint32_t)art.sheet.rgba[o] << 16) |
                            ((uint32_t)art.sheet.rgba[o+1] << 8) | art.sheet.rgba[o+2];
                }
            }
        };
        blit(kBgX, kBgY, kBgW, kBgH, 0, 0, W, H);
        int lw = W / 2, lh = lw * kLogoH / kLogoW;
        if (lh > H / 2) { lh = H / 2; lw = lh * kLogoW / kLogoH; }
        blit(kLogoX, kLogoY, kLogoW, kLogoH, (W - lw) / 2, (H - lh) / 5, lw, lh);
        if (!saveFrame(cfg.titleArt, W, H, fb)) {
            std::fprintf(stderr, "error: could not write %s\n", cfg.titleArt.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d) from %s\n", cfg.titleArt.c_str(), W, H,
                    "title/SNES - Super Mario Kart - Miscellaneous - Title & Results Background(1).png");
        return 0;
    }
    if (!cfg.titleShot.empty()) {
        smk::TitleScreen ts = smk::loadTitleScreen(rom);
        if (!ts.ok) { std::fprintf(stderr, "error: %s\n", ts.err.c_str()); return 1; }
        int maxTile = 0;
        std::vector<int> hist(256, 0);
        for (int i = 0; i < smk::kTitleMapBytes; i++) {
            maxTile = std::max<int>(maxTile, ts.map[i]);
            hist[ts.map[i]]++;
        }
        std::printf("title screen: %d tiles in map, max tile %d, %zu bytes of tile data\n",
                    ts.tileCount, maxTile, ts.tiles.size());
        std::printf("  most used tiles:");
        std::vector<std::pair<int,int>> top;
        for (int i = 0; i < 256; i++) if (hist[i]) top.push_back({hist[i], i});
        std::sort(top.begin(), top.end(), [](const std::pair<int,int>& a,
                                            const std::pair<int,int>& b) { return a.first > b.first; });
        for (size_t i = 0; i < top.size() && i < 6; i++)
            std::printf("  tile %d x%d", top[i].second, top[i].first);
        std::printf("\n");
        // composite with a stand-in ramp so the shapes are visible
        static const uint32_t kRamp[4] = { 0xFF000000u, 0xFF3060C0u, 0xFFF8F8F8u, 0xFFE02020u };
        std::vector<uint32_t> fb((size_t)smk::kTitlePixels * smk::kTitlePixels, 0xFF101010u);
        int oob = 0;
        for (int y = 0; y < smk::kTitlePixels; y++) {
            for (int x = 0; x < smk::kTitlePixels; x++) {
                int tile = ts.map[(y >> 3) * smk::kTitleMapSide + (x >> 3)];
                if (tile >= ts.tileCount) { oob++; continue; }
                uint8_t v = ts.tiles[(size_t)tile * 64 + (y & 7) * 8 + (x & 7)];
                fb[(size_t)y * smk::kTitlePixels + x] = kRamp[v & 3];
            }
        }
        std::printf("  cells whose tile is missing: %d of %d\n",
                    oob, smk::kTitlePixels * smk::kTitlePixels);
        if (!saveFrame(cfg.titleShot, smk::kTitlePixels, smk::kTitlePixels, fb)) {
            std::fprintf(stderr, "error: could not write %s\n", cfg.titleShot.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d)\n", cfg.titleShot.c_str(),
                    smk::kTitlePixels, smk::kTitlePixels);
        return 0;
    }
    if (cfg.levelDump) {
        // Two-stage level data: the ROM stream decompresses to a second stream,
        // which is the level data proper.  The 128x128 tilemap is the first
        // 0x4000 bytes; the start position and the road data live around it.
        for (int id = 0; id < smk::kNumCourses; id++) {
            if (id != cfg.course) continue;
            uint32_t ptr = smk::rd24(rom.data, smk::TBL_LEVEL_DATA + (size_t)id * 3);
            smk::Decomp outer = smk::decompSmk(rom.data, smk::hiromOffset(ptr), 0x80000);
            if (!outer.ok) { std::printf("course %d: outer failed\n", id); return 1; }
            smk::Decomp inner = smk::decompSmk(outer.out, 0, 0x80000);
            std::printf("course %d  file=0x%06X  outer=%zu  inner=%zu  ok=%d\n",
                        id, smk::hiromOffset(ptr), outer.out.size(),
                        inner.out.size(), (int)inner.ok);
            const std::vector<uint8_t>& L = inner.out;
            if (L.size() < 16) return 1;
            auto w16 = [&](size_t o) { return (uint16_t)(L[o] | (L[o+1] << 8)); };
            auto w32 = [&](size_t o) { return (uint32_t)w16(o) | ((uint32_t)w16(o+2) << 16); };
            std::printf("  first 64 bytes:");
            for (int i = 0; i < 64; i++) std::printf(" %02X", L[i]);
            std::printf("\n");
            std::printf("  as words:");
            for (int i = 0; i < 16; i++) std::printf(" %04X", w16((size_t)i * 2));
            std::printf("\n");
            // candidate start position: a word pair inside the map's range
            // (0..1023) that is not tile data-looking
            std::printf("  scanning for a start-position-looking pair...\n");
            int found = 0;
            for (size_t o = 0; o + 4 <= L.size() && found < 8; o += 2) {
                uint16_t a = w16(o), b2 = w16(o + 2);
                if (a < 1024 && b2 < 1024 && a > 8 && b2 > 8 && o < 0x40) {
                    std::printf("    offset 0x%04zX: x=%u y=%u\n", o, a, b2);
                    found++;
                }
            }
        }
        return 0;
    }
    if (cfg.stats) return runStats(rom);
    if (cfg.gridTest) {
        // where is the chequered start line on each track that has art?
        for (int c = 0; c < smk::kNumCourses; c++) {
            RefAssets r2;
            loadRefAssets(r2, c, cfg.chr & 7);
            if (!r2.haveTrack) continue;
            smk::Grid g = smk::findGrid(r2.track);
            std::printf("%-18s ", smk::kCourseNames[c]);
            if (!g.found) { std::printf("no start line (%s)\n", g.note.c_str()); continue; }
            std::printf("line at %4d,%4d  %s  (run %d)\n", g.lineX, g.lineY,
                        g.vertical ? "vertical" : "horizontal", 0);
        }
        return 0;
    }
    if (cfg.surfTest) {
        // count how much of the track the classifier calls road / grass / sand
        if (!ref.haveTrack) { std::fprintf(stderr, "no track art, cannot test\n"); return 1; }
        long n[3] = {};
        for (int y = 0; y < 1024; y += 2)
            for (int x = 0; x < 1024; x += 2) n[surfaceAt(&ref.track, x, y)]++;
        long tot = n[0] + n[1] + n[2];
        std::printf("%s: road %.1f%%  grass %.1f%%  sand %.1f%%  (%ld samples)\n",
                    ref.trackName.c_str(), 100.0 * n[0] / tot, 100.0 * n[1] / tot,
                    100.0 * n[2] / tot, tot);
        std::printf("  y=512: ");
        for (int x = 0; x < 1024; x += 32) {
            int s = surfaceAt(&ref.track, x, 512);
            std::printf("%c", s == smk::SURF_ROAD ? 'R' : s == smk::SURF_GRASS ? 'G' : 'S');
        }
        std::printf("\n");
        // where is the biggest drivable sand run?  used to prove the slowdown
        int bestLen = 0, bestX = 0, bestY = 0, bestDir = 0;
        static const int8_t kDX[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        static const int8_t kDY[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
        for (int y = 0; y < 1024; y += 8) {
            for (int x = 0; x < 1024; x += 8) {
                if (surfaceAt(&ref.track, x, y) != smk::SURF_SAND) continue;
                for (int d = 0; d < 8; d++) {
                    int len = 0;
                    for (int s = 8; s <= 240; s += 8) {
                        int sx = (x + kDX[d] * s) & 1023, sy = (y + kDY[d] * s) & 1023;
                        if (surfaceAt(&ref.track, sx, sy) != smk::SURF_SAND) break;
                        len = s;
                    }
                    if (len > bestLen) { bestLen = len; bestX = x; bestY = y; bestDir = d; }
                }
            }
        }
        std::printf("  longest sand run: %d px from (%d,%d) heading 0x%04X\n",
                    bestLen, bestX, bestY, bestDir * 0x2000u);
        return 0;
    }
    if (cfg.palDump) {
        std::printf("BGR555 words and their RGB888 expansion (CGRAM index: word)\n");
        for (int base = 0; base < 256; base += 8) {
            std::printf("pal[%3d..%3d]:", base, base + 7);
            for (int i = 0; i < 8; i++) {
                uint16_t w = course.pal[base + i];
                uint32_t c = smk::bgr555(w);
                std::printf("  %04X=R%02u,G%02u,B%02u", w,
                            (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            }
            std::printf("\n");
        }
        std::printf("note: in BGR555 0x7C00 is red, 0x001F is blue, 0x03E0 is green\n");
        return 0;
    }
    if (!cfg.sheet.empty()) {
        const int cols = 16, rows = (smk::kTilesPerSheet + cols - 1) / cols;
        uint16_t kpal[16];
        for (int i = 0; i < 16; i++) kpal[i] = smk::driverPal(course, cfg.palIdx, i);
        uint32_t rgba[16];
        for (int i = 0; i < 16; i++) rgba[i] = smk::bgr555(kpal[i]);
        std::printf("kart palette block 0x%02X for %s: %04X %04X %04X %04X %04X %04X\n",
                    cfg.palIdx, smk::kCharNames[cfg.chr & 7],
                    kpal[0], kpal[1], kpal[2], kpal[3], kpal[4], kpal[5]);
        std::vector<uint32_t> sh((size_t)cols * 16 * rows * 16, 0xFF303030u);
        for (int t = 0; t < smk::kTilesPerSheet; t++) {
            int cx = (t % cols) * 16, cy = (t / cols) * 16;
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    uint8_t v = drv.tiles[(size_t)t * 64 + py * 8 + px];
                    uint32_t c = v ? rgba[v & 15] : 0xFF101010u;
                    sh[(size_t)(cy + py * 2) * cols * 16 + (cx + px * 2)]     = c;
                    sh[(size_t)(cy + py * 2) * cols * 16 + (cx + px * 2) + 1] = c;
                }
            }
        }
        if (!saveFrame(cfg.sheet, cols * 16, rows * 16, sh)) {
            std::fprintf(stderr, "error: could not write %s\n", cfg.sheet.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d, %d tiles at 2x, %d per row)\n",
                    cfg.sheet.c_str(), cols * 16, rows * 16, smk::kTilesPerSheet, cols);
        return 0;
    }
    if (!cfg.blocks.empty()) {
        // every 4x2 tile block, 4 blocks per row, so the kart frames can be
        // spotted by eye and counted back to a tile index
        const int bw = 8 * cfg.sprScale, bh = 8 * cfg.sprScale;
        const int perRow = 4;
        int total = smk::kTilesPerSheet / (cfg.sprCols * cfg.sprRows);
        if (cfg.blockCount > 0 && total > cfg.blockCount) total = cfg.blockCount;
        int rows = (total + perRow - 1) / perRow;
        int W = perRow * cfg.sprCols * bw, H = rows * cfg.sprRows * bh;
        uint16_t kpal[16];
        for (int i = 0; i < 16; i++) kpal[i] = smk::driverPal(course, cfg.palIdx, i);
        std::vector<uint32_t> fb((size_t)W * H, 0xFF505050u);
        for (int b = 0; b < total; b++) {
            int cx = (b % perRow) * cfg.sprCols * bw, cy = (b / perRow) * cfg.sprRows * bh;
            smk::drawKartSprite(fb.data(), W, H,
                                cx + (cfg.sprCols * bw) / 2, cy + cfg.sprRows * bh,
                                cfg.sprScale, drv, kpal, b * cfg.sprCols * cfg.sprRows,
                                cfg.sprCols, cfg.sprRows);
        }
        if (!saveFrame(cfg.blocks, W, H, fb)) {
            std::fprintf(stderr, "error: could not write %s\n", cfg.blocks.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d, %d blocks of %dx%d, %d per row)\n",
                    cfg.blocks.c_str(), W, H, total, cfg.sprCols, cfg.sprRows, perRow);
        return 0;
    }
    if (!cfg.mapshot.empty()) {
        std::vector<uint32_t> top;
        renderTopdown(course, top, 256, 256);
        if (!saveFrame(cfg.mapshot, 256, 256, top)) {
            std::fprintf(stderr, "error: could not write %s\n", cfg.mapshot.c_str());
            return 1;
        }
        std::printf("wrote %s (256x256, whole course from above)\n", cfg.mapshot.c_str());
        return 0;
    }
    smk::Stats st = smk::loadStats(rom, cfg.chr, cfg.cc);
    smk::Kart kart;
    kart.st = st;
    kart.surfaceAt = surfaceAt;
    kart.surfaceCtx = ref.haveTrack ? (const void*)&ref.track : nullptr;
    int sx = cfg.startX, sy = cfg.startY;
    if (kart.surfaceCtx && !cfg.noAutoStart) {
        // never spawn stuck against a wall: face the kart along the road
        uint16_t h0 = 0;
        int packed = drivableStart(kart.surfaceCtx, sx, sy, &h0);
        int nx = packed >> 16, ny = packed & 0xFFFF;
        if (nx != sx || ny != sy)
            std::printf("start %d,%d not drivable; using %d,%d heading 0x%04X\n",
                        sx, sy, nx, ny, h0);
        sx = nx; sy = ny;
        kart.reset(sx, sy);
        kart.heading = h0;
    } else {
        kart.reset(sx, sy);
    }

    std::printf("course: %d = %s  (graphics family %s)\n", cfg.course,
                smk::kCourseNames[cfg.course],
                smk::kFamilyNames[course.family]);
    std::printf("driver: %s   engine: %dcc\n", smk::kCharNames[cfg.chr & 7],
                cfg.cc == 0 ? 50 : (cfg.cc == 4 ? 150 : 100));
    std::printf("stats:  top speed %d units/frame, accel curve %d..%d, "
                "turn rows %d/%d/%d\n",
                st.maxSpeed, st.accelQ8[0], st.accelQ8[15],
                st.turnMax[0], st.turnMax[1], st.turnMax[2]);

    if (cfg.selftest || !cfg.shot.empty())
        return runHeadless(cfg, course, kart, drv, ref);
    return runWindowed(cfg, course, kart, drv, ref);
}
