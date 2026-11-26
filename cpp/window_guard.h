// ===========================================================================
// window_guard.h - Keep application windows off the dedicated mirror display.
// A separate worker receives asynchronous WinEvents and periodically rescans.
// No input hooks, injected DLLs, display changes, or renderer-side enumeration.
// ===========================================================================
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "monitors.h"

class WindowGuard {
    HANDLE stop_ = nullptr, thread_ = nullptr;
    HWND mirror_ = nullptr;
    RECT targetRect_{};
    HMONITOR target_ = nullptr, preferred_ = nullptr;
    std::vector<HMONITOR> allowed_;
    std::unordered_map<HWND, ULONGLONG> attempts_;
    bool dirty_ = true;
    // The notification callback is invoked on the registering worker thread.
    inline static thread_local WindowGuard* current_ = nullptr;

    bool stopping() const { return WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT; }
    bool allowed(HMONITOR m) const {
        return m && std::find(allowed_.begin(), allowed_.end(), m) != allowed_.end();
    }
    static bool workArea(HMONITOR m, MONITORINFO& mi) {
        mi = {}; mi.cbSize = sizeof(mi);
        return m && GetMonitorInfoW(m, &mi) && mi.rcWork.right > mi.rcWork.left && mi.rcWork.bottom > mi.rcWork.top;
    }
    static HMONITOR monitorFor(const RECT& r) { return MonitorFromRect(&r, MONITOR_DEFAULTTONULL); }
    static bool shellWindow(HWND w) {
        if (w == GetDesktopWindow() || w == GetShellWindow()) return true;
        wchar_t cls[128]{};
        GetClassNameW(w, cls, 128);
        for (const wchar_t* name : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
                                    L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland",
                                    L"#32768", L"tooltips_class32", L"SysShadow", L"TaskListThumbnailWnd",
                                    L"MultitaskingViewFrame", L"DV2ControlHost"})
            if (lstrcmpiW(cls, name) == 0) return true;
        return false;
    }
    HMONITOR destination(HWND w) const {
        // Keep owned dialogs with their visible parent when possible.
        HWND owner = GetWindow(w, GW_OWNER);
        if (owner && IsWindowVisible(owner) && !IsIconic(owner)) {
            HMONITOR m = MonitorFromWindow(owner, MONITOR_DEFAULTTONULL);
            if (allowed(m)) return m;
        }
        POINT p{};
        if (GetCursorPos(&p)) {
            HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
            if (allowed(m)) return m;
        }
        if (allowed(preferred_)) return preferred_;
        return allowed_.empty() ? nullptr : allowed_.front();
    }
    // Keep the position relative to the old work area, but fit the window in
    // the destination. Non-resizable windows retain their size and visible title.
    static RECT relocate(RECT r, const RECT& from, const RECT& to, bool resizable) {
        LONG w = r.right-r.left, h = r.bottom-r.top;
        LONG aw = to.right-to.left, ah = to.bottom-to.top;
        if (resizable) { w = std::min(w, aw); h = std::min(h, ah); }
        LONG x = std::clamp(to.left+(r.left-from.left), to.left, to.left+std::max(0L,aw-w));
        LONG y = std::clamp(to.top+(r.top-from.top), to.top, to.top+std::max(0L,ah-h));
        return {x,y,x+w,y+h};
    }
    void moveWindow(HWND w) {
        if (stopping() || w == mirror_ || !IsWindowVisible(w)) return;
        LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE), ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((style & WS_CHILD) || (ex & WS_EX_NOACTIVATE) || shellWindow(w)) return;
        // Skip transient tool windows, but retain captioned tools and dialogs.
        if ((ex & WS_EX_TOOLWINDOW) && (style & WS_CAPTION) != WS_CAPTION) return;
        if (MonitorFromWindow(w, MONITOR_DEFAULTTONULL) != target_) return;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return;
        ULONGLONG now = GetTickCount64();
        auto it = attempts_.find(w);
        if (it != attempts_.end() && now-it->second < 750) return;
        MONITORINFO from{}, to{};
        if (!workArea(target_, from) || !EqualRect(&from.rcMonitor, &targetRect_) || !workArea(destination(w), to)) return;
        if (EqualRect(&from.rcMonitor, &to.rcMonitor)) return;
        bool resizable = (style & WS_THICKFRAME) != 0;
        if (IsIconic(w) || IsZoomed(w)) {
            WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
            if (!GetWindowPlacement(w, &wp)) return;
            // Normal placement uses workspace coordinates for ordinary windows,
            // screen coordinates for tool windows. Keep the existing show state.
            RECT r = wp.rcNormalPosition;
            if (!(ex & WS_EX_TOOLWINDOW)) OffsetRect(&r, from.rcWork.left-from.rcMonitor.left, from.rcWork.top-from.rcMonitor.top);
            // A maximized window can have restore bounds on a different monitor.
            // If so, use the monitor containing those bounds as the origin.
            MONITORINFO origin{};
            if (!workArea(monitorFor(r), origin)) origin = from;
            if (r.right <= r.left || r.bottom <= r.top) return;
            r = relocate(r, origin.rcWork, to.rcWork, resizable);
            if (!(ex & WS_EX_TOOLWINDOW)) OffsetRect(&r, to.rcMonitor.left-to.rcWork.left, to.rcMonitor.top-to.rcWork.top);
            wp.rcNormalPosition = r;
            wp.ptMinPosition = {-1,-1}; wp.ptMaxPosition = {-1,-1};
            wp.flags = (wp.flags & WPF_RESTORETOMAXIMIZED) | WPF_ASYNCWINDOWPLACEMENT;
            if (stopping()) return;
            attempts_[w] = now;
            SetWindowPlacement(w, &wp);
        } else {
            RECT r{};
            if (!GetWindowRect(w, &r) || r.right <= r.left || r.bottom <= r.top) return;
            RECT dest = relocate(r, from.rcWork, to.rcWork, resizable);
            UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
            if (dest.right-dest.left == r.right-r.left && dest.bottom-dest.top == r.bottom-r.top) flags |= SWP_NOSIZE;
            if (stopping()) return;
            attempts_[w] = now;
            SetWindowPos(w, nullptr, dest.left, dest.top, dest.right-dest.left, dest.bottom-dest.top, flags);
        }
    }
    static BOOL CALLBACK enumerate(HWND w, LPARAM arg) {
        auto* self = reinterpret_cast<WindowGuard*>(arg);
        if (self->stopping()) return FALSE;
        self->moveWindow(w);
        return TRUE;
    }
    static void CALLBACK onEvent(HWINEVENTHOOK, DWORD, HWND w, LONG object, LONG child, DWORD, DWORD) {
        if (current_ && w && w != current_->mirror_ && object == OBJID_WINDOW && child == 0 &&
            GetAncestor(w, GA_ROOT) == w && MonitorFromWindow(w, MONITOR_DEFAULTTONULL) == current_->target_)
            current_->dirty_ = true;
    }
    static DWORD WINAPI run(LPVOID arg) {
        auto* self = static_cast<WindowGuard*>(arg);
        current_ = self;
        MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
        HWINEVENTHOOK hooks[] = {
            SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, onEvent, 0, 0, WINEVENT_OUTOFCONTEXT),
            SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, onEvent, 0, 0, WINEVENT_OUTOFCONTEXT),
            SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, onEvent, 0, 0, WINEVENT_OUTOFCONTEXT),
            SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, onEvent, 0, 0, WINEVENT_OUTOFCONTEXT)
        };
        ULONGLONG next = 0, last = 0;
        while (!self->stopping()) {
            // Bound message processing to avoid starvation on busy desktops.
            for (int i=0; i<128 && PeekMessageW(&msg,nullptr,0,0,PM_REMOVE); ++i) {
                if (msg.message == WM_QUIT) { SetEvent(self->stop_); break; }
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (self->stopping()) break;
            ULONGLONG now = GetTickCount64();
            if (now >= next || (self->dirty_ && now-last >= 50)) {
                self->dirty_ = false;
                EnumWindows(enumerate, reinterpret_cast<LPARAM>(self));
                now = GetTickCount64(); last = now; next = now+500;
                for (auto it = self->attempts_.begin(); it != self->attempts_.end();) {
                    if (now-it->second > 5000) it = self->attempts_.erase(it); else ++it;
                }
            }
            ULONGLONG due = self->dirty_ ? std::min(next,last+50) : next;
            DWORD delay = due > now ? static_cast<DWORD>(due-now) : 0;
            if (MsgWaitForMultipleObjects(1,&self->stop_,FALSE,delay,QS_ALLINPUT) == WAIT_FAILED) break;
        }
        for (auto h : hooks) if (h) UnhookWinEvent(h);
        current_ = nullptr;
        return 0;
    }
