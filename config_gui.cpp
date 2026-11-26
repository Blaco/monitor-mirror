// ===========================================================================
// config_gui.cpp - Implementation of the visual setup dialog (raw Win32).
//
// Two "canvas" child windows draw a monitor to scale with a draggable /
// resizable rectangle on it. The left canvas is the free-form working area on
// the main monitor; the right canvas is the mirror area, whose box is locked to
// the working area's aspect ratio. Numeric fields mirror each box two-way.
// ===========================================================================
#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include "config_gui.h"
#include "color_picker.h"
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <cmath>
#include <cwchar>
#include <algorithm>
#include <utility>
#include <initializer_list>

// ---- control ids -----------------------------------------------------------
enum {
    ID_SRC_COMBO = 100, ID_TGT_COMBO,
    ID_SRC_X = 110, ID_SRC_Y, ID_SRC_W, ID_SRC_H,
    ID_TGT_X = 120, ID_TGT_Y, ID_TGT_W, ID_TGT_H,
    ID_FPS = 130, ID_VSYNC, ID_CURSOR, ID_COVER,
    ID_ROT, ID_FLIPH, ID_FLIPV, ID_WINDOWED, ID_LOG, ID_LOCK_CURSOR, ID_LOW_LATENCY, ID_GUARD_WINDOWS,
    ID_SAVE = 200, ID_IMPORT, ID_EXPORT, ID_QUIT, ID_REFRESH, ID_IDENTIFY,
    ID_TAB=300, ID_MAG_ENABLE, ID_MAG_MODE, ID_MAG_SHAPE, ID_MAG_W, ID_MAG_H,
    ID_MAG_ZOOM, ID_MAG_EDGE, ID_MAG_BORDER, ID_MAG_COLOR, ID_MAG_SHARP, ID_MAG_KEY, ID_MAG_FEATHER,
    ID_MAG_W_EDIT=330, ID_MAG_H_EDIT, ID_MAG_ZOOM_EDIT, ID_MAG_BORDER_EDIT, ID_MAG_FEATHER_EDIT
};
static_assert(ID_GUARD_WINDOWS < ID_SAVE, "Control ID ranges must not overlap");
static_assert(ID_MAG_FEATHER < ID_MAG_W_EDIT, "Magnifier control IDs must not overlap");
static const UINT WM_CANVAS_CHANGED = WM_APP + 20;   // wParam: 0=source, 1=target

// shared fonts / brushes (created in RunConfigDialog)
static HFONT  g_font = nullptr, g_hdrFont = nullptr, g_titleFont = nullptr, g_subFont = nullptr, g_btnFont = nullptr;
static HBRUSH g_bgBrush = nullptr, g_cardBrush = nullptr;
static bool g_dark = false;
static COLORREF CLR_BG, CLR_CARD, CLR_CARD_LINE, CLR_SHADOW, CLR_TEXT, CLR_SUB;
static COLORREF CLR_ACCENT, CLR_ACCENT_HI, CLR_ACCENT_LO, CLR_HDRTX, CLR_HDRSUB;
static COLORREF CLR_BANNER_BG, CLR_BANNER_LN, CLR_BANNER_TX, CLR_DANGER, CLR_DANGER_HI;

// ---- a monitor-with-a-box canvas -------------------------------------------
struct Canvas {
    HWND hwnd = nullptr, parent = nullptr;
    const MonitorInfo* mon = nullptr;
    RECT region{};                 // monitor-local pixels (left,top,right,bottom)
    bool  ratioLocked = false;
    double lockAspect = 1.0;       // w/h to preserve when ratioLocked
    int   which = 0;               // 0 source, 1 target
    bool  enabled = true;          // target stays disabled until a source is chosen
    // view mapping (monitor px -> canvas px)
    double scale = 1.0; int offX = 0, offY = 0;
    // drag
    int   dragMode = -1;           // -1 none, 8 = move, 0..7 = handles
    POINT dragMouse0{}; RECT dragRegion0{};
};

static double clampd(double v, double lo, double hi){ return v < lo ? lo : (v > hi ? hi : v); }
static int    clampi(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }
static void   fillRound(HDC dc, RECT r, int rad, COLORREF fill);     // defined below
static void   strokeRound(HDC dc, RECT r, int rad, COLORREF line);

static void computeView(Canvas* c) {
    RECT cr; GetClientRect(c->hwnd, &cr);
    int cw = cr.right, ch = cr.bottom, pad = 8;
    if (!c->mon) { c->scale = 1; c->offX = c->offY = 0; return; }
    double sx = (double)(cw - 2*pad) / c->mon->w();
    double sy = (double)(ch - 2*pad) / c->mon->h();
    c->scale = std::min(sx, sy);
    if (c->scale <= 0) c->scale = 0.0001;
    c->offX = (int)((cw - c->mon->w() * c->scale) / 2);
    c->offY = (int)((ch - c->mon->h() * c->scale) / 2);
}
static POINT m2c(Canvas* c, int mx, int my) {
    return { (LONG)(c->offX + mx * c->scale), (LONG)(c->offY + my * c->scale) };
}
static POINT c2m(Canvas* c, int cx, int cy) {
    return { (LONG)((cx - c->offX) / c->scale), (LONG)((cy - c->offY) / c->scale) };
}

// The 8 handle centres in monitor-local coords, order: TL,T,TR,R,BR,B,BL,L
static void handlePoints(const RECT& r, POINT out[8]) {
    int mx = (r.left + r.right)/2, my = (r.top + r.bottom)/2;
    out[0] = { r.left,  r.top };    out[1] = { mx, r.top };    out[2] = { r.right, r.top };
    out[3] = { r.right, my };       out[4] = { r.right, r.bottom };
    out[5] = { mx, r.bottom };      out[6] = { r.left, r.bottom };  out[7] = { r.left, my };
}
static bool isCorner(int h){ return h==0||h==2||h==4||h==6; }

static void normalize(RECT& r){
    if (r.right  < r.left) std::swap(r.left, r.right);
    if (r.bottom < r.top ) std::swap(r.top,  r.bottom);
}
static void clampInside(RECT& r, int mw, int mh){
    if (r.left < 0) r.left = 0;   if (r.top < 0) r.top = 0;
    if (r.right  > mw) r.right  = mw;   if (r.bottom > mh) r.bottom = mh;
    if (r.right - r.left < 8) r.right  = std::min<LONG>(mw, r.left + 8);
    if (r.bottom - r.top < 8) r.bottom = std::min<LONG>(mh, r.top  + 8);
}

// A box centred on the monitor, `frac` of its size (e.g. 0.5 = half).
static RECT centeredBox(const MonitorInfo& m, double frac){
    int w = (int)(m.w() * frac), h = (int)(m.h() * frac);
    int x = (m.w() - w) / 2, y = (m.h() - h) / 2;
    return { x, y, x + w, y + h };
}
// A centred box that fits aspect `asp` (w/h) inside `frac` of the monitor.
static RECT centeredRatioBox(const MonitorInfo& m, double frac, double asp){
    if (asp <= 0) asp = 1;
    double bw = m.w() * frac, bh = m.h() * frac;
    double w = bw, h = w / asp;
    if (h > bh){ h = bh; w = h * asp; }
    int iw = (int)w, ih = (int)h;
    int x = (m.w() - iw) / 2, y = (m.h() - ih) / 2;
    return { x, y, x + iw, y + ih };
}

// snap region edges to the monitor edges / centre when within a few pixels.
static void snapRect(Canvas* c, RECT& r, bool moving){
    int mw = c->mon->w(), mh = c->mon->h();
    int thr = (int)(7.0 / (c->scale > 0 ? c->scale : 1.0)); if (thr < 2) thr = 2;
    auto nr = [&](int a, int b){ return abs(a - b) <= thr; };
    if (moving){
        int w = r.right - r.left, h = r.bottom - r.top;
        if      (nr(r.left, 0))                 { r.left = 0;      r.right = w; }
        else if (nr(r.right, mw))               { r.right = mw;    r.left = mw - w; }
        else if (nr((r.left+r.right)/2, mw/2))  { r.left = mw/2 - w/2; r.right = r.left + w; }
        if      (nr(r.top, 0))                  { r.top = 0;       r.bottom = h; }
        else if (nr(r.bottom, mh))              { r.bottom = mh;   r.top = mh - h; }
        else if (nr((r.top+r.bottom)/2, mh/2))  { r.top = mh/2 - h/2; r.bottom = r.top + h; }
    } else {
        if (nr(r.left, 0))   r.left = 0;
        if (nr(r.right, mw)) r.right = mw;
        if (nr(r.top, 0))    r.top = 0;
        if (nr(r.bottom, mh))r.bottom = mh;
    }
}

// resize with the aspect ratio preserved, anchored at the corner opposite `h`.
static void resizeRatio(Canvas* c, int h, int mx, int my, double asp){
    const RECT r0 = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    if (asp <= 0) asp = 1;
    int anchorX, anchorY;   // fixed corner
    int signX, signY;       // +1 grows right/down from anchor, -1 grows left/up
    switch (h){
        case 0: anchorX=r0.right; anchorY=r0.bottom; signX=-1; signY=-1; break; // TL
        case 2: anchorX=r0.left;  anchorY=r0.bottom; signX=+1; signY=-1; break; // TR
        case 4: anchorX=r0.left;  anchorY=r0.top;    signX=+1; signY=+1; break; // BR
        default:anchorX=r0.right; anchorY=r0.top;    signX=-1; signY=+1; break; // BL(6)
    }
    mx = clampi(mx, 0, mw); my = clampi(my, 0, mh);
    double availW = (signX > 0) ? (mw - anchorX) : anchorX;
    double availH = (signY > 0) ? (mh - anchorY) : anchorY;
    double w = std::fabs((double)mx - anchorX);
    w = clampd(w, 8, std::min(availW, availH * asp));
    double hh = w / asp;
    RECT r;
    if (signX > 0){ r.left = anchorX; r.right = anchorX + (int)w; }
    else          { r.right = anchorX; r.left = anchorX - (int)w; }
    if (signY > 0){ r.top = anchorY; r.bottom = anchorY + (int)hh; }
    else          { r.bottom = anchorY; r.top = anchorY - (int)hh; }
    clampInside(r, mw, mh);
    c->region = r;
}

static void resizeFree(Canvas* c, int h, int mx, int my){
    RECT r = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    mx = clampi(mx, 0, mw); my = clampi(my, 0, mh);
    if (h==0||h==6||h==7) r.left  = mx;    // left edge
    if (h==2||h==3||h==4) r.right = mx;    // right edge
    if (h==0||h==1||h==2) r.top    = my;   // top edge
    if (h==4||h==5||h==6) r.bottom = my;   // bottom edge
    normalize(r);
    snapRect(c, r, false);
    clampInside(r, mw, mh);
    c->region = r;
}

