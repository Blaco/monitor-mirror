// ===========================================================================
// monitors.h - Shared DXGI monitor enumeration.
//
// Both the configuration GUI (needs the list of monitors, their sizes and
// positions) and the renderer (needs the matching DXGI adapter/output to
// duplicate) use this. Monitors are identified by their device name
// (\\.\DISPLAYx) so a saved config can be matched back to hardware on launch.
// ===========================================================================
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cwchar>

struct MonitorInfo {
    std::wstring device;      // \\.\DISPLAYx  (can renumber on re-plug)
    std::wstring hwid;        // stable hardware id (monitor interface path / EDID-derived)
    RECT         rect{};      // desktop coordinates (physical pixels)
    bool         primary = false;
    UINT         adapterIdx = 0;
    UINT         outputIdx  = 0;
    unsigned     number = 0;   // UI only: active display-path order, never a capture ID
    std::wstring model;
    double       refreshHz = 0.0;

    int  w() const { return rect.right  - rect.left; }
    int  h() const { return rect.bottom - rect.top;  }

    std::wstring numberLabel() const { return L"Monitor " + std::to_wstring(number); }

    // UI label; preserve device and hwid separately for capture and persistence.
    std::wstring label() const {
        std::wstring s = numberLabel() + L" — " + std::to_wstring(w()) + L"x" + std::to_wstring(h());
        if (refreshHz > 0.0) {
            wchar_t hz[32]{};
            if (std::abs(refreshHz - std::round(refreshHz)) < 0.005)
                swprintf(hz, 32, L" @ %.0f Hz", refreshHz);
            else swprintf(hz, 32, L" @ %.2f Hz", refreshHz);
            s += hz;
        }
        if (primary) s += L" (Primary)";
        if (!model.empty()) s += L" — " + model;
        return s;
    }
};

// CCD provides the active path order and friendly target names. The path index
// is a useful approximation of Settings' Identify number, not a documented ID.
// Match by GDI source name, not DXGI enumeration order or connector ID.
inline void AddDisplayLabels(std::vector<MonitorInfo>& mons) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) break;
        paths.resize(np); modes.resize(nm);
        LONG rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER) { paths.clear(); continue; }
        if (rc != ERROR_SUCCESS) { paths.clear(); break; }
        paths.resize(np);
        break;
    }
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto& path = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        for (auto& mon : mons) {
            if (mon.number || lstrcmpiW(mon.device.c_str(), source.viewGdiDeviceName) != 0) continue;
            mon.number = static_cast<unsigned>(i + 1);
            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
                mon.model = target.monitorFriendlyDeviceName;
            const auto& rate = path.targetInfo.refreshRate;
            if (rate.Numerator && rate.Denominator)
                mon.refreshHz = static_cast<double>(rate.Numerator) / rate.Denominator;
            break;
        }
    }
    // If CCD is unavailable or a DXGI output has no matching path, allocate an
    // unused UI number. Never parse DISPLAYx, and never change hardware IDs.
    unsigned next = 1;
    for (auto& mon : mons) {
        if (!mon.number) {
            while (std::any_of(mons.begin(), mons.end(), [&](const MonitorInfo& m){ return m.number == next; })) ++next;
            mon.number = next++;
        }
        if (mon.model.empty()) {
            DISPLAY_DEVICEW dd{}; dd.cb = sizeof(dd);
            if (EnumDisplayDevicesW(mon.device.c_str(), 0, &dd, 0)) mon.model = dd.DeviceString;
        }
        if (mon.refreshHz <= 0.0) {
            DEVMODEW dm{}; dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(mon.device.c_str(), ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                mon.refreshHz = dm.dmDisplayFrequency;
        }
    }
    std::sort(mons.begin(), mons.end(), [](const MonitorInfo& a, const MonitorInfo& b){ return a.number < b.number; });
}

// Enumerate all desktop-attached outputs via DXGI. The primary is the one whose
// top-left is the desktop origin (0,0).
inline std::vector<MonitorInfo> EnumMonitors() {
    using Microsoft::WRL::ComPtr;
    std::vector<MonitorInfo> mons;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())))
        return mons;

    ComPtr<IDXGIAdapter1> ad;
    for (UINT ai = 0; SUCCEEDED(factory->EnumAdapters1(ai, ad.ReleaseAndGetAddressOf())); ++ai) {
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; SUCCEEDED(ad->EnumOutputs(oi, o.ReleaseAndGetAddressOf())); ++oi) {
            DXGI_OUTPUT_DESC d{};
            if (FAILED(o->GetDesc(&d))) continue;
            if (!d.AttachedToDesktop) continue;
            MonitorInfo mi;
            mi.device     = d.DeviceName;
            mi.rect       = d.DesktopCoordinates;
            mi.primary    = (d.DesktopCoordinates.left == 0 && d.DesktopCoordinates.top == 0);
            mi.adapterIdx = ai;
            mi.outputIdx  = oi;
            // stable hardware id: the monitor interface path (contains the EDID
            // manufacturer/product and a per-connection instance), which survives
            // \\.\DISPLAYx renumbering when displays are re-plugged.
            DISPLAY_DEVICEW dd = {}; dd.cb = sizeof(dd);
            if (EnumDisplayDevicesW(d.DeviceName, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0])
                mi.hwid = dd.DeviceID;
            mons.push_back(mi);
        }
    }
    AddDisplayLabels(mons);
    return mons;
}

// Find a monitor by device name. Returns index into `mons`, or -1.
inline int FindMonitor(const std::vector<MonitorInfo>& mons, const std::wstring& device) {
    for (size_t i = 0; i < mons.size(); ++i)
        if (mons[i].device == device) return (int)i;
    return -1;
}
