// ===========================================================================
// config.h - Mirror configuration: model, load/save, and validation.
//
// The config describes WHAT to mirror and WHERE:
//   source : a monitor + a pixel rectangle on it (the drawing / working area)
//   target : a monitor + a rectangle on it (where the mirror is shown), whose
//            aspect ratio is locked to the source rectangle's (accounting for
//            rotation). The target may instead be a floating, movable/resizable
//            window (windowed mode) - handy for single-monitor use and OBS.
//   fps / rotation / flip / cover / cursor / vsync / log : options.
//
// Rectangles are stored MONITOR-LOCAL (0,0 = that monitor's top-left) so a
// config stays meaningful if the desktop is rearranged. Monitors are keyed by a
// stable hardware id (EDID-derived) with the \\.\DISPLAYx device name as a
// fallback, so re-plugging that renumbers displays doesn't break the config.
//
// The file is a tiny INI, auto-loaded/auto-saved next to the exe.
// ===========================================================================
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>
#include <algorithm>
#include "monitors.h"

struct RegionCfg {
    std::wstring device;              // \\.\DISPLAYx (fallback key)
    std::wstring hwid;                // stable hardware id (primary key)
    int x = 0, y = 0, w = 0, h = 0;   // rectangle, monitor-local pixels
};

// Lens dimensions are target-image pixels; zoom is relative to the normal mirror.
struct MagnifierConfig {
    bool enabled = false;
    int mode = 0;       // 0 toggle hotkey, 1 hold hotkey, 2 always on
    int shape = 0;      // 0 ellipse, 1 rectangle, 2 rounded rectangle
    int width = 420, height = 300, zoom = 200; // percent
    int edge = 0;       // 0 clip at image edge, 1 keep lens inside image
    int border = 3, color = 0xFFFFFF; // COLORREF, packed RGB
    int feather = 0; // inward edge blend in target pixels
    bool sharp = false;
    int modifiers = MOD_ALT | MOD_SHIFT, key = 'M';
    void normalize() {
        mode=std::clamp(mode,0,2); shape=std::clamp(shape,0,2);
        width=std::clamp(width,64,4096); height=std::clamp(height,64,4096);
        zoom=std::clamp(zoom,125,800); edge=std::clamp(edge,0,1);
        border=std::clamp(border,0,20); color=std::clamp(color,0,0xFFFFFF); feather=std::clamp(feather,0,64);
        modifiers &= MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        if (key<=0 || key>254 || key==VK_F12 || key==VK_SHIFT || key==VK_CONTROL || key==VK_MENU ||
            key==VK_LSHIFT || key==VK_RSHIFT || key==VK_LCONTROL || key==VK_RCONTROL || key==VK_LMENU || key==VK_RMENU ||
            key==VK_LWIN || key==VK_RWIN || (!modifiers && !(key>=VK_F1 && key<=VK_F11))) {
            modifiers=MOD_ALT|MOD_SHIFT; key='M';
        }
    }
};

struct MirrorConfig {
    MagnifierConfig magnifier;
    bool lowLatency = false; // optional newest-frame presentation
    bool guardWindows = true; // keep application windows off the dedicated mirror display
    bool lockCursor = true; // exclude the mirror monitor when enabled
    bool followMouse = false; // all non-target monitors (exactly two in a three-monitor setup)
    RegionCfg source;
    RegionCfg target;
    int  fps      = 0;        // 0 = uncapped
    bool vsync    = false;
    bool cursor   = true;
    bool cover    = true;     // fill the rest of the mirror monitor black
    bool windowed = false;    // mirror into a floating window (may share the source screen)
    int  rotation = 0;        // 0 / 90 / 180 / 270 degrees (clockwise)
    bool flipH    = false;    // mirror horizontally
    bool flipV    = false;    // mirror vertically
    bool log      = false;    // write mirror.log next to the exe

    double srcAspect() const { return source.h ? (double)source.w / source.h : 1.0; }
    // aspect the TARGET rectangle must have: rotation by 90/270 swaps w/h.
    double effTargetAspect() const {
        if (!source.w || !source.h) return 1.0;
        bool swap = (rotation == 90 || rotation == 270);
        return swap ? (double)source.h / source.w : (double)source.w / source.h;
    }
};

// ---- exe-directory helpers -------------------------------------------------
inline std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}
inline std::wstring DefaultConfigPath() { return ExeDir() + L"\\mirror.cfg"; }

// ---- monitor resolution: stable hwid first, device name as fallback --------
inline int ResolveMonitor(const std::vector<MonitorInfo>& mons, const RegionCfg& r) {
    if (!r.hwid.empty()) {
        for (size_t i = 0; i < mons.size(); ++i)
            if (!mons[i].hwid.empty() && mons[i].hwid == r.hwid) return (int)i;
    }
    // A different known hardware ID must not silently become the target.
    int i = FindMonitor(mons, r.device);
    if (i >= 0 && !r.hwid.empty() && !mons[i].hwid.empty()) return -1;
    return i;   // fall back to \\.\DISPLAYx
}