static void moveRegion(Canvas* c, int mx, int my){
    RECT r = c->dragRegion0;
    int mw = c->mon->w(), mh = c->mon->h();
    int dx = mx - c->dragMouse0.x, dy = my - c->dragMouse0.y;
    r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
    if (r.left < 0){ r.right -= r.left; r.left = 0; }
    if (r.top  < 0){ r.bottom -= r.top; r.top = 0; }
    if (r.right  > mw){ int o = r.right - mw; r.left -= o; r.right -= o; }
    if (r.bottom > mh){ int o = r.bottom - mh; r.top -= o; r.bottom -= o; }
    c->region = r;
}

// nudge / clamp-shift the whole region by (dx,dy) monitor px (arrow keys).
static void nudgeRegion(Canvas* c, int dx, int dy){
    int mw = c->mon->w(), mh = c->mon->h();
    RECT r = c->region;
    r.left += dx; r.right += dx; r.top += dy; r.bottom += dy;
    if (r.left < 0){ r.right -= r.left; r.left = 0; }
    if (r.top  < 0){ r.bottom -= r.top; r.top = 0; }
    if (r.right  > mw){ int o = r.right - mw; r.left -= o; r.right -= o; }
    if (r.bottom > mh){ int o = r.bottom - mh; r.top -= o; r.bottom -= o; }
    c->region = r;
}

static int hitTest(Canvas* c, int cx, int cy){
    if (!c->mon) return -1;
    POINT hp[8]; handlePoints(c->region, hp);
    for (int i = 0; i < 8; ++i){
        if (c->ratioLocked && !isCorner(i)) continue;  // locked: corners only
        POINT p = m2c(c, hp[i].x, hp[i].y);
        if (abs(cx - p.x) <= 6 && abs(cy - p.y) <= 6) return i;
    }
    POINT a = m2c(c, c->region.left, c->region.top);
    POINT b = m2c(c, c->region.right, c->region.bottom);
    if (cx > a.x && cx < b.x && cy > a.y && cy < b.y) return 8;  // move
    return -1;
}

static LPCWSTR cursorFor(int h){
    switch (h){
        case 0: case 4: return IDC_SIZENWSE;
        case 2: case 6: return IDC_SIZENESW;
        case 1: case 5: return IDC_SIZENS;
        case 3: case 7: return IDC_SIZEWE;
        case 8:         return IDC_SIZEALL;
        default:        return IDC_ARROW;
    }
}