public:
    WindowGuard() = default;
    WindowGuard(const WindowGuard&) = delete;
    WindowGuard& operator=(const WindowGuard&) = delete;
    ~WindowGuard() { stop(); }
    bool start(HWND mirror, const std::vector<MonitorInfo>& mons, int ti, int si) {
        if (thread_) {
            if (WaitForSingleObject(thread_,0) == WAIT_TIMEOUT) return true;
            stop();
        }
        if (ti < 0 || ti >= static_cast<int>(mons.size())) return false;
        targetRect_ = mons[ti].rect; target_ = monitorFor(targetRect_); mirror_ = mirror;
        if (!target_) return false;
        allowed_.clear(); preferred_ = nullptr;
        for (int i=0; i<static_cast<int>(mons.size()); ++i) if (i != ti) {
            HMONITOR m = monitorFor(mons[i].rect);
            if (m && m != target_ && !allowed(m)) allowed_.push_back(m);
            if (m && m != target_ && i == si) preferred_ = m;
        }
        if (allowed_.empty()) return false;
        dirty_ = true;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) return false;
        thread_ = CreateThread(nullptr,0,run,this,0,nullptr);
        if (!thread_) { CloseHandle(stop_); stop_ = nullptr; return false; }
        return true;
    }
    void stop() {
        if (!thread_) return;
        SetEvent(stop_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_); CloseHandle(stop_);
        thread_ = stop_ = nullptr;
        attempts_.clear(); allowed_.clear();
    }
};
