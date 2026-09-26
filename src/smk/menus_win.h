// menus_win.h -- the front end: title, character select, cup select and
// course select, then back to the caller which starts the race.
//
// Art sources, all read at runtime from the project folder:
//   title/...Title & Results Background(1).png  title art, panels measured
//                                              from its black separators
//   title/...Character Select(3).png            character-select chrome
//   <driver> racer sheets                        the picture in each box
//   SNES - Super Mario Kart - Tracks - <course>.png   the course preview
//
// The Course Select sheet is a documentation page (a preview template plus two
// worked examples and the cup/course lists as text), not a sprite sheet, so the
// cup and course screens are built from its lists and its colours rather than
// blitting it directly.
#pragma once
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../png_read.h"
#include "titlescreen_win.h"          // loadTitleArt + blitScaled + panel rects

// ---------------------------------------------------------------- cup tables
// DATA_81EC1B / DATA_81EC2F in the ROM list 20 race courses; the sheet's
// COURSE SELECT list gives the same four cups in order.
struct CupCourse { const char* name; int courseId; };

static const CupCourse kMushroomCup[5] = {
    { "MARIO CIRCUIT 1", 7 },  { "CHOCO ISLAND 1", 18 }, { "KOOPA BEACH 1", 13 },
    { "DONUT PLAINS 3", 11 },  { "BATTLE COURSE 1", 20 },
};
static const CupCourse kFlowerCup[5] = {
    { "DONUT PLAINS 1", 19 },  { "GHOST VALLEY 1", 16 }, { "CHOCO ISLAND 2", 10 },
    { "KOOPA BEACH 2", 6 },    { "BATTLE COURSE 2", 21 },
};
static const CupCourse kStarCup[5] = {
    { "GHOST VALLEY 2", 1 },   { "DONUT PLAINS 2", 2 },   { "VANILLA LAKE 1", 12 },
    { "GHOST VALLEY 3", 8 },   { "BATTLE COURSE 3", 22 },
};
static const CupCourse kSpecialCup[5] = {
    { "BOWSER CASTLE 1", 17 }, { "BOWSER CASTLE 2", 3 },  { "BOWSER CASTLE 3", 9 },
    { "VANILLA LAKE 2", 4 },   { "RAINBOW ROAD", 5 },
};
static const CupCourse* kCups[4] = { kMushroomCup, kFlowerCup, kStarCup, kSpecialCup };
static const char* kCupNames[4] = { "MUSHROOM CUP", "FLOWER CUP", "STAR CUP", "SPECIAL CUP" };

// the 8 driver sheets, in driver order (index 3 is the "Donkey Kong Jr." one)
struct DriverArt { std::string name; pngread::Image sheet; int x, y, w, h; bool ok; };

// ------------------------------------------------------------------- helpers
static void centreText(HDC dc, const RECT& box, const char* s, COLORREF col, int px, int weight) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    HFONT f = CreateFontA(px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, f);
    RECT r = box;
    DrawTextA(dc, s, -1, &r, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(f);
}

static void box(HDC dc, const RECT& r, COLORREF fill, COLORREF edge) {
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(dc, &r, b);
    DeleteObject(b);
    FrameRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    (void)edge;
}

struct MenuResult { int chr = 0, cup = 0, course = 7; };

// ---------------------------------------------------------------- the screens
// One window reused for every screen; `stage` decides what is painted and which
// keys do what.
enum Stage { STAGE_TITLE, STAGE_CHAR, STAGE_PLAYERS, STAGE_CUP, STAGE_COURSE };

struct MenuState {
    Stage stage = STAGE_TITLE;
    MenuResult r;
    TitleArt title;
    std::vector<DriverArt> drivers;
    std::vector<pngread::Image> trackArt;      // indexed by course id
    std::vector<bool> trackOk;
    std::string trackName[24];
    const char* hint = "";
    DWORD born = 0;
    int players = 1;                            // 1..8
    int cursor[8] = { 0, 1, 2, 3, 4, 5, 6, 7 }; // driver per slot
    int active = 0;                             // slot being edited
};

static std::string artDir() {
    char base[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, base, MAX_PATH);
    std::string dir(base);
    size_t s = dir.find_last_of("\\/");
    dir = (s == std::string::npos) ? std::string() : dir.substr(0, s + 1);
    return dir;
}

