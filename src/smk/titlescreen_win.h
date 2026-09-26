// titlescreen_win.h -- the window shown after the ROM is imported and before
// the race.
//
// The art comes from the project folder:
//   title/SNES - Super Mario Kart - Miscellaneous - Title & Results Background(1).png
// which is a sprite sheet.  Probing its all-black separators gives the panels:
//   logo      x   6..239   y   7..91    (234 x 85)
//   TITLE     x 246..757   y  16..271   (512 x 256)
//   RESULTS   x 764..1275  y  16..271   (512 x 256)
// with a second variant of each starting at y = 278.  The whole panel goes on
// screen, so the picture is stretched to the window.
#pragma once
#include <windows.h>

#include <algorithm>
#include <vector>

#include "../png_read.h"

// panel rectangles inside the sheet, measured from its black separators.
// The logo starts at y=16: rows 7..12 of that panel are the sheet's own
// "INTERNATIONAL" annotation, which is not part of the logo.
static const int kLogoX = 6,   kLogoY = 16,  kLogoW = 234, kLogoH = 76;
static const int kBgX   = 246, kBgY   = 16,  kBgW   = 512, kBgH   = 256;

struct TitleArt {
    pngread::Image sheet;
    bool ok = false;
    std::string err;
};

// look for the sheet next to the exe, or one/two directories up (the same
// places the track and racer art is looked for)
static TitleArt loadTitleArt() {
    TitleArt a;
    static const char* rel[] = {
        "title/SNES - Super Mario Kart - Miscellaneous - Title & Results Background(1).png",
        "../title/SNES - Super Mario Kart - Miscellaneous - Title & Results Background(1).png",
        "../../title/SNES - Super Mario Kart - Miscellaneous - Title & Results Background(1).png",
    };
    char base[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, base, MAX_PATH);
    std::string dir(base);
    size_t slash = dir.find_last_of("\\/");
    dir = (slash == std::string::npos) ? std::string() : dir.substr(0, slash + 1);

    for (const char* r : rel) {
        for (const std::string& root : { dir, std::string() }) {
            std::string p = root + r;
            DWORD a2 = GetFileAttributesA(p.c_str());
            if (a2 == INVALID_FILE_ATTRIBUTES || (a2 & FILE_ATTRIBUTE_DIRECTORY)) continue;
            pngread::Image im = pngread::read(p);
            if (im.ok) { a.sheet = im; a.ok = true; return a; }
        }
    }
    a.err = "title art not found";
    return a;
}

// stretch a source rectangle into a destination rectangle, nearest neighbour
static bool blitScaled(HDC dc, const pngread::Image& im,
                        int sx, int sy, int sw, int sh,
                        const RECT& dst) {
    int dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return false;
    for (int y = 0; y < dh; y++) {
        int iy = sy + (int)((int64_t)y * sh / dh);
        if (iy < 0 || iy >= im.h) continue;
        for (int x = 0; x < dw; x++) {
            int ix = sx + (int)((int64_t)x * sw / dw);
            if (ix < 0 || ix >= im.w) continue;
            size_t o = ((size_t)iy * im.w + ix) * 4;
            if (im.rgba[o] == 0 && im.rgba[o+1] == 0 && im.rgba[o+2] == 0) continue;
            SetPixelV(dc, dst.left + x, dst.top + y,
                      RGB(im.rgba[o], im.rgba[o+1], im.rgba[o+2]));
        }
    }
    return true;
}

static void paintTitleScreen(HWND hwnd, const TitleArt& art) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    if (art.ok) {
        // background fills the window; the logo is centred over it, scaled to
        // about half the width so it reads like the original's title screen
        blitScaled(dc, art.sheet, kBgX, kBgY, kBgW, kBgH, rc);
        int lw = (rc.right - rc.left) / 2;
        int lh = lw * kLogoH / kLogoW;
        if (lh > (rc.bottom - rc.top) / 2) {
            lh = (rc.bottom - rc.top) / 2;
            lw = lh * kLogoW / kLogoH;
        }
        RECT lr = { (rc.right - lw) / 2, (rc.bottom - lh) / 5,
                    (rc.right + lw) / 2, (rc.bottom - lh) / 5 + lh };
        blitScaled(dc, art.sheet, kLogoX, kLogoY, kLogoW, kLogoH, lr);
    }

    SetBkMode(dc, TRANSPARENT);
    HFONT sm = CreateFontA(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, sm);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT t2 = { 0, rc.bottom - 46, rc.right, rc.bottom - 20 };
    DrawTextA(dc, "press Enter or click to start", -1, &t2, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, old);
    DeleteObject(sm);
    EndPaint(hwnd, &ps);
}

[[maybe_unused]] static void startTitleScreen(HINSTANCE inst) {
    static TitleArt art;
    art = loadTitleArt();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        TitleArt* a = (TitleArt*)GetWindowLongPtrA(h, GWLP_USERDATA);
        switch (m) {
            case WM_PAINT:      if (a) paintTitleScreen(h, *a); return 0;
            case WM_ERASEBKGND: return 1;
            case WM_LBUTTONDOWN:
            case WM_KEYDOWN:
            case WM_CHAR:       DestroyWindow(h); return 0;
            case WM_CLOSE:      DestroyWindow(h); return 0;
            case WM_DESTROY:    PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"SMKTitle";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"SMKTitle", L"Super Mario Kart",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 660, 400,
                                nullptr, nullptr, inst, nullptr);
    if (!hwnd) return;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)&art);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