static void paintCanvas(Canvas* c){
    PAINTSTRUCT ps; HDC dc = BeginPaint(c->hwnd, &ps);
    RECT cr; GetClientRect(c->hwnd, &cr);
    // double buffer
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(CLR_CARD); FillRect(mem, &cr, bg); DeleteObject(bg);

    if (c->mon){
        computeView(c);
        SetBkMode(mem, TRANSPARENT);

        // monitor body (rounded dark panel)
        POINT a = m2c(c, 0, 0), b = m2c(c, c->mon->w(), c->mon->h());
        RECT mr = { a.x, a.y, b.x, b.y };
        fillRound(mem, mr, 8, RGB(37, 42, 53));
        strokeRound(mem, mr, 8, RGB(22, 26, 34));

        const COLORREF regFill = c->ratioLocked ? RGB(36, 168, 124) : RGB(70, 130, 232);
        const COLORREF regEdge = c->ratioLocked ? RGB(120, 226, 186) : RGB(150, 190, 255);

        // region rectangle
        POINT ra = m2c(c, c->region.left, c->region.top);
        POINT rb = m2c(c, c->region.right, c->region.bottom);
        RECT rr = { ra.x, ra.y, rb.x, rb.y };
        fillRound(mem, rr, 4, regFill);
        HPEN pen = CreatePen(PS_SOLID, 2, regEdge);
        HGDIOBJ op = SelectObject(mem, pen), ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
        RoundRect(mem, rr.left, rr.top, rr.right, rr.bottom, 4, 4);
        SelectObject(mem, ob); SelectObject(mem, op); DeleteObject(pen);

        // dimension text centred in the region
        wchar_t txt[64];
        swprintf(txt, 64, L"%d × %d", c->region.right - c->region.left, c->region.bottom - c->region.top);
        SetTextColor(mem, RGB(255, 255, 255));
        SelectObject(mem, g_subFont ? g_subFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(mem, txt, -1, &rr, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOCLIP);

        // handles as circles (white fill, coloured ring)
        POINT hp[8]; handlePoints(c->region, hp);
        HPEN hpen = CreatePen(PS_SOLID, 2, regEdge);
        HBRUSH hbr = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ op2 = SelectObject(mem, hpen), ob2 = SelectObject(mem, hbr);
        for (int i = 0; i < 8; ++i){
            if (c->ratioLocked && !isCorner(i)) continue;
            POINT p = m2c(c, hp[i].x, hp[i].y);
            Ellipse(mem, p.x - 5, p.y - 5, p.x + 5, p.y + 5);
        }
        SelectObject(mem, ob2); SelectObject(mem, op2); DeleteObject(hpen); DeleteObject(hbr);
    } else {
        SetBkMode(mem, TRANSPARENT); SetTextColor(mem, RGB(150,155,164));
        SelectObject(mem, g_font ? g_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        LPCWSTR hint = c->enabled ? L"Select a monitor above."
                                  : L"Select the main working monitor first.";
        DrawTextW(mem, hint, -1, &cr, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }

    BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(c->hwnd, &ps);
}

static LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    Canvas* c = (Canvas*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg){
    case WM_PAINT: if (c) paintCanvas(c); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;   // let arrow keys reach us for nudging
    case WM_LBUTTONDOWN: {
        if (!c || !c->mon) return 0;
        SetFocus(hwnd);                            // so arrow-key nudging works after a click
        int h = hitTest(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h < 0) return 0;
        c->dragMode = h;
        c->dragRegion0 = c->region;
        c->dragMouse0 = c2m(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!c || !c->mon) return 0;
        POINT mm = c2m(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (c->dragMode < 0){
            int h = hitTest(c, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            SetCursor(LoadCursorW(nullptr, cursorFor(h)));
            return 0;
        }
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (c->dragMode == 8)          moveRegion(c, mm.x, mm.y);
        else if (c->ratioLocked)       resizeRatio(c, c->dragMode, mm.x, mm.y, c->lockAspect);
        else if (shift && isCorner(c->dragMode)){   // hold Shift = keep the working area's shape
            const RECT& r0 = c->dragRegion0;
            double a = (r0.bottom > r0.top) ? (double)(r0.right - r0.left) / (r0.bottom - r0.top) : 1.0;
            resizeRatio(c, c->dragMode, mm.x, mm.y, a);
        } else                         resizeFree(c, c->dragMode, mm.x, mm.y);
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(c->parent, WM_CANVAS_CHANGED, (WPARAM)c->which, 0);
        return 0;
    }
    case WM_KEYDOWN: {
        if (!c || !c->mon) return 0;
        int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
        int dx = 0, dy = 0;
        if      (wp == VK_LEFT)  dx = -step;
        else if (wp == VK_RIGHT) dx =  step;
        else if (wp == VK_UP)    dy = -step;
        else if (wp == VK_DOWN)  dy =  step;
        else return 0;
        nudgeRegion(c, dx, dy);
        InvalidateRect(hwnd, nullptr, FALSE);
        SendMessageW(c->parent, WM_CANVAS_CHANGED, (WPARAM)c->which, 0);
        return 0;
    }
    case WM_LBUTTONUP:
        if (c && c->dragMode >= 0){ c->dragMode = -1; ReleaseCapture();
            SendMessageW(c->parent, WM_CANVAS_CHANGED, (WPARAM)c->which, 0); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;  // we set it in WM_MOUSEMOVE
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- dialog state ----------------------------------------------------------
struct DlgState {
    std::vector<MonitorInfo> monsStore;                 // owned; re-enumerated by Refresh
    const std::vector<MonitorInfo>* mons = nullptr;     // -> &monsStore
    MirrorConfig* out = nullptr;
    std::wstring  defaultPath;
    ConfigResult  result = ConfigResult::Quit;
    bool          done = false;
    bool          syncing = false;       // guard against edit<->canvas feedback
    Canvas src, tgt;
    HWND hSrcCombo=0, hTgtCombo=0;
    HWND hSrcX=0,hSrcY=0,hSrcW=0,hSrcH=0;
    HWND hTgtX=0,hTgtY=0,hTgtW=0,hTgtH=0;
    HWND hFps=0, hVsync=0, hCursor=0, hCover=0;

    HWND hRot=0, hFlipH=0, hFlipV=0, hWindowed=0, hLog=0, hLockCursor=0, hLowLatency=0, hGuardWindows=0;
    HWND hTab=nullptr, hMagEnable=nullptr, hMagMode=nullptr, hMagShape=nullptr;
    HWND hMagW=nullptr, hMagH=nullptr, hMagZoom=nullptr, hMagEdge=nullptr;
    HWND hMagBorder=nullptr, hMagColor=nullptr, hMagSharp=nullptr, hMagKey=nullptr;
    HWND hMagWValue=nullptr, hMagHValue=nullptr, hMagZoomValue=nullptr, hMagBorderValue=nullptr;
    HWND hMagFeather=nullptr, hMagFeatherValue=nullptr;
    bool syncingMagnifier=false;       // guard against slider <-> edit feedback
    COLORREF magColor=RGB(255,255,255);
    std::vector<HWND> mirrorPage, magPage;
    int page=0;
    HWND tooltips=nullptr;       // native option help; owned by this setup window
    HWND hover=nullptr;          // button currently hovered (for owner-draw)
    std::wstring banner;         // info/error strip text (painted, not a control)
    std::vector<HWND> identifyWindows; // temporary non-activating monitor labels
};

// Identify overlays use the setup UI thread, not the rendering thread. They
// are tool windows with no activation, so Window Guard leaves them alone.
static constexpr UINT_PTR IDENTIFY_TIMER = 1;
static LRESULT CALLBACK IdentifyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT r{}; GetClientRect(hwnd, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        wchar_t number[32]{}; GetWindowTextW(hwnd, number, 32);
        HFONT font = CreateFontW(-std::max(1L, (r.bottom-r.top)*2/3), 0,0,0, FW_BOLD,
                                 0,0,0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
        HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255,255,255));
        DrawTextW(dc, number, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if (font) { SelectObject(dc, old); DeleteObject(font); }
        EndPaint(hwnd, &ps); return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void hideIdentify(HWND hwnd, DlgState* s) {
    KillTimer(hwnd, IDENTIFY_TIMER);
    for (HWND w : s->identifyWindows) if (IsWindow(w)) DestroyWindow(w);
    s->identifyWindows.clear();
}

static void showIdentify(HWND hwnd, DlgState* s) {
    hideIdentify(hwnd, s);
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = IdentifyProc; wc.hInstance = hi;
    wc.lpszClassName = L"MirrorIdentifyOverlay";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    for (const auto& mon : *s->mons) {
        int w = std::max(1, std::min(240, mon.w()));
        int h = std::max(1, std::min(180, mon.h()));
        int x = mon.rect.left + (mon.w()-w)/2, y = mon.rect.top + (mon.h()-h)/2;
        std::wstring number = std::to_wstring(mon.number);
        HWND overlay = CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|
                                       WS_EX_LAYERED|WS_EX_TRANSPARENT,
            wc.lpszClassName, number.c_str(), WS_POPUP, x,y,w,h, nullptr,nullptr,hi,nullptr);
        if (!overlay) continue;
        if (!SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA)) { DestroyWindow(overlay); continue; }
        // Display on the physical screen without also copying this label into
        // the mirror when the capture API honors capture exclusion (Win 10/11).
        SetWindowDisplayAffinity(overlay, 0x00000011); // WDA_EXCLUDEFROMCAPTURE
        s->identifyWindows.push_back(overlay);
        SetWindowPos(overlay, HWND_TOPMOST, x,y,w,h, SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
    if (!s->identifyWindows.empty() && !SetTimer(hwnd, IDENTIFY_TIMER, 3000, nullptr)) hideIdentify(hwnd, s);
}

static void setInt(HWND e, int v){ wchar_t b[16]; swprintf(b,16,L"%d",v); SetWindowTextW(e,b); }
static int  getInt(HWND e){ wchar_t b[16]={0}; GetWindowTextW(e,b,16); return _wtoi(b); }
// Like setInt, but never rewrites the control the user is typing in — SetWindowText
// resets the caret to the start, which would reverse what's being typed.
static void setIntF(HWND e, int v){ if (e == GetFocus()) return; setInt(e, v); }

// rotation currently selected in the dialog (degrees), and the resulting
// aspect the target box must have (rotation by 90/270 swaps w/h).
static int dlgRotation(DlgState* s){
    int i = (int)SendMessageW(s->hRot, CB_GETCURSEL, 0, 0);
    return (i <= 0) ? 0 : i * 90;
}
static double dlgEffAspect(DlgState* s){
    int w = s->src.region.right - s->src.region.left;
    int h = s->src.region.bottom - s->src.region.top;
    if (w <= 0 || h <= 0) return 1.0;
    double a = (double)w / h;
    int rot = dlgRotation(s);
    return (rot == 90 || rot == 270) ? 1.0 / a : a;
}

static void writeSrcFields(DlgState* s){
    s->syncing = true;
    setIntF(s->hSrcX, s->src.region.left);
    setIntF(s->hSrcY, s->src.region.top);
    setIntF(s->hSrcW, s->src.region.right - s->src.region.left);
    setIntF(s->hSrcH, s->src.region.bottom - s->src.region.top);
    s->syncing = false;
}
static void writeTgtFields(DlgState* s){
    s->syncing = true;
    setIntF(s->hTgtX, s->tgt.region.left);
    setIntF(s->hTgtY, s->tgt.region.top);
    setIntF(s->hTgtW, s->tgt.region.right - s->tgt.region.left);
    setIntF(s->hTgtH, s->tgt.region.bottom - s->tgt.region.top);
    s->syncing = false;
}

// re-shape the target box to the (rotation-adjusted) source aspect
static void refitTarget(DlgState* s){
    if (!s->tgt.mon) return;
    double asp = dlgEffAspect(s);
    s->tgt.lockAspect = asp;
    int mw = s->tgt.mon->w(), mh = s->tgt.mon->h();
    RECT& r = s->tgt.region;
    int w = r.right - r.left; if (w < 8) w = 8;
    int h = (int)std::lround(w / asp); if (h < 8){ h = 8; w = (int)std::lround(h*asp); }
    if (w > mw){ w = mw; h = (int)std::lround(w/asp); }
    if (h > mh){ h = mh; w = (int)std::lround(h*asp); }
    r.right = r.left + w; r.bottom = r.top + h;
    if (r.right  > mw){ int o = r.right - mw; r.left -= o; r.right -= o; if (r.left<0) r.left=0; }
    if (r.bottom > mh){ int o = r.bottom - mh; r.top -= o; r.bottom -= o; if (r.top<0) r.top=0; }
}

static void selectMonitorIdx(HWND combo, int idx){
    SendMessageW(combo, CB_SETCURSEL, idx < 0 ? (WPARAM)-1 : (WPARAM)idx, 0);
}

// The mirror monitor can only be chosen once a working monitor is picked (so its
// box can be initialised at the working area's aspect ratio).
static void setTargetEnabled(DlgState* s, bool on){
    s->tgt.enabled = on;
    EnableWindow(s->hTgtCombo, on);
    EnableWindow(s->tgt.hwnd, on);
    EnableWindow(s->hTgtX, on); EnableWindow(s->hTgtY, on); EnableWindow(s->hTgtW, on);
    if (!on){ SendMessageW(s->hTgtCombo, CB_SETCURSEL, (WPARAM)-1, 0); s->tgt.mon = nullptr; }
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
}

static void updateModeControls(DlgState* s) {
    bool follow = (int)SendMessageW(s->hSrcCombo, CB_GETCURSEL, 0, 0) == (int)s->mons->size();
    bool low = SendMessageW(s->hLowLatency, BM_GETCHECK, 0, 0) == BST_CHECKED;
    for (HWND h : {s->hSrcX,s->hSrcY,s->hSrcW,s->hSrcH,s->src.hwnd,
                   s->hCover,s->hRot,s->hFlipH,s->hFlipV,s->hWindowed}) EnableWindow(h, !follow);
    s->src.enabled = !follow;
    bool targetRegion = !follow && s->src.mon != nullptr;
    s->tgt.enabled = targetRegion;
    for (HWND h : {s->hTgtX,s->hTgtY,s->hTgtW,s->tgt.hwnd}) EnableWindow(h, targetRegion);
    EnableWindow(s->hFps, !follow && !low);
    bool dedicated = follow || SendMessageW(s->hWindowed, BM_GETCHECK, 0, 0) != BST_CHECKED;
    EnableWindow(s->hGuardWindows, dedicated);
    if (follow) {
        if (s->src.mon) s->src.region = {0,0,s->src.mon->w(),s->src.mon->h()};
        if (s->tgt.mon) s->tgt.region = {0,0,s->tgt.mon->w(),s->tgt.mon->h()};
        writeSrcFields(s); writeTgtFields(s);
    }
    InvalidateRect(s->src.hwnd, nullptr, FALSE);
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
}

// Native trackbars store the original integer settings without quantization.
static int sliderValue(HWND slider) {
    return (int)SendMessageW(slider, TBM_GETPOS, 0, 0);
}
struct MagnifierField { HWND slider, edit; int id; };
static void writeMagnifierValue(DlgState* s, HWND slider, HWND edit) {
    const bool wasSyncing=s->syncingMagnifier;
    s->syncingMagnifier=true;
    std::wstring text=std::to_wstring(sliderValue(slider));
    SetWindowTextW(edit,text.c_str());
    s->syncingMagnifier=wasSyncing;
}
// Permit incomplete input while typing; clamp on focus loss or Save/Export.
// An empty field restores the last valid slider value.
static void editMagnifierValue(DlgState* s, HWND slider, HWND edit, bool commit) {
    if(s->syncingMagnifier || !slider || !edit) return;
    wchar_t buf[16]{};
    int n=GetWindowTextW(edit,buf,16);
    int lo=(int)SendMessageW(slider,TBM_GETRANGEMIN,0,0);
    int hi=(int)SendMessageW(slider,TBM_GETRANGEMAX,0,0);
    int v=0;
    bool valid=n>0;
    for(int i=0;i<n;++i) {
        if(buf[i]<L'0'||buf[i]>L'9') {valid=false; break;}
        // Saturate before multiplying to avoid overflow even on pasted input.
        v=std::min(hi+1,v*10+int(buf[i]-L'0'));
    }
    if(valid && (commit || (v>=lo && v<=hi))) {
        v=clampi(v,lo,hi);
        if(sliderValue(slider)!=v) SendMessageW(slider,TBM_SETPOS,TRUE,v);
    }
    if(commit) writeMagnifierValue(s,slider,edit);
}
static void updateMagnifierValues(DlgState* s) {
    for(const auto& f : {MagnifierField{s->hMagW,s->hMagWValue,ID_MAG_W_EDIT},
        {s->hMagH,s->hMagHValue,ID_MAG_H_EDIT},{s->hMagZoom,s->hMagZoomValue,ID_MAG_ZOOM_EDIT},
        {s->hMagBorder,s->hMagBorderValue,ID_MAG_BORDER_EDIT},{s->hMagFeather,s->hMagFeatherValue,ID_MAG_FEATHER_EDIT}})
        writeMagnifierValue(s,f.slider,f.edit);
}
static bool handleMagnifierEdit(DlgState* s, int id, int code) {
    if(code!=EN_CHANGE && code!=EN_KILLFOCUS) return false;
    for(const auto& f : {MagnifierField{s->hMagW,s->hMagWValue,ID_MAG_W_EDIT},
        {s->hMagH,s->hMagHValue,ID_MAG_H_EDIT},{s->hMagZoom,s->hMagZoomValue,ID_MAG_ZOOM_EDIT},
        {s->hMagBorder,s->hMagBorderValue,ID_MAG_BORDER_EDIT},{s->hMagFeather,s->hMagFeatherValue,ID_MAG_FEATHER_EDIT}})
        if(id==f.id) {editMagnifierValue(s,f.slider,f.edit,code==EN_KILLFOCUS); return true;}
    return false;
}
static void commitMagnifierValues(DlgState* s) {
    for(const auto& f : {MagnifierField{s->hMagW,s->hMagWValue,ID_MAG_W_EDIT},
        {s->hMagH,s->hMagHValue,ID_MAG_H_EDIT},{s->hMagZoom,s->hMagZoomValue,ID_MAG_ZOOM_EDIT},
        {s->hMagBorder,s->hMagBorderValue,ID_MAG_BORDER_EDIT},{s->hMagFeather,s->hMagFeatherValue,ID_MAG_FEATHER_EDIT}})
        editMagnifierValue(s,f.slider,f.edit,true);
}
static void updateMagnifierControls(DlgState* s) {
    bool enabled=SendMessageW(s->hMagEnable,BM_GETCHECK,0,0)==BST_CHECKED;
    for (HWND h : {s->hMagMode,s->hMagShape,s->hMagW,s->hMagH,s->hMagZoom,s->hMagEdge,s->hMagBorder,s->hMagColor,s->hMagSharp,s->hMagFeather}) EnableWindow(h,enabled);
    EnableWindow(s->hMagKey,enabled && SendMessageW(s->hMagMode,CB_GETCURSEL,0,0)!=2);
    for (HWND h : {s->hMagWValue,s->hMagHValue,s->hMagZoomValue,s->hMagBorderValue,s->hMagFeatherValue}) EnableWindow(h,enabled);
}
static void loadMagnifier(DlgState* s, MagnifierConfig m) {
    m.normalize();
    SendMessageW(s->hMagEnable,BM_SETCHECK,m.enabled?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(s->hMagSharp,BM_SETCHECK,m.sharp?BST_CHECKED:BST_UNCHECKED,0);
    for (auto item : {std::pair<HWND,int>{s->hMagMode,m.mode},{s->hMagShape,m.shape},{s->hMagEdge,m.edge}})
        SendMessageW(item.first,CB_SETCURSEL,item.second,0);
    SendMessageW(s->hMagW,TBM_SETPOS,TRUE,m.width);
    SendMessageW(s->hMagH,TBM_SETPOS,TRUE,m.height);
    SendMessageW(s->hMagZoom,TBM_SETPOS,TRUE,m.zoom);
    SendMessageW(s->hMagBorder,TBM_SETPOS,TRUE,m.border);
    SendMessageW(s->hMagFeather,TBM_SETPOS,TRUE,m.feather);
    s->magColor=COLORREF(m.color); InvalidateRect(s->hMagColor,nullptr,TRUE);
    updateMagnifierValues(s);
    BYTE mods=0;
    if(m.modifiers&MOD_CONTROL) mods|=HOTKEYF_CONTROL;
    if(m.modifiers&MOD_ALT) mods|=HOTKEYF_ALT;
    if(m.modifiers&MOD_SHIFT) mods|=HOTKEYF_SHIFT;
    SendMessageW(s->hMagKey,HKM_SETHOTKEY,MAKEWORD(m.key,mods),0);
    updateMagnifierControls(s);
}
static MagnifierConfig readMagnifier(DlgState* s) {
    commitMagnifierValues(s);
    MagnifierConfig m;
    m.enabled=SendMessageW(s->hMagEnable,BM_GETCHECK,0,0)==BST_CHECKED;
    m.sharp=SendMessageW(s->hMagSharp,BM_GETCHECK,0,0)==BST_CHECKED;
    m.mode=(int)SendMessageW(s->hMagMode,CB_GETCURSEL,0,0);
    m.shape=(int)SendMessageW(s->hMagShape,CB_GETCURSEL,0,0);
    m.edge=(int)SendMessageW(s->hMagEdge,CB_GETCURSEL,0,0);
    m.color=int(s->magColor); m.feather=sliderValue(s->hMagFeather);
    m.width=sliderValue(s->hMagW); m.height=sliderValue(s->hMagH); m.zoom=sliderValue(s->hMagZoom); m.border=sliderValue(s->hMagBorder);
    WORD hk=(WORD)SendMessageW(s->hMagKey,HKM_GETHOTKEY,0,0);
    m.key=LOBYTE(hk); BYTE mods=HIBYTE(hk); m.modifiers=0;
    if(mods&HOTKEYF_CONTROL) m.modifiers|=MOD_CONTROL;
    if(mods&HOTKEYF_ALT) m.modifiers|=MOD_ALT;
    if(mods&HOTKEYF_SHIFT) m.modifiers|=MOD_SHIFT;
    m.normalize(); return m;
}
// load all controls from a MirrorConfig
static void controlsFromConfig(DlgState* s, const MirrorConfig& c){
    const auto& mons = *s->mons;
    int si = ResolveMonitor(mons, c.source);   // stable hwid first, device name fallback
    int ti = ResolveMonitor(mons, c.target);
    selectMonitorIdx(s->hSrcCombo, c.followMouse ? (int)mons.size() : si);
    selectMonitorIdx(s->hTgtCombo, ti);
    if (c.followMouse) si = ti>=0 ? FollowSource(mons, ti) : (mons.empty() ? -1 : 0);
    s->src.mon = si >= 0 ? &mons[si] : nullptr;
    s->tgt.mon = ti >= 0 ? &mons[ti] : nullptr;

    if (s->src.mon){
        if (c.source.w <= 0) s->src.region = centeredBox(*s->src.mon, 0.5);  // fresh: centred 50%
        else {
            RECT r = { c.source.x, c.source.y, c.source.x + c.source.w, c.source.y + c.source.h };
            clampInside(r, s->src.mon->w(), s->src.mon->h());
            s->src.region = r;
        }
    }
    // options (rotation set before computing the target's default aspect)
    loadMagnifier(s,c.magnifier);
    setInt(s->hFps, c.fps);
    SendMessageW(s->hVsync,  BM_SETCHECK, c.vsync  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hCursor, BM_SETCHECK, c.cursor ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hCover,  BM_SETCHECK, c.cover  ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hRot, CB_SETCURSEL, (WPARAM)(c.rotation / 90), 0);
    SendMessageW(s->hFlipH, BM_SETCHECK, c.flipH ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hFlipV, BM_SETCHECK, c.flipV ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hWindowed, BM_SETCHECK, c.windowed ? BST_CHECKED : BST_UNCHECKED, 0);

    SendMessageW(s->hLockCursor, BM_SETCHECK, c.lockCursor ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hGuardWindows, BM_SETCHECK, c.guardWindows ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hLowLatency, BM_SETCHECK, c.lowLatency ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(s->hLog, BM_SETCHECK, c.log ? BST_CHECKED : BST_UNCHECKED, 0);

    if (s->tgt.mon){
        if (c.target.w <= 0) s->tgt.region = centeredRatioBox(*s->tgt.mon, 0.5, dlgEffAspect(s));
        else {
            RECT r = { c.target.x, c.target.y, c.target.x + c.target.w, c.target.y + c.target.h };
            clampInside(r, s->tgt.mon->w(), s->tgt.mon->h());
            s->tgt.region = r;
        }
    }
    refitTarget(s);
    setTargetEnabled(s, s->src.mon != nullptr);
    if (s->src.mon) selectMonitorIdx(s->hTgtCombo, ti);   // re-apply after enable
    updateModeControls(s);
    writeSrcFields(s); writeTgtFields(s);
    InvalidateRect(s->src.hwnd, nullptr, FALSE);
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
}

static bool configFromControls(DlgState* s, MirrorConfig& c){
    int si = (int)SendMessageW(s->hSrcCombo, CB_GETCURSEL, 0, 0);
    int ti = (int)SendMessageW(s->hTgtCombo, CB_GETCURSEL, 0, 0);
    if (si < 0 || ti < 0) return false;
    const auto& mons = *s->mons;
    c.followMouse = si == (int)mons.size();
    if (ti >= (int)mons.size()) return false;
    if (c.followMouse) {
        if (mons.size() < 2) return false;
        si = FollowSource(mons, ti);
        if (si < 0) return false;
    }
    if (si >= (int)mons.size()) return false;
    c.source.device = mons[si].device; c.source.hwid = mons[si].hwid;
    c.target.device = mons[ti].device; c.target.hwid = mons[ti].hwid;
    c.source.x = s->src.region.left; c.source.y = s->src.region.top;
    c.source.w = s->src.region.right - s->src.region.left;
    c.source.h = s->src.region.bottom - s->src.region.top;
    c.target.x = s->tgt.region.left; c.target.y = s->tgt.region.top;
    c.target.w = s->tgt.region.right - s->tgt.region.left;
    c.target.h = s->tgt.region.bottom - s->tgt.region.top;
    c.magnifier=readMagnifier(s);
    c.fps = clampi(getInt(s->hFps), 0, 1000);
    c.vsync    = SendMessageW(s->hVsync,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.cursor   = SendMessageW(s->hCursor,   BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.cover    = SendMessageW(s->hCover,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.windowed = SendMessageW(s->hWindowed, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.flipH    = SendMessageW(s->hFlipH,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.flipV    = SendMessageW(s->hFlipV,    BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.lockCursor = SendMessageW(s->hLockCursor, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.guardWindows = SendMessageW(s->hGuardWindows, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.lowLatency = SendMessageW(s->hLowLatency, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.log      = SendMessageW(s->hLog,      BM_GETCHECK, 0, 0) == BST_CHECKED;

    c.rotation = dlgRotation(s);
    if (c.followMouse) {
        c.source.x = c.source.y = c.target.x = c.target.y = 0;
        c.source.w = mons[si].w(); c.source.h = mons[si].h();
        c.target.w = mons[ti].w(); c.target.h = mons[ti].h();
    }
    return true;
}

// Re-enumerate connected monitors (e.g. after plugging/unplugging a display) and
// repopulate the pickers, keeping the current choices/areas where possible.
static void refreshMonitors(DlgState* s){
    MirrorConfig cur; bool have = configFromControls(s, cur);   // snapshot current choices
    s->monsStore = EnumMonitors();
    SendMessageW(s->hSrcCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(s->hTgtCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& m : s->monsStore){
        SendMessageW(s->hSrcCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
        SendMessageW(s->hTgtCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
    }
    SendMessageW(s->hSrcCombo, CB_ADDSTRING, 0, (LPARAM)L"Follow Mouse (all except target)");
    if (have) {
        controlsFromConfig(s, cur);   // re-select by stable id/device, restore areas + options
    } else {
        s->src.mon = s->tgt.mon = nullptr;
        setTargetEnabled(s, false);
        updateModeControls(s);
        InvalidateRect(s->src.hwnd, nullptr, FALSE);
        InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
    }
}

// user typed in the source X/Y/W/H fields
static void srcFieldsToCanvas(DlgState* s){
    if (!s->src.mon) return;
    int x = getInt(s->hSrcX), y = getInt(s->hSrcY), w = getInt(s->hSrcW), h = getInt(s->hSrcH);
    RECT r = { x, y, x + std::max(8,w), y + std::max(8,h) };
    clampInside(r, s->src.mon->w(), s->src.mon->h());
    s->src.region = r;
    refitTarget(s);
    InvalidateRect(s->src.hwnd, nullptr, FALSE);
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
    writeTgtFields(s);
}
// user typed in the target X/Y/W fields (H is derived from the locked ratio)
static void tgtFieldsToCanvas(DlgState* s){
    if (!s->tgt.mon) return;
    int x = getInt(s->hTgtX), y = getInt(s->hTgtY), w = getInt(s->hTgtW);
    double asp = s->tgt.lockAspect > 0 ? s->tgt.lockAspect : 1.0;
    int h = (int)std::lround(std::max(8,w) / asp);
    RECT r = { x, y, x + std::max(8,w), y + std::max(8,h) };
    clampInside(r, s->tgt.mon->w(), s->tgt.mon->h());
    s->tgt.region = r;
    refitTarget(s);   // re-clamp exactly to ratio & bounds
    InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
    writeTgtFields(s);
}

// ---- import / export -------------------------------------------------------
static bool browseFile(HWND owner, const std::wstring& startDir, bool save, std::wstring& out){
    wchar_t file[MAX_PATH] = L"mirror.cfg";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Mirror config (*.cfg)\0*.cfg\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = startDir.c_str();
    ofn.lpstrDefExt = L"cfg";
    ofn.Flags = OFN_NOCHANGEDIR | (save ? (OFN_OVERWRITEPROMPT) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return false;
    out = file; return true;
}

// Read Windows app appearance and accent without changing system settings.
static COLORREF blend(COLORREF a, COLORREF b, int percent) {
    return RGB((GetRValue(a)*(100-percent)+GetRValue(b)*percent)/100,
               (GetGValue(a)*(100-percent)+GetGValue(b)*percent)/100,
               (GetBValue(a)*(100-percent)+GetBValue(b)*percent)/100);
}
static void readTheme() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    HIGHCONTRASTW hc = {sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    bool contrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    g_dark = !light && !contrast;
    CLR_BG = g_dark ? RGB(32,32,32) : RGB(242,244,248);
    CLR_CARD = g_dark ? RGB(43,43,43) : RGB(255,255,255);
    CLR_TEXT = g_dark ? RGB(245,245,245) : RGB(30,34,42);
    CLR_SUB = g_dark ? RGB(185,185,185) : RGB(112,118,130);
    CLR_CARD_LINE = blend(CLR_CARD, CLR_TEXT, 20);
    CLR_SHADOW = blend(CLR_BG, RGB(0,0,0), 8);
    DWORD accent = 0; BOOL opaque = FALSE;
    CLR_ACCENT = GetSysColor(COLOR_HIGHLIGHT);
    if (SUCCEEDED(DwmGetColorizationColor(&accent, &opaque)))
        CLR_ACCENT = RGB((accent>>16)&255, (accent>>8)&255, accent&255);
    if (contrast) {
        CLR_BG = CLR_CARD = CLR_SHADOW = GetSysColor(COLOR_WINDOW);
        CLR_TEXT = CLR_SUB = CLR_CARD_LINE = GetSysColor(COLOR_WINDOWTEXT);
        CLR_ACCENT = GetSysColor(COLOR_HIGHLIGHT);
    }
    CLR_ACCENT_HI = blend(CLR_ACCENT, RGB(255,255,255), 12);
    CLR_ACCENT_LO = blend(CLR_ACCENT, RGB(0,0,0), 15);
    int luminance = 299*GetRValue(CLR_ACCENT)+587*GetGValue(CLR_ACCENT)+114*GetBValue(CLR_ACCENT);
    CLR_HDRTX = contrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) :
        (luminance > 150000 ? RGB(0,0,0) : RGB(255,255,255));
    CLR_HDRSUB = CLR_HDRTX;
    CLR_BANNER_BG = contrast ? CLR_CARD : (g_dark ? RGB(65,52,24) : RGB(255,246,209));
    CLR_BANNER_LN = contrast ? CLR_TEXT : (g_dark ? RGB(150,115,45) : RGB(240,214,140));
    CLR_BANNER_TX = contrast ? CLR_TEXT : (g_dark ? RGB(255,221,145) : RGB(122,82,8));
    CLR_DANGER = contrast ? CLR_TEXT : (g_dark ? RGB(255,135,135) : RGB(197,58,58));
    CLR_DANGER_HI = blend(CLR_CARD, CLR_DANGER, 12);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_cardBrush) DeleteObject(g_cardBrush);
    g_bgBrush = CreateSolidBrush(CLR_BG);
    g_cardBrush = CreateSolidBrush(CLR_CARD);
}
static BOOL CALLBACK themeChild(HWND child, LPARAM) {
    wchar_t cls[32] = {};
    GetClassNameW(child, cls, 32);
    if (!lstrcmpiW(cls,L"EDIT") || !lstrcmpiW(cls,L"COMBOBOX") ||
        !lstrcmpiW(cls,L"BUTTON") || !lstrcmpiW(cls,TRACKBAR_CLASSW) ||
        !lstrcmpiW(cls,HOTKEY_CLASSW))
        SetWindowTheme(child, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    return TRUE;
}
static void applyTheme(HWND hwnd) {
    static bool applying = false;
    if (applying) return;
    applying = true;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    EnumChildWindows(hwnd, themeChild, 0);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN|RDW_FRAME);
    applying = false;
}

// ---- layout constants (shared by createControls and WM_PAINT) --------------
enum {
    LO_HDR_H = 60, LO_MARGIN = 16, LO_CARD_W = 448, LO_GAP = 12,
    LO_CARD_AX = LO_MARGIN,
    LO_CARD_BX = LO_MARGIN + LO_CARD_W + LO_GAP,
    LO_TAB_Y = LO_HDR_H + 8, LO_TAB_H = 40,
    LO_BANNER_Y = LO_HDR_H + 56, LO_BANNER_H = 44,
    LO_CARD_Y = LO_BANNER_Y + LO_BANNER_H + 12,
    LO_CARD_H = 312,
    LO_PAD = 16,
    LO_OPT_Y = LO_CARD_Y + LO_CARD_H + 16,
    LO_OPT_H = 138,                      // two option cards, each with a heading and three rows
    LO_BTN_Y = LO_OPT_Y + LO_OPT_H + 14
};

static HWND makeLabel(HWND p, HINSTANCE hi, LPCWSTR t, int x, int y, int w, int h, HFONT f = nullptr){
    // SS_CENTERIMAGE centres text vertically so descenders (g, p, y) are never clipped.
    HWND c = CreateWindowExW(0, L"STATIC", t, WS_CHILD|WS_VISIBLE|SS_LEFT|SS_CENTERIMAGE|SS_NOPREFIX,
                             x,y,w,h, p, nullptr, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g_font), TRUE); return c;
}
static HWND makeEdit(HWND p, HINSTANCE hi, int id, int x, int y, int w, int h){
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_NUMBER|ES_RIGHT,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}
static HWND makeButton(HWND p, HINSTANCE hi, int id, LPCWSTR t, int x, int y, int w, int h){
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(g_btnFont ? g_btnFont : g_font), TRUE); return c;
}
static HWND makeCheck(HWND p, HINSTANCE hi, int id, LPCWSTR t, int x, int y, int w, int h){
    HWND c = CreateWindowExW(0, L"BUTTON", t, WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                             x,y,w,h, p, (HMENU)(INT_PTR)id, hi, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return c;
}

// ---- option tooltips -------------------------------------------------------
// Native common controls handle hover timing and mouse tracking locally on
// the setup UI thread. Text literals remain valid for the tool's lifetime.
static void createOptionTooltips(HWND hwnd, DlgState* s, HINSTANCE hi) {
    s->tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, hi, nullptr);
    if (!s->tooltips) return;  // help is optional; setup still works
    SetWindowPos(s->tooltips, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    SendMessageW(s->tooltips, TTM_SETMAXTIPWIDTH, 0, 400);
    SendMessageW(s->tooltips, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    SendMessageW(s->tooltips, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    SendMessageW(s->tooltips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 15000);

    auto add = [&](HWND control, LPCWSTR text) {
        if (!control) return;
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND|TTF_SUBCLASS;
        tool.hwnd = hwnd;
        tool.uId = (UINT_PTR)control;
        tool.lpszText = const_cast<LPWSTR>(text);
        SendMessageW(s->tooltips, TTM_ADDTOOLW, 0, (LPARAM)&tool);
    };
    add(s->hFps, L"Limit mirror presentations per second. 0 means no manual cap. Ignored in Follow Mouse and Low latency modes.");
    add(s->hVsync, L"Synchronize presentation with the target display's refresh to reduce tearing. Turning this off may reduce latency but can cause tearing.");
    add(s->hCursor, L"Include the captured mouse pointer in the mirrored image. Does not change the actual mouse pointer or its movement.");
    add(s->hCover, L"Fill the unused area of the target monitor with black instead of showing its desktop. Always enabled in Follow Mouse; floating windows are letterboxed.");
    add(s->hLowLatency, L"Skip unchanged frames and bypass the manual FPS cap, with a short wait for a fresh frame when idle. May reduce redundant rendering or latency; the improvement depends on your setup. V-Sync remains independent.");
    add(s->hRot, L"Rotate the mirrored image clockwise. Does not rotate the Windows desktop. Fixed-source mode only.");
    add(s->hFlipH, L"Flip the mirrored image left to right. Fixed-source mode only.");
    add(s->hFlipV, L"Flip the mirrored image top to bottom. Fixed-source mode only.");
    add(s->hWindowed, L"Use a movable, resizable mirror window instead of a dedicated mirror display. Cursor confinement and window protection are inactive in this mode. Fixed-source mode only.");
    add(s->hLog, L"Write diagnostic information to mirror.log next to mirror.exe. Useful for troubleshooting; leave off for normal use.");
    add(s->hLockCursor, L"Keep the actual mouse pointer off the dedicated mirror monitor. Temporarily releases confinement during setup and diagnostics; inactive when paused or floating.");
    add(s->hMagEnable,L"Enable a local lens on the mirrored image only. The working monitor and actual pointer are unchanged.");
    add(s->hMagMode,L"Toggle with a hotkey, show while holding it, or always show the lens while the pointer is in the captured area.");
    add(s->hMagShape,L"Choose ellipse, rectangle, or rounded rectangle. Equal width and height produce a circle or square.");
    add(s->hMagColor,L"Open the RGB color wheel to choose the outline color. Ignored when border width is zero.");
    add(s->hMagW,L"Lens width in target image pixels, 64 to 4096. Automatically limited to the image size. Arrow keys adjust one pixel at a time.");
    add(s->hMagH,L"Lens height in target image pixels, 64 to 4096. Automatically limited to the image size. Arrow keys adjust one pixel at a time.");
    add(s->hMagZoom,L"Magnification relative to the normal mirror, 125 to 800 percent. The original capture texture is sampled. Arrow keys adjust one percent at a time.");
    add(s->hMagEdge,L"Clip the lens at the image edge or shift it inside. The source sample stays centered on the actual cursor; near edges the outermost source pixels are repeated.");
    add(s->hMagBorder,L"Border thickness in target pixels, 0 to 20. Zero removes the border.");
    add(s->hMagFeather,L"Blend the outer lens edge into the normal image. Zero keeps a crisp edge.");
    add(s->hMagSharp,L"Use nearest-neighbor sampling inside the lens instead of smooth linear interpolation.");
    add(s->hMagKey,L"Choose a shortcut with Ctrl, Alt or Shift, or a function key F1-F11. F12 is reserved by Windows. Empty or invalid shortcuts revert to Alt+Shift+M.");
    add(s->hGuardWindows, L"Move existing and newly opened application windows off the dedicated mirror monitor, preferably to the monitor under the cursor. Some protected windows may resist relocation. Inactive when paused or floating.");
}

// ---- GDI paint helpers -----------------------------------------------------
static void fillRound(HDC dc, RECT r, int rad, COLORREF fill){
    HBRUSH b = CreateSolidBrush(fill); HPEN pn = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pn);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pn);
}
static void strokeRound(HDC dc, RECT r, int rad, COLORREF line){
    HPEN pn = CreatePen(PS_SOLID, 1, line); HGDIOBJ op = SelectObject(dc, pn);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pn);
}
static void drawCard(HDC dc, int x, int y, int w, int h){
    RECT sh = { x, y + 2, x + w + 2, y + h + 2 }; fillRound(dc, sh, 14, CLR_SHADOW);  // soft shadow
    RECT c  = { x, y, x + w, y + h };            fillRound(dc, c, 14, CLR_CARD);
    strokeRound(dc, c, 14, CLR_CARD_LINE);
}

// hover tracking for owner-drawn buttons
static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref){
    DlgState* s = (DlgState*)ref;
    if (m == WM_MOUSEMOVE){
        if (s && s->hover != h){
            s->hover = h; InvalidateRect(h, nullptr, TRUE);
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        }
    } else if (m == WM_MOUSELEAVE){
        if (s && s->hover == h){ s->hover = nullptr; InvalidateRect(h, nullptr, TRUE); }
    }
    return DefSubclassProc(h, m, w, l);
}
static void drawButton(DRAWITEMSTRUCT* di, DlgState* s){
    HDC dc = di->hDC; RECT r = di->rcItem;
    bool down = (di->itemState & ODS_SELECTED) != 0;
    bool hot  = s && s->hover == di->hwndItem;
    bool primary = (int)di->CtlID == ID_SAVE, danger = (int)di->CtlID == ID_QUIT;
    COLORREF fill, line, tx;
    if (primary){ fill = down ? CLR_ACCENT_LO : (hot ? CLR_ACCENT_HI : CLR_ACCENT); line = fill; tx = CLR_HDRTX; }
    else if (danger){ fill = down ? blend(CLR_CARD, CLR_DANGER, 22) : (hot ? CLR_DANGER_HI : CLR_CARD); line = CLR_DANGER; tx = CLR_DANGER; }
    else { fill = down ? blend(CLR_CARD, CLR_TEXT, 15) : (hot ? blend(CLR_CARD, CLR_TEXT, 7) : CLR_CARD); line = CLR_CARD_LINE; tx = CLR_TEXT; }

    HBRUSH bg = CreateSolidBrush(CLR_BG); FillRect(dc, &r, bg); DeleteObject(bg);   // clear rounded corners
    fillRound(dc, r, 9, fill); strokeRound(dc, r, 9, line);

    wchar_t txt[80] = {0}; GetWindowTextW(di->hwndItem, txt, 80);
    if (s && di->CtlID==ID_MAG_COLOR) {
        RECT sw={r.left+12,r.top+5,r.left+32,r.bottom-5};
        fillRound(dc,sw,20,s->magColor); strokeRound(dc,sw,20,CLR_CARD_LINE);
        swprintf(txt,80,L"#%02X%02X%02X  ·  Choose…",unsigned(GetRValue(s->magColor)),unsigned(GetGValue(s->magColor)),unsigned(GetBValue(s->magColor)));
        r.left+=36;
    }
    if(di->itemState & ODS_DISABLED) tx=CLR_SUB;
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, tx);
    HGDIOBJ of = SelectObject(dc, g_btnFont ? g_btnFont : g_font);
    DrawTextW(dc, txt, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc, of);
    if (di->itemState & ODS_FOCUS) { InflateRect(&r,-4,-4); DrawFocusRect(dc,&r); }
}

static void drawTab(DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool active = (int)di->itemID == (int)SendMessageW(di->hwndItem, TCM_GETCURSEL, 0, 0);
    FillRect(dc, &r, g_bgBrush);
    InflateRect(&r, -2, -2);
    fillRound(dc, r, 8, active ? CLR_ACCENT : CLR_CARD);
    strokeRound(dc, r, 8, active ? CLR_ACCENT : CLR_CARD_LINE);
    wchar_t text[64] = {};
    TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=text; item.cchTextMax=64;
    SendMessageW(di->hwndItem, TCM_GETITEMW, di->itemID, (LPARAM)&item);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, active ? CLR_HDRTX : CLR_TEXT);
    HGDIOBJ old = SelectObject(dc, g_btnFont);
    DrawTextW(dc, text, -1, &r, DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(dc, old);
    if (active && GetFocus() == di->hwndItem) {
        InflateRect(&r,-4,-4);
        DrawFocusRect(dc,&r);
    }
}

static void createControls(HWND hwnd, DlgState* s, HINSTANCE hi, const std::wstring& banner){
    const auto& mons = *s->mons;
    s->banner = banner;

    // Create tabs first so keyboard traversal starts with page navigation.
    // Owner-drawn items use the same theme colors as the rest of the dialog.
    s->hTab=CreateWindowExW(0,WC_TABCONTROLW,L"",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|TCS_BUTTONS|TCS_FIXEDWIDTH|TCS_OWNERDRAWFIXED,
        LO_MARGIN,LO_TAB_Y,384,LO_TAB_H,hwnd,(HMENU)(INT_PTR)ID_TAB,hi,nullptr);
    SendMessageW(s->hTab,WM_SETFONT,(WPARAM)g_btnFont,TRUE);
    SendMessageW(s->hTab,TCM_SETITEMSIZE,0,MAKELPARAM(180,34));
    TCITEMW tab{}; tab.mask=TCIF_TEXT;
    tab.pszText=const_cast<LPWSTR>(L"Mirroring"); SendMessageW(s->hTab,TCM_INSERTITEMW,0,(LPARAM)&tab);
    tab.pszText=const_cast<LPWSTR>(L"Magnifier"); SendMessageW(s->hTab,TCM_INSERTITEMW,1,(LPARAM)&tab);
    SendMessageW(s->hTab,TCM_SETCURSEL,0,0);

    // Card A (source) / Card B (target) inner geometry
    const int ax = LO_CARD_AX + LO_PAD, bx = LO_CARD_BX + LO_PAD;
    const int innerW = LO_CARD_W - LO_PAD * 2;                 // usable width inside a card
    const int canvasW = 200, gap = 32;                         // wide gap so fields never crowd the box
    const int fw = innerW - canvasW - gap;                     // numeric-field column width
    const int afx = ax + canvasW + gap, bfx = bx + canvasW + gap;
    const int y0 = LO_CARD_Y;                                  // card top

    auto column = [&](int cardX, int cx, int fx, int hdrNum, LPCWSTR title, LPCWSTR hint,
                      HWND& combo, int comboId, Canvas& cv,
                      HWND& eX, HWND& eY, HWND& eW, HWND& eH, int idX, int idY, int idW, int idH,
                      bool tgt){
        makeLabel(hwnd, hi, title, cx, y0 + 14, LO_CARD_W - LO_PAD*2, 24, g_hdrFont);
        makeLabel(hwnd, hi, hint,  cx, y0 + 40, LO_CARD_W - LO_PAD*2, 20, g_subFont);
        combo = CreateWindowExW(0, L"COMBOBOX", L"",
            WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL, cx, y0 + 64, innerW, 300,
            hwnd, (HMENU)(INT_PTR)comboId, hi, nullptr);
        SendMessageW(combo, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(combo, CB_SETDROPPEDWIDTH, 640, 0);

        int cvY = y0 + 100, cvH = 196;
        cv.hwnd = CreateWindowExW(0, L"MirrorCfgCanvas", L"", WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                                  cx, cvY, canvasW, cvH, hwnd, nullptr, hi, nullptr);
        cv.parent = hwnd; cv.which = tgt ? 1 : 0; cv.ratioLocked = tgt;
        SetWindowLongPtrW(cv.hwnd, GWLP_USERDATA, (LONG_PTR)&cv);

        // each row: label, then its edit 21px lower (clear gap), rows 49px apart
        int r0 = cvY, rh = 49, lo = 22;
        makeLabel(hwnd, hi, L"X  (px)", fx, r0,        fw, 20); eX = makeEdit(hwnd, hi, idX, fx, r0+lo,        fw, 24);
        makeLabel(hwnd, hi, L"Y  (px)", fx, r0+rh,     fw, 20); eY = makeEdit(hwnd, hi, idY, fx, r0+rh+lo,     fw, 24);
        makeLabel(hwnd, hi, L"Width",   fx, r0+rh*2,   fw, 20); eW = makeEdit(hwnd, hi, idW, fx, r0+rh*2+lo,   fw, 24);
        makeLabel(hwnd, hi, tgt ? L"Height (auto)" : L"Height",
                  fx, r0+rh*3,   fw, 20); eH = makeEdit(hwnd, hi, idH, fx, r0+rh*3+lo,   fw, 24);
    };

    column(LO_CARD_AX, ax, afx, 1,
           L"1  ·  Main working monitor",
           L"Where your drawing app runs — drag the box or type the area.",
           s->hSrcCombo, ID_SRC_COMBO, s->src,
           s->hSrcX, s->hSrcY, s->hSrcW, s->hSrcH, ID_SRC_X, ID_SRC_Y, ID_SRC_W, ID_SRC_H, false);
    column(LO_CARD_BX, bx, bfx, 2,
           L"2  ·  Mirror onto monitor",
           L"Locked to the same shape — drag to place, drag a corner to resize.",
           s->hTgtCombo, ID_TGT_COMBO, s->tgt,
           s->hTgtX, s->hTgtY, s->hTgtW, s->hTgtH, ID_TGT_X, ID_TGT_Y, ID_TGT_W, ID_TGT_H, true);

    EnableWindow(s->hTgtH, FALSE);   // derived from the locked ratio

    for (const auto& m : mons){
        SendMessageW(s->hSrcCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
        SendMessageW(s->hTgtCombo, CB_ADDSTRING, 0, (LPARAM)m.label().c_str());
    }

    SendMessageW(s->hSrcCombo, CB_ADDSTRING, 0, (LPARAM)L"Follow Mouse (all except target)");

    // ---- two option cards: image/performance and desktop behavior ----
    const int ox=ax, dx=bx, oy=LO_OPT_Y;
    makeLabel(hwnd,hi,L"Image & performance",ox,oy+10,innerW,24,g_hdrFont);
    makeLabel(hwnd,hi,L"Desktop & window behavior",dx,oy+10,innerW,24,g_hdrFont);
    makeLabel(hwnd,hi,L"FPS limit",ox,oy+40,76,24);
    s->hFps=makeEdit(hwnd,hi,ID_FPS,ox+78,oy+40,56,24);
    makeLabel(hwnd,hi,L"0 = uncapped",ox+140,oy+40,106,24,g_subFont);
    s->hVsync=makeCheck(hwnd,hi,ID_VSYNC,L"V-Sync",ox+264,oy+40,140,24);
    makeLabel(hwnd,hi,L"Rotation",ox,oy+72,66,24);
    s->hRot=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,
        ox+68,oy+72,84,160,hwnd,(HMENU)(INT_PTR)ID_ROT,hi,nullptr);
    SendMessageW(s->hRot,WM_SETFONT,(WPARAM)g_font,TRUE);
    for (LPCWSTR r : {L"0°",L"90°",L"180°",L"270°"}) SendMessageW(s->hRot,CB_ADDSTRING,0,(LPARAM)r);
    SendMessageW(s->hRot,CB_SETCURSEL,0,0);
    s->hFlipH=makeCheck(hwnd,hi,ID_FLIPH,L"Flip X",ox+164,oy+72,82,24);
    s->hFlipV=makeCheck(hwnd,hi,ID_FLIPV,L"Flip Y",ox+264,oy+72,140,24);
    s->hCursor=makeCheck(hwnd,hi,ID_CURSOR,L"Show cursor",ox,oy+104,124,24);
    s->hCover=makeCheck(hwnd,hi,ID_COVER,L"Black background",ox+126,oy+104,156,24);
    s->hLowLatency=makeCheck(hwnd,hi,ID_LOW_LATENCY,L"Low latency",ox+290,oy+104,126,24);
    s->hWindowed=makeCheck(hwnd,hi,ID_WINDOWED,L"Floating window",dx,oy+40,196,24);
    s->hLog=makeCheck(hwnd,hi,ID_LOG,L"Log to file",dx+210,oy+40,190,24);
    s->hLockCursor=makeCheck(hwnd,hi,ID_LOCK_CURSOR,L"Lock cursor to active screens",dx,oy+72,innerW,24);
    s->hGuardWindows=makeCheck(hwnd,hi,ID_GUARD_WINDOWS,L"Keep windows off mirror monitor",dx,oy+104,innerW,24);

    // The pages share the parent and Save/Cancel buttons; switching pages
    // only hides controls, so unsaved changes on either page are preserved.
    for(HWND h=GetWindow(hwnd,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT))
        if(h!=s->hTab) s->mirrorPage.push_back(h);
    auto track=[&](HWND h){
        s->magPage.push_back(h);
        return h;
    };
    auto label=[&](LPCWSTR text,int x,int y,int w,HFONT font=nullptr){return track(makeLabel(hwnd,hi,text,x,y,w,24,font));};
    auto combo=[&](int id,int x,int y,int w,std::initializer_list<LPCWSTR> items){
        HWND h=track(CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
            x,y,w,180,hwnd,(HMENU)(INT_PTR)id,hi,nullptr));
        SendMessageW(h,WM_SETFONT,(WPARAM)g_font,TRUE);
        for(auto item:items) SendMessageW(h,CB_ADDSTRING,0,(LPARAM)item);
        return h;
    };
    // Magnifier: activation/appearance on the left; numeric sliders on the right.
    const int mx=ax, rx=bx, my=LO_CARD_Y;
    label(L"Magnifying glass",mx,my+14,innerW,g_hdrFont);
    label(L"Enlarge details on the mirrored output only.",mx,my+40,innerW,g_subFont);
    s->hMagEnable=track(makeCheck(hwnd,hi,ID_MAG_ENABLE,L"Enable magnifier",mx,my+76,innerW,24));
    label(L"Activation",mx,my+116,176);
    s->hMagMode=combo(ID_MAG_MODE,mx+180,my+116,236,{L"Toggle hotkey",L"Hold hotkey",L"Always on"});
    label(L"Keyboard shortcut",mx,my+154,176);
    s->hMagKey=track(CreateWindowExW(WS_EX_CLIENTEDGE,HOTKEY_CLASSW,L"",WS_CHILD|WS_TABSTOP,
        mx+180,my+154,236,25,hwnd,(HMENU)(INT_PTR)ID_MAG_KEY,hi,nullptr));
    SendMessageW(s->hMagKey,WM_SETFONT,(WPARAM)g_font,TRUE);
    label(L"Appearance",mx,my+207,innerW,g_hdrFont);
    label(L"Lens shape",mx,my+244,176);
    s->hMagShape=combo(ID_MAG_SHAPE,mx+180,my+244,236,{L"Ellipse / circle",L"Rectangle / square",L"Rounded rectangle"});
    label(L"Image edge behavior",mx,my+282,176);
    s->hMagEdge=combo(ID_MAG_EDGE,mx+180,my+282,236,{L"Clip at image edge",L"Keep lens inside image"});
    label(L"Border color",mx,my+320,176);
    s->hMagColor=track(makeButton(hwnd,hi,ID_MAG_COLOR,L"Choose color…",mx+180,my+320,236,30));
    SetWindowSubclass(s->hMagColor,BtnSub,1,(DWORD_PTR)s);
    s->hMagSharp=track(makeCheck(hwnd,hi,ID_MAG_SHARP,L"Sharp pixels (nearest neighbor)",mx,my+362,innerW,24));
    label(L"Equal width and height make a circle or square.",mx,my+405,innerW,g_subFont);

    label(L"Lens size & zoom",rx,my+14,innerW,g_hdrFont);
    label(L"Drag a slider or type an exact value.",rx,my+40,innerW,g_subFont);
    auto slider=[&](int id,int editId,LPCWSTR name,LPCWSTR range,LPCWSTR unit,int y,int lo,int hiValue,int page,HWND& display){
        label(name,rx,y,innerW-116);
        display=track(makeEdit(hwnd,hi,editId,rx+innerW-108,y,72,24));
        SendMessageW(display,EM_SETLIMITTEXT,4,0);
        label(unit,rx+innerW-30,y,30,g_subFont);
        HWND h=track(CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,
            rx,y+25,innerW,30,hwnd,(HMENU)(INT_PTR)id,hi,nullptr));
        SendMessageW(h,TBM_SETRANGE,TRUE,MAKELPARAM(lo,hiValue));
        SendMessageW(h,TBM_SETLINESIZE,0,1);
        SendMessageW(h,TBM_SETPAGESIZE,0,page);
        label(range,rx,y+55,innerW,g_subFont);
        return h;
    };
    s->hMagW=slider(ID_MAG_W,ID_MAG_W_EDIT,L"Lens width",L"64–4096 target pixels",L"px",my+75,64,4096,64,s->hMagWValue);
    s->hMagH=slider(ID_MAG_H,ID_MAG_H_EDIT,L"Lens height",L"64–4096 target pixels",L"px",my+148,64,4096,64,s->hMagHValue);
    s->hMagZoom=slider(ID_MAG_ZOOM,ID_MAG_ZOOM_EDIT,L"Magnification",L"125–800% of normal mirror scale",L"%",my+221,125,800,25,s->hMagZoomValue);
    s->hMagBorder=slider(ID_MAG_BORDER,ID_MAG_BORDER_EDIT,L"Border thickness",L"0–20 pixels · 0 hides the outline",L"px",my+294,0,20,2,s->hMagBorderValue);
    s->hMagFeather=slider(ID_MAG_FEATHER,ID_MAG_FEATHER_EDIT,L"Edge blur",L"0–64 pixels · 0 keeps a crisp edge",L"px",my+367,0,64,4,s->hMagFeatherValue);
    for(HWND h:s->magPage) ShowWindow(h,SW_HIDE);
    createOptionTooltips(hwnd, s, hi);

    // ---- buttons ----
    int by = LO_BTN_Y, right = LO_CARD_BX + LO_CARD_W;
    makeButton(hwnd, hi, ID_SAVE,    L"Save && Start",    LO_CARD_AX,       by, 150, 40);
    makeButton(hwnd, hi, ID_IMPORT,  L"Import…",          LO_CARD_AX + 162, by, 100, 40);
    makeButton(hwnd, hi, ID_EXPORT,  L"Export…",          LO_CARD_AX + 270, by, 100, 40);
    makeButton(hwnd, hi, ID_REFRESH, L"↻ Refresh monitors", LO_CARD_AX + 378, by, 184, 40);
    makeButton(hwnd, hi, ID_IDENTIFY,L"Identify monitors", LO_CARD_AX + 570, by, 176, 40);
    makeButton(hwnd, hi, ID_QUIT,    L"Cancel && Quit",   right - 150,      by, 150, 40);

    // subclass the owner-drawn buttons for hover feedback
    for (int id : { ID_SAVE, ID_IMPORT, ID_EXPORT, ID_REFRESH, ID_IDENTIFY, ID_QUIT })
        SetWindowSubclass(GetDlgItem(hwnd, id), BtnSub, 1, (DWORD_PTR)s);
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    DlgState* s = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg){
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        readTheme(); applyTheme(hwnd); return 0;
    case WM_ERASEBKGND: return 1;   // painted fully in WM_PAINT (no flicker)
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(CLR_BG); FillRect(mem, &cr, bg); DeleteObject(bg);

        // header band
        RECT hb = { 0, 0, cr.right, LO_HDR_H };
        HBRUSH ab = CreateSolidBrush(CLR_ACCENT); FillRect(mem, &hb, ab); DeleteObject(ab);
        SetBkMode(mem, TRANSPARENT);
        SelectObject(mem, g_titleFont); SetTextColor(mem, CLR_HDRTX);
        RECT tr = { LO_MARGIN + 6, 9, cr.right - 16, 41 };
        DrawTextW(mem, L"Display Mirror", -1, &tr, DT_LEFT|DT_SINGLELINE);
        SelectObject(mem, g_subFont); SetTextColor(mem, CLR_HDRSUB);
        RECT sr = { LO_MARGIN + 6, 37, cr.right - 16, 56 };
        DrawTextW(mem, L"Choose what to mirror and where it appears.", -1, &sr, DT_LEFT|DT_SINGLELINE);

        // Full-width divider makes the tab strip distinct from page contents.
        RECT tabLine={LO_MARGIN,LO_TAB_Y+LO_TAB_H+2,cr.right-LO_MARGIN,LO_TAB_Y+LO_TAB_H+3};
        HBRUSH line=CreateSolidBrush(CLR_CARD_LINE); FillRect(mem,&tabLine,line); DeleteObject(line);

        // info / error banner
        if (s && !s->banner.empty()){
            RECT br = { LO_MARGIN, LO_BANNER_Y, cr.right - LO_MARGIN, LO_BANNER_Y + LO_BANNER_H };
            fillRound(mem, br, 10, CLR_BANNER_BG); strokeRound(mem, br, 10, CLR_BANNER_LN);
            SelectObject(mem, g_font); SetTextColor(mem, CLR_BANNER_TX);
            RECT bt = br; bt.left += 14; bt.right -= 14; bt.top += 5;
            DrawTextW(mem, s->banner.c_str(), -1, &bt, DT_LEFT|DT_WORDBREAK);
        } else {
            SelectObject(mem,g_font); SetTextColor(mem,CLR_SUB);
            RECT hint={LO_MARGIN+4,LO_BANNER_Y,cr.right-LO_MARGIN,LO_BANNER_Y+LO_BANNER_H};
            DrawTextW(mem,s && s->page==1 ? L"Customize a cursor-following lens. Apply changes when ready." :
                L"Choose your source and destination, then adjust image and desktop behavior.",
                -1,&hint,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        }

        // cards
        if(s && s->page==1) {
            const int h=LO_OPT_Y+LO_OPT_H-LO_CARD_Y;
            drawCard(mem,LO_CARD_AX,LO_CARD_Y,LO_CARD_W,h);
            drawCard(mem,LO_CARD_BX,LO_CARD_Y,LO_CARD_W,h);
            RECT divider={LO_CARD_AX+LO_PAD,LO_CARD_Y+194,LO_CARD_AX+LO_CARD_W-LO_PAD,LO_CARD_Y+195};
            HBRUSH b=CreateSolidBrush(CLR_CARD_LINE); FillRect(mem,&divider,b); DeleteObject(b);
        } else {
            drawCard(mem, LO_CARD_AX, LO_CARD_Y, LO_CARD_W, LO_CARD_H);
            drawCard(mem, LO_CARD_BX, LO_CARD_Y, LO_CARD_W, LO_CARD_H);
            drawCard(mem, LO_CARD_AX, LO_OPT_Y, LO_CARD_W, LO_OPT_H);
            drawCard(mem, LO_CARD_BX, LO_OPT_Y, LO_CARD_W, LO_OPT_H);
        }

        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON){ drawButton(di, s); return TRUE; }
        if (di->CtlType == ODT_TAB && di->CtlID == ID_TAB){ drawTab(di); return TRUE; }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE); SetTextColor(dc, IsWindowEnabled((HWND)lp) ? CLR_TEXT : CLR_SUB); SetBkColor(dc, CLR_CARD);
        return (LRESULT)g_cardBrush;   // controls sit on white cards
    }
    case WM_CANVAS_CHANGED:
        if (s){
            if (wp == 0){ writeSrcFields(s); refitTarget(s); writeTgtFields(s);
                          InvalidateRect(s->tgt.hwnd, nullptr, FALSE); }
            else        { writeTgtFields(s); }
        }
        return 0;
    case WM_HSCROLL:
        if (s && lp && ((HWND)lp==s->hMagW || (HWND)lp==s->hMagH ||
                        (HWND)lp==s->hMagZoom || (HWND)lp==s->hMagBorder || (HWND)lp==s->hMagFeather)) {
            updateMagnifierValues(s);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if(s && ((NMHDR*)lp)->hwndFrom==s->hTab && ((NMHDR*)lp)->code==TCN_SELCHANGE) {
            if(s->page==1) commitMagnifierValues(s);
            s->page=(int)SendMessageW(s->hTab,TCM_GETCURSEL,0,0);
            for(HWND h:s->mirrorPage) ShowWindow(h,s->page==0?SW_SHOWNA:SW_HIDE);
            for(HWND h:s->magPage) ShowWindow(h,s->page==1?SW_SHOWNA:SW_HIDE);
            InvalidateRect(s->hTab,nullptr,TRUE);
            InvalidateRect(hwnd,nullptr,FALSE); return 0;
        }
        break;
    case WM_COMMAND: {
        if (!s) return 0;
        int id = LOWORD(wp), code = HIWORD(wp);
        if (handleMagnifierEdit(s,id,code)) return 0;
        if (id==ID_MAG_COLOR && code==BN_CLICKED) { if(ColorPicker::Choose(hwnd,s->magColor)) InvalidateRect(s->hMagColor,nullptr,TRUE); return 0; }
        if ((id==ID_MAG_ENABLE && code==BN_CLICKED) || (id==ID_MAG_MODE && code==CBN_SELCHANGE)) {updateMagnifierControls(s); return 0;}
        if (code == CBN_SELCHANGE && (id == ID_SRC_COMBO || id == ID_TGT_COMBO)){
            const auto& mons = *s->mons;
            int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
            if (id == ID_SRC_COMBO){
                bool follow = sel == (int)mons.size();
                int target = (int)SendMessageW(s->hTgtCombo,CB_GETCURSEL,0,0);
                int preview = follow ? (target>=0 ? FollowSource(mons,target) : (mons.empty()?-1:0)) : sel;
                s->src.mon = (preview >= 0 && preview < (int)mons.size()) ? &mons[preview] : nullptr;
                if (s->src.mon)
                    s->src.region = centeredBox(*s->src.mon, follow ? 1.0 : 0.5);   // full source in Follow Mouse
                // the mirror monitor becomes selectable now that we know the ratio
                setTargetEnabled(s, s->src.mon != nullptr);
                refitTarget(s);
                writeSrcFields(s); writeTgtFields(s);
                InvalidateRect(s->src.hwnd, nullptr, FALSE);
                InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            } else {                                                 // target
                s->tgt.mon = (sel >= 0 && sel < (int)mons.size()) ? &mons[sel] : nullptr;
                if ((int)SendMessageW(s->hSrcCombo,CB_GETCURSEL,0,0)==(int)mons.size() && sel>=0) {
                    int preview=FollowSource(mons,sel);
                    s->src.mon=preview>=0?&mons[preview]:nullptr;
                }
                if (s->tgt.mon){
                    double asp = dlgEffAspect(s);
                    s->tgt.lockAspect = asp;
                    s->tgt.region = centeredRatioBox(*s->tgt.mon, 0.5, asp);  // centred, 50%, locked ratio
                }
                writeTgtFields(s);
                InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            }
            updateModeControls(s);
            return 0;
        }
        if (code == CBN_SELCHANGE && id == ID_ROT){   // rotation changes the target's shape
            refitTarget(s); writeTgtFields(s);
            InvalidateRect(s->tgt.hwnd, nullptr, FALSE);
            return 0;
        }
        if (code == EN_CHANGE && !s->syncing){
            if (id==ID_SRC_X||id==ID_SRC_Y||id==ID_SRC_W||id==ID_SRC_H){ srcFieldsToCanvas(s); writeSrcFields(s); }
            else if (id==ID_TGT_X||id==ID_TGT_Y||id==ID_TGT_W)         { tgtFieldsToCanvas(s); }
            return 0;
        }
        if (code == EN_KILLFOCUS){   // field lost focus: show the actual (clamped) value
            if (id==ID_SRC_X||id==ID_SRC_Y||id==ID_SRC_W||id==ID_SRC_H) writeSrcFields(s);
            else if (id==ID_TGT_X||id==ID_TGT_Y||id==ID_TGT_W)          writeTgtFields(s);
            return 0;
        }
        if (code == BN_CLICKED){
            if (id == ID_LOW_LATENCY || id == ID_WINDOWED) { updateModeControls(s); return 0; }
            if (id == ID_REFRESH){ hideIdentify(hwnd, s); refreshMonitors(s); return 0; }
            if (id == ID_IDENTIFY){ showIdentify(hwnd, s); return 0; }
            if (id == ID_QUIT){ s->result = ConfigResult::Quit; s->done = true; DestroyWindow(hwnd); return 0; }
            if (id == ID_SAVE){
                MirrorConfig c;
                if (!configFromControls(s, c)){
                    MessageBoxW(hwnd, L"Please choose both a working monitor and a mirror monitor.",
                                L"Setup", MB_ICONWARNING); return 0; }
                std::wstring err = ValidateConfig(c, *s->mons);
                if (!err.empty()){ MessageBoxW(hwnd, err.c_str(), L"Setup", MB_ICONWARNING); return 0; }
                *s->out = c; s->result = ConfigResult::Save; s->done = true; DestroyWindow(hwnd); return 0;
            }
            if (id == ID_IMPORT){
                std::wstring path;
                if (browseFile(hwnd, ExeDir(), false, path)){
                    MirrorConfig c;
                    if (LoadConfig(c, path)){ controlsFromConfig(s, c);
                        MessageBoxW(hwnd, L"Configuration imported. Review it, then click Save && Start.",
                                    L"Import", MB_ICONINFORMATION); }
                    else MessageBoxW(hwnd, L"That file is not a valid mirror configuration.",
                                     L"Import", MB_ICONERROR);
                }
                return 0;
            }
            if (id == ID_EXPORT){
                MirrorConfig c;
                if (!configFromControls(s, c)){
                    MessageBoxW(hwnd, L"Choose both monitors before exporting.", L"Export", MB_ICONWARNING); return 0; }
                std::wstring path;
                if (browseFile(hwnd, ExeDir(), true, path)){
                    if (SaveConfig(c, path)) MessageBoxW(hwnd, L"Configuration exported.", L"Export", MB_ICONINFORMATION);
                    else MessageBoxW(hwnd, L"Could not write that file.", L"Export", MB_ICONERROR);
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_TIMER:
        if (s && wp == IDENTIFY_TIMER) { hideIdentify(hwnd, s); return 0; }
        break;
    case WM_DISPLAYCHANGE:
        if (s) hideIdentify(hwnd, s);
        break;
    case WM_CLOSE:
        if (s){ s->result = ConfigResult::Quit; s->done = true; }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (s) {
            if (s->tooltips) { DestroyWindow(s->tooltips); s->tooltips = nullptr; }
            hideIdentify(hwnd, s);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

ConfigResult RunConfigDialog(MirrorConfig& cfg,
                             const std::vector<MonitorInfo>& mons,
                             HINSTANCE hInst,
                             const std::wstring& banner,
                             const std::wstring& defaultPath, bool live){
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_HOTKEY_CLASS | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);   // opt into themed (modern) common controls
    }
    auto mkFont = [](int px, int weight){
        return CreateFontW(-px, 0,0,0, weight, 0,0,0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
    };
    if (!g_font)      g_font      = mkFont(15, FW_NORMAL);
    if (!g_subFont)   g_subFont   = mkFont(13, FW_NORMAL);
    if (!g_hdrFont)   g_hdrFont   = mkFont(17, FW_SEMIBOLD);
    if (!g_btnFont)   g_btnFont   = mkFont(15, FW_SEMIBOLD);
    if (!g_titleFont) g_titleFont = mkFont(22, FW_SEMIBOLD);
    readTheme();

    static bool canvasReg = false;
    if (!canvasReg){
        WNDCLASSW wc = {};
        wc.lpfnWndProc = CanvasProc; wc.hInstance = hInst;
        wc.lpszClassName = L"MirrorCfgCanvas";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        canvasReg = true;
    }
    static bool dlgReg = false;
    if (!dlgReg){
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DlgProc; wc.hInstance = hInst;
        wc.lpszClassName = L"MirrorCfgDialog";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));  // app icon (IDI_ICON1)
        RegisterClassW(&wc);
        dlgReg = true;
    }

    DlgState st;
    st.monsStore = mons; st.mons = &st.monsStore;   // own the list so Refresh can update it
    st.out = &cfg; st.defaultPath = defaultPath;

    // client must fit: header + banner + cards + options + buttons + margin
    int clientW = LO_CARD_BX + LO_CARD_W + LO_MARGIN;
    int clientH = LO_BTN_Y + 40 + LO_MARGIN;
    RECT wr = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&wr, WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE, WS_EX_APPWINDOW);
    int W = wr.right - wr.left, H = wr.bottom - wr.top;
    // Place setup on a non-target display, so a topmost mirror cannot hide it.
    POINT point{}; GetCursorPos(&point);
    int ti=ResolveMonitor(mons,cfg.target);
    if (ti>=0 && PtInRect(&mons[ti].rect,point)) {
        for (int i=0;i<(int)mons.size();++i) if (i!=ti) {
            point={mons[i].rect.left+mons[i].w()/2,mons[i].rect.top+mons[i].h()/2}; break;
        }
    }
    MONITORINFO mi{}; mi.cbSize=sizeof(mi);
    RECT work{0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};
    if (GetMonitorInfoW(MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST),&mi)) work=mi.rcWork;
    int sx=work.left+std::max(0,(int)(work.right-work.left-W)/2);
    int sy=work.top+std::max(0,(int)(work.bottom-work.top-H)/2);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"MirrorCfgDialog", L"Mirror - Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, sx, sy, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return ConfigResult::Quit;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);

    createControls(hwnd, &st, hInst, banner);
    if (live) {
        SetWindowTextW(GetDlgItem(hwnd, ID_SAVE), L"Save && Apply");
        SetWindowTextW(GetDlgItem(hwnd, ID_QUIT), L"Cancel");
    }
    // seed from the incoming cfg (may be blank on first launch)
    controlsFromConfig(&st, cfg);
    applyTheme(hwnd);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0){
        if (!IsDialogMessageW(hwnd, &msg)){
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
    return st.result;
}