// the kart picture in the top-left cell of a racer sheet, same crop the race
// view uses
static DriverArt loadDriverArt(int i) {
    DriverArt d;
    static const char* names[8] = {
        "Mario", "Luigi", "Princess Toadstool _ Peach", "Donkey Kong Jr.",
        "Yoshi", "Toad", "Koopa Troopa", "Bowser",
    };
    static const char* chars[8] = {
        "MARIO", "LUIGI", "PEACH", "DAISY", "YOSHI", "TOAD", "KOOPA", "BOWSER",
    };
    d.name = names[i];
    d.x = 2; d.y = 2; d.w = 36; d.h = 32; d.ok = false;
    const char* rel[3] = {
        "SNES - Super Mario Kart - Racers - %s.png",
        "../SNES - Super Mario Kart - Racers - %s.png",
        "../../SNES - Super Mario Kart - Racers - %s.png",
    };
    char buf[512];
    std::string dir = artDir();
    for (const char* fmt : rel) {
        std::snprintf(buf, sizeof(buf), fmt, names[i]);
        for (const std::string& root : { dir, std::string() }) {
            std::string p = root + buf;
            DWORD a = GetFileAttributesA(p.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            pngread::Image im = pngread::read(p);
            if (im.ok) { d.sheet = im; d.ok = true; return d; }
        }
    }
    (void)chars;
    return d;
}

static std::string courseFile(const char* upper) {
    std::string s = upper;
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return "SNES - Super Mario Kart - Tracks - " + s + ".png";
}

static const char* kCourseNameUpper[24] = {
    "MARIO CIRCUIT 3", "GHOST VALLEY 2", "DONUT PLAINS 2", "BOWSER CASTLE 2",
    "VANILLA LAKE 2", "RAINBOW ROAD", "KOOPA BEACH 2", "MARIO CIRCUIT 1",
    "GHOST VALLEY 3", "BOWSER CASTLE 3", "CHOCO ISLAND 2", "DONUT PLAINS 3",
    "VANILLA LAKE 1", "KOOPA BEACH 1", "MARIO CIRCUIT 4", "MARIO CIRCUIT 2",
    "GHOST VALLEY 1", "BOWSER CASTLE 1", "CHOCO ISLAND 1", "DONUT PLAINS 1",
    "BATTLE COURSE 3", "BATTLE COURSE 4", "BATTLE COURSE 1", "BATTLE COURSE 2",
};

static void loadAllTrackArt(MenuState& ms) {
    ms.trackArt.assign(24, pngread::Image());
    ms.trackOk.assign(24, false);
    std::string dir = artDir();
    const char* rel[2] = { "%s", "../%s" };
    char buf[512];
    for (int c = 0; c < 24; c++) {
        std::snprintf(buf, sizeof(buf), rel[0], courseFile(kCourseNameUpper[c]).c_str());
        for (const std::string& root : { dir, std::string() }) {
            std::string p = root + buf;
            DWORD a = GetFileAttributesA(p.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            pngread::Image im = pngread::read(p);
            if (im.ok) {
                if (im.h > im.w) { im.h = im.w; im.rgba.resize((size_t)im.w * im.h * 4); }
                if (im.w == im.h) { ms.trackArt[c] = im; ms.trackOk[c] = true;
                                    ms.trackName[c] = p; break; }
            }
        }
    }
}

// ------------------------------------------------------------------- painting
static void paintMenu(HWND hwnd, MenuState& ms) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bgc = CreateSolidBrush(RGB(0, 128, 128));      // the sheet's teal
    FillRect(dc, &rc, bgc);
    DeleteObject(bgc);

    switch (ms.stage) {
        case STAGE_TITLE:
            if (ms.title.ok) {
                blitScaled(dc, ms.title.sheet, kBgX, kBgY, kBgW, kBgH, rc);
                int lw = (rc.right - rc.left) / 2, lh = lw * kLogoH / kLogoW;
                if (lh > (rc.bottom - rc.top) / 2) { lh = (rc.bottom - rc.top) / 2;
                                                       lw = lh * kLogoW / kLogoH; }
                RECT lr = { (rc.right - lw) / 2, (rc.bottom - lh) / 5,
                            (rc.right + lw) / 2, (rc.bottom - lh) / 5 + lh };
                blitScaled(dc, ms.title.sheet, kLogoX, kLogoY, kLogoW, kLogoH, lr);
            }
            centreText(dc, RECT{0, rc.bottom - 52, rc.right, rc.bottom - 26},
                       "press Enter or click to start", RGB(255,255,255), 15, FW_NORMAL);
            return;

        case STAGE_CHAR: {
            centreText(dc, RECT{0, 8, rc.right, 34}, "CHOOSE YOUR DRIVER",
                       RGB(255, 220, 80), 20, FW_BOLD);
            // 4 x 2 grid of boxes
            int cw = 150, chh = 120, gap = 12;
            int totalW = 4 * cw + 3 * gap;
            int ox = (rc.right - totalW) / 2, oy = 70;
            for (int i = 0; i < 8; i++) {
                int cx = ox + (i % 4) * (cw + gap);
                int cy = oy + (i / 4) * (chh + gap);
                RECT r = { cx, cy, cx + cw, cy + chh };
                box(dc, r, RGB(240, 240, 240), RGB(0,0,0));
                if (i < (int)ms.drivers.size() && ms.drivers[i].ok) {
                    blitScaled(dc, ms.drivers[i].sheet, ms.drivers[i].x, ms.drivers[i].y,
                               ms.drivers[i].w, ms.drivers[i].h,
                               RECT{ r.left + 4, r.top + 6, r.right - 4, r.bottom - 26 });
                }
                if (i == ms.r.chr) {
                    RECT hl = { r.left - 3, r.top - 3, r.right + 3, r.bottom + 3 };
                    HBRUSH hb = CreateSolidBrush(RGB(255, 60, 40));
                    FrameRect(dc, &hl, hb);
                    DeleteObject(hb);
                }
                centreText(dc, RECT{ r.left, r.bottom - 24, r.right, r.bottom - 4 },
                           ms.drivers[i].name.c_str(), RGB(20,20,20), 13, FW_BOLD);
            }
            centreText(dc, RECT{0, rc.bottom - 46, rc.right, rc.bottom - 20},
                       ms.hint, RGB(255,255,255), 14, FW_NORMAL);
            return;
        }

        case STAGE_PLAYERS: {
            centreText(dc, RECT{0, 8, rc.right, 34}, "PLAYERS",
                       RGB(255, 220, 80), 20, FW_BOLD);
            char n[64];
            std::snprintf(n, sizeof(n), "%d", ms.players);
            centreText(dc, RECT{0, 52, rc.right, 92}, n, RGB(255,255,255), 30, FW_BOLD);
            // 8 slots; the active one is highlighted
            int cw = 120, chh = 92, gap = 10;
            int totalW = 4 * cw + 3 * gap;
            int ox = (rc.right - totalW) / 2, oy = 140;
            for (int i = 0; i < 8; i++) {
                int cx = ox + (i % 4) * (cw + gap);
                int cy = oy + (i / 4) * (chh + gap);
                RECT r = { cx, cy, cx + cw, cy + chh };
                bool used = i < ms.players;
                box(dc, r, used ? RGB(240,240,240) : RGB(110,110,110), RGB(0,0,0));
                if (used && i < (int)ms.drivers.size() && ms.drivers[i].ok) {
                    blitScaled(dc, ms.drivers[ms.cursor[i]].sheet,
                               ms.drivers[ms.cursor[i]].x, ms.drivers[ms.cursor[i]].y,
                               ms.drivers[ms.cursor[i]].w, ms.drivers[ms.cursor[i]].h,
                               RECT{ r.left + 4, r.top + 6, r.right - 4, r.bottom - 20 });
                } else {
                    centreText(dc, r, "-", RGB(70,70,70), 18, FW_BOLD);
                }
                if (i == ms.active && used) {
                    HBRUSH hb = CreateSolidBrush(RGB(255, 60, 40));
                    RECT hl = { r.left - 3, r.top - 3, r.right + 3, r.bottom + 3 };
                    FrameRect(dc, &hl, hb);
                    DeleteObject(hb);
                }
                centreText(dc, RECT{ r.left, r.bottom - 18, r.right, r.bottom - 2 },
                           used ? ms.drivers[ms.cursor[i]].name.c_str() : "empty",
                           used ? RGB(20,20,20) : RGB(200,200,200), 11, FW_BOLD);
            }
            centreText(dc, RECT{0, rc.bottom - 46, rc.right, rc.bottom - 20},
                       ms.hint, RGB(255,255,255), 14, FW_NORMAL);
            return;
        }

        case STAGE_CUP: {
            centreText(dc, RECT{0, 8, rc.right, 34}, "SELECT YOUR CUP",
                       RGB(255, 220, 80), 20, FW_BOLD);
            int cw = 250, chh = 70, gap = 16;
            for (int i = 0; i < 4; i++) {
                int cx = 40 + i * (cw + gap);
                int cy = 90;
                RECT r = { cx, cy, cx + cw, cy + chh };
                box(dc, r, RGB(200, 190, 170), RGB(0,0,0));
                if (i == ms.r.cup) {
                    HBRUSH hb = CreateSolidBrush(RGB(255, 60, 40));
                    FrameRect(dc, &r, hb);
                    DeleteObject(hb);
                }
                centreText(dc, r, kCupNames[i], RGB(20,20,20), 15, FW_BOLD);
            }
            // the chosen cup's courses
            const CupCourse* cup = kCups[ms.r.cup];
            int y0 = 200;
            for (int i = 0; i < 5; i++) {
                RECT r = { 40, y0 + i * 26, rc.right - 40, y0 + i * 26 + 22 };
                if (i == ms.r.course) {
                    HBRUSH hb = CreateSolidBrush(RGB(255, 240, 120));
                    FillRect(dc, &r, hb);
                    DeleteObject(hb);
                }
                char line[128];
                std::snprintf(line, sizeof(line), "%d.  %s", i + 1, cup[i].name);
                centreText(dc, r, line, RGB(255,255,255), 14, i == ms.r.course ? FW_BOLD : FW_NORMAL);
            }
            centreText(dc, RECT{0, rc.bottom - 46, rc.right, rc.bottom - 20},
                       ms.hint, RGB(255,255,255), 14, FW_NORMAL);
            return;
        }

        case STAGE_COURSE: {
            const CupCourse* cup = kCups[ms.r.cup];
            centreText(dc, RECT{0, 6, rc.right, 30}, "COURSE SELECT",
                       RGB(255, 220, 80), 18, FW_BOLD);
            // preview of the highlighted course, from its track picture
            int id = cup[ms.r.course].courseId;
            RECT pv = { rc.right - 350, 44, rc.right - 20, 44 + 320 };
            box(dc, pv, RGB(168, 208, 136), RGB(0,0,0));   // green outer edge
            if (id >= 0 && id < 24 && ms.trackOk[id]) {
                RECT inner = { pv.left + 8, pv.top + 8, pv.right - 8, pv.top + 8 + (pv.right-pv.left-16) };
                blitScaled(dc, ms.trackArt[id], 0, 0, ms.trackArt[id].w, ms.trackArt[id].h, inner);
            } else {
                centreText(dc, pv, "no art", RGB(60,60,60), 14, FW_NORMAL);
            }
            for (int i = 0; i < 5; i++) {
                RECT r = { 30, 54 + i * 30, rc.right - 370, 54 + i * 30 + 26 };
                if (i == ms.r.course) {
                    HBRUSH hb = CreateSolidBrush(RGB(255, 240, 120));
                    FillRect(dc, &r, hb);
                    DeleteObject(hb);
                }
                char line[128];
                std::snprintf(line, sizeof(line), "%s", cup[i].name);
                centreText(dc, r, line, RGB(255,255,255), 15,
                           i == ms.r.course ? FW_BOLD : FW_NORMAL);
            }
            centreText(dc, RECT{0, rc.bottom - 46, rc.right, rc.bottom - 20},
                       ms.hint, RGB(255,255,255), 14, FW_NORMAL);
            return;
        }
    }
    EndPaint(hwnd, &ps);
}

static MenuResult runMenus(HINSTANCE inst) {
    MenuState ms;
    ms.title = loadTitleArt();
    for (int i = 0; i < 8; i++) ms.drivers.push_back(loadDriverArt(i));
    loadAllTrackArt(ms);
    ms.r.cup = 0; ms.r.course = 0; ms.r.chr = 0;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        MenuState* s = (MenuState*)GetWindowLongPtrA(h, GWLP_USERDATA);
        switch (m) {
            case WM_PAINT:      if (s) paintMenu(h, *s); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_KEYDOWN: {
                if (!s) break;
                // A keypress that dismissed the file dialog must not also skip
                // the title screen, so ignore input for a moment after opening.
                if (GetTickCount() - s->born < 700) break;
                if (s->stage == STAGE_TITLE) { s->stage = STAGE_CHAR; s->hint = "arrows to choose, Enter to confirm"; break; }
                if (s->stage == STAGE_CHAR) {
                    if (w == VK_LEFT)  s->r.chr = (s->r.chr + 7) & 7;
                    if (w == VK_RIGHT) s->r.chr = (s->r.chr + 1) & 7;
                    if (w == VK_UP)    s->r.chr = (s->r.chr + 4) & 7;
                    if (w == VK_DOWN)  s->r.chr = (s->r.chr + 4) & 7;
                    if (w == VK_RETURN || w == VK_SPACE) {
                        s->r.chr = s->cursor[s->r.chr & 7];
                        s->r.course = 0; s->active = 0; s->stage = STAGE_PLAYERS;
                        s->hint = "arrows set the count, Tab picks a slot, Enter to continue";
                    }
                    break;
                }
                if (s->stage == STAGE_PLAYERS) {
                    // up/down sets how many players; left/right picks the
                    // driver in the highlighted slot; Tab moves the slot
                    if (w == VK_UP)    s->players = s->players % 8 + 1;
                    if (w == VK_DOWN)  s->players = (s->players + 6) % 8 + 1;
                    if (w == VK_TAB)   s->active = (s->active + 1) % 8;
                    if (w == VK_LEFT)  s->cursor[s->active] = (s->cursor[s->active] + 7) & 7;
                    if (w == VK_RIGHT) s->cursor[s->active] = (s->cursor[s->active] + 1) & 7;
                    if (w == VK_RETURN || w == VK_SPACE) {
                        s->r.chr = s->cursor[0];
                        s->r.course = 0; s->stage = STAGE_CUP;
                        s->hint = "arrows to choose a course, Enter to confirm";
                    }
                    break;
                }
                if (s->stage == STAGE_CUP) {
                    if (w == VK_LEFT || w == VK_UP)    s->r.course = (s->r.course + 4) % 5;
                    if (w == VK_RIGHT || w == VK_DOWN)  s->r.course = (s->r.course + 1) % 5;
                    if (w == VK_RETURN || w == VK_SPACE) { s->stage = STAGE_COURSE;
                        s->hint = "arrows to choose a track, Enter to race"; }
                    break;
                }
                if (s->stage == STAGE_COURSE) {
                    if (w == VK_LEFT || w == VK_UP)    s->r.course = (s->r.course + 4) % 5;
                    if (w == VK_RIGHT || w == VK_DOWN)  s->r.course = (s->r.course + 1) % 5;
                    if (w == VK_RETURN || w == VK_SPACE || w == VK_ESCAPE) {
                        s->r.course = kCups[s->r.cup][s->r.course].courseId;
                        DestroyWindow(h); return 0;
                    }
                    break;
                }
                break;
            }
            case WM_LBUTTONDOWN: {
                if (!s) break;
                if (GetTickCount() - s->born < 700) break;
                if (s->stage == STAGE_TITLE) { s->stage = STAGE_CHAR;
                    s->hint = "arrows to choose, Enter to confirm"; break; }
                if (s->stage == STAGE_COURSE) {
                    s->r.course = kCups[s->r.cup][s->r.course].courseId;
                    DestroyWindow(h); return 0;
                }
                if (s->stage == STAGE_CHAR) {
                    s->r.course = 0; s->active = 0; s->stage = STAGE_PLAYERS;
                    s->hint = "arrows set the count, Tab picks a slot, Enter to continue";
                    break;
                }
                if (s->stage == STAGE_PLAYERS) {
                    s->r.chr = s->cursor[0];
                    s->r.course = 0; s->stage = STAGE_CUP;
                    s->hint = "arrows to choose a course, Enter to confirm";
                    break;
                }
                s->stage = STAGE_COURSE;
                s->hint = "arrows to choose a track, Enter to race";
                break;
            }
            case WM_CLOSE:   DestroyWindow(h); return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        // Everything else, WM_NCCREATE included, must reach DefWindowProcW:
        // returning 0 from WM_NCCREATE makes CreateWindowExW fail outright,
        // which is why the menu window never appeared.
        InvalidateRect(h, nullptr, FALSE);
        return DefWindowProcW(h, m, w, l);
    };

    // the title names the stage, so it is obvious which screen is up
    auto stageTitle = [](Stage s) -> const wchar_t* {
        switch (s) {
            case STAGE_TITLE:   return L"Super Mario Kart - Title";
            case STAGE_CHAR:    return L"Super Mario Kart - Character select";
            case STAGE_PLAYERS: return L"Super Mario Kart - Players";
            case STAGE_CUP:     return L"Super Mario Kart - Cup select";
            case STAGE_COURSE:  return L"Super Mario Kart - Course select";
        }
        return L"Super Mario Kart";
    };
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SMKMenu";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"SMKMenu", L"Super Mario Kart - Title",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 680, 460,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "[menu] CreateWindowExW failed, error %lu\n", GetLastError());
        return ms.r;
    }
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)&ms);
    ms.born = GetTickCount();
    ShowWindow(hwnd, SW_SHOW);
    // Without this the window never gets keyboard focus, so the arrow keys and
    // Enter do nothing -- this is a console app, so the console tends to win.
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetActiveWindow(hwnd);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // keep the stage name in the title bar so it is obvious which screen
        // is up; done here because a stage change happens inside the wndproc
        const wchar_t* want = stageTitle(ms.stage);
        wchar_t cur[128] = {};
        GetWindowTextW(hwnd, cur, 128);
        if (std::wstring(cur) != want) {
            SetWindowTextW(hwnd, want);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return ms.r;
}