// ---- tiny INI writer/reader ------------------------------------------------
inline bool SaveConfig(const MirrorConfig& c, const std::wstring& path) {
    std::wstring temp = path + L".tmp";
    FILE* f = _wfopen(temp.c_str(), L"w, ccs=UTF-8");
    if (!f) return false;
    // NOTE: %ls (not %s) - in this CRT's wide fwprintf, %s treats the argument
    // as a narrow string and truncates the wchar_t* at its first zero byte.
    fwprintf(f, L"# mirror configuration\n");
    fwprintf(f, L"[source]\ndevice=%ls\nhwid=%ls\nx=%d\ny=%d\nw=%d\nh=%d\n",
             c.source.device.c_str(), c.source.hwid.c_str(), c.source.x, c.source.y, c.source.w, c.source.h);
    fwprintf(f, L"[target]\ndevice=%ls\nhwid=%ls\nx=%d\ny=%d\nw=%d\nh=%d\n",
             c.target.device.c_str(), c.target.hwid.c_str(), c.target.x, c.target.y, c.target.w, c.target.h);
    fwprintf(f, L"[options]\nfps=%d\nvsync=%d\ncursor=%d\ncover=%d\nwindowed=%d\nrotation=%d\nfliph=%d\nflipv=%d\nlog=%d\n",
             c.fps, c.vsync?1:0, c.cursor?1:0, c.cover?1:0, c.windowed?1:0, c.rotation, c.flipH?1:0, c.flipV?1:0, c.log?1:0);
    fwprintf(f, L"followmouse=%d\n", c.followMouse ? 1 : 0);
    fwprintf(f, L"lockcursor=%d\n", c.lockCursor ? 1 : 0);
    fwprintf(f, L"guardwindows=%d\n", c.guardWindows ? 1 : 0);
    fwprintf(f, L"lowlatency=%d\n", c.lowLatency ? 1 : 0);
    const auto& m=c.magnifier;
    fwprintf(f,L"[magnifier]\nenabled=%d\nmode=%d\nshape=%d\nwidth=%d\nheight=%d\nzoom=%d\nedge=%d\nborder=%d\nrgb=%d\nfeather=%d\nsharp=%d\nmodifiers=%d\nkey=%d\n",
        int(m.enabled),m.mode,m.shape,m.width,m.height,m.zoom,m.edge,m.border,m.color,m.feather,int(m.sharp),m.modifiers,m.key);
    bool ok = fflush(f) == 0 && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

inline bool LoadConfig(MirrorConfig& c, const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"r, ccs=UTF-8");
    if (!f) return false;
    wchar_t line[512];
    std::wstring section;
    bool sawSource = false, sawTarget = false;
    while (fgetws(line, 512, f)) {
        std::wstring s(line);
        size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring::npos) continue;
        size_t b = s.find_last_not_of(L" \t\r\n");
        s = s.substr(a, b - a + 1);
        if (s.empty() || s[0] == L'#' || s[0] == L';') continue;
        if (s[0] == L'[') { section = s.substr(1, s.find(L']') - 1); continue; }

        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = s.substr(0, eq), val = s.substr(eq + 1);

        RegionCfg* r = nullptr;
        if (section == L"source") { r = &c.source; sawSource = true; }
        else if (section == L"target") { r = &c.target; sawTarget = true; }

        if (r) {
            if      (key == L"device") r->device = val;
            else if (key == L"hwid")   r->hwid = val;
            else if (key == L"x") r->x = _wtoi(val.c_str());
            else if (key == L"y") r->y = _wtoi(val.c_str());
            else if (key == L"w") r->w = _wtoi(val.c_str());
            else if (key == L"h") r->h = _wtoi(val.c_str());
        } else if (section == L"magnifier") {
            auto& m=c.magnifier; int n=_wtoi(val.c_str());
            if (key==L"enabled") m.enabled=n!=0;
            else if (key==L"mode") m.mode=n;
            else if (key==L"shape") m.shape=n;
            else if (key==L"width") m.width=n;
            else if (key==L"height") m.height=n;
            else if (key==L"zoom") m.zoom=n;
            else if (key==L"edge") m.edge=n;
            else if (key==L"border") m.border=n;
            else if (key==L"color") { // old four-color palette
                const int palette[]={0xFFFFFF,0x000000,0xFFFF00,0x00FFFF};
                m.color=palette[std::clamp(n,0,3)];
            }
            else if (key==L"rgb") m.color=n;
            else if (key==L"feather") m.feather=n;
            else if (key==L"sharp") m.sharp=n!=0;
            else if (key==L"modifiers") m.modifiers=n;
            else if (key==L"key") m.key=n;
        } else if (section == L"options") {
            if (key == L"lowlatency") c.lowLatency = _wtoi(val.c_str()) != 0;
            else if (key == L"guardwindows") c.guardWindows = _wtoi(val.c_str()) != 0;
            else if (key == L"lockcursor") c.lockCursor = _wtoi(val.c_str()) != 0;
            else if (key == L"followmouse") c.followMouse = _wtoi(val.c_str()) != 0;
            else if (key == L"fps")      c.fps      = _wtoi(val.c_str());
            else if (key == L"vsync")    c.vsync    = _wtoi(val.c_str()) != 0;
            else if (key == L"cursor")   c.cursor   = _wtoi(val.c_str()) != 0;
            else if (key == L"cover")    c.cover    = _wtoi(val.c_str()) != 0;
            else if (key == L"windowed") c.windowed = _wtoi(val.c_str()) != 0;
            else if (key == L"rotation") c.rotation = _wtoi(val.c_str());
            else if (key == L"fliph")    c.flipH    = _wtoi(val.c_str()) != 0;
            else if (key == L"flipv")    c.flipV    = _wtoi(val.c_str()) != 0;
            else if (key == L"log")      c.log      = _wtoi(val.c_str()) != 0;
        }
    }
    fclose(f);
    c.magnifier.normalize();
    if (c.rotation != 0 && c.rotation != 90 && c.rotation != 180 && c.rotation != 270) c.rotation = 0;
    return sawSource && sawTarget && !c.source.device.empty() && !c.target.device.empty();
}

// Follow Mouse does not depend on the saved preview source.
inline int FollowSource(const std::vector<MonitorInfo>& mons, int ti) {
    if (ti < 0) return -1;
    POINT p{}; GetCursorPos(&p);
    int first = -1;
    for (int i = 0; i < (int)mons.size(); ++i) {
        if (i == ti || mons[i].adapterIdx != mons[ti].adapterIdx) continue;
        if (first < 0) first = i;
        if (PtInRect(&mons[i].rect, p)) return i;
    }
    return first;
}

// ---- validation against the CURRENT monitors -------------------------------
// Returns an empty string if the config is usable right now, otherwise a
// human-readable reason (shown as the banner in the setup dialog).
inline std::wstring ValidateConfig(const MirrorConfig& c, const std::vector<MonitorInfo>& mons) {
    if (mons.empty())
        return L"No monitors were detected.";

    int si = ResolveMonitor(mons, c.source);
    int ti = ResolveMonitor(mons, c.target);
    if (ti < 0)
        return L"The mirror monitor from your configuration is not connected.";
    if (c.followMouse) {
        if (FollowSource(mons, ti) < 0) return L"Follow Mouse requires at least two monitors and distinct source and target.";
        for (const auto& mon : mons) if (mon.adapterIdx != mons[ti].adapterIdx) return L"Follow Mouse requires all monitors on the same GPU.";
        return L"";
    }
    if (si < 0) return L"The main working monitor from your configuration is not connected.";
    if (si == ti && !c.windowed)
        return L"The working monitor and the mirror monitor are the same. Pick two different "
               L"monitors, or turn on \"Floating window\".";

    const MonitorInfo& sm = mons[si];
    const MonitorInfo& tm = mons[ti];

    if (c.source.w < 8 || c.source.h < 8)
        return L"The working area is too small.";
    if (c.source.x < 0 || c.source.y < 0 ||
        c.source.x + c.source.w > sm.w() || c.source.y + c.source.h > sm.h())
        return L"The working area no longer fits on the main monitor "
               L"(its resolution may have changed).";

    if (c.target.w < 8 || c.target.h < 8)
        return L"The mirror area is too small.";
    if (c.target.x < 0 || c.target.y < 0 ||
        c.target.x + c.target.w > tm.w() || c.target.y + c.target.h > tm.h())
        return L"The mirror area no longer fits on the mirror monitor "
               L"(its resolution may have changed).";

    // aspect ratio must match the (rotation-adjusted) working area; allow ~2% drift.
    double sa = c.effTargetAspect();
    double ta = c.target.h ? (double)c.target.w / c.target.h : 0.0;
    if (sa > 0 && ta > 0) {
        double diff = (sa > ta) ? (sa - ta) / sa : (ta - sa) / ta;
        if (diff > 0.02)
            return L"The mirror area no longer matches the working area's shape (aspect ratio).";
    }

    if (c.fps < 0 || c.fps > 1000)
        return L"The frame rate is out of range (0-1000).";

    return L"";  // OK
}
