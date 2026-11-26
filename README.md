<p align="center">
  <img src="mirror.ico" width="96" alt="Active Monitor Mirror icon">
</p>

<h1 align="center">Active Monitor Mirror</h1>

<p align="center">
  A low-latency Windows monitor mirroring tool with automatic source switching,
  target-monitor guardrails, and a cursor-following magnifier.
</p>

> **Windows 10 / 11 only** (64-bit). Use extended displays for a dedicated mirror monitor.

---

## What it's for

Active Monitor Mirror is a continuation of [Monitor Mirror by finaea](https://github.com/finaea/monitor-mirror).

It mirrors a desktop region or an entire monitor onto another display or a floating window using Direct3D 11 and DXGI Desktop Duplication. It can also automatically switch the mirrored source based on the mouse position, which makes it useful for a dedicated preview or mirror monitor that follows the active screen.

## Key features

- ⚡ **Native GPU mirroring** — Direct3D 11 and DXGI Desktop Duplication with GPU scaling and no video encoding.
- 🖥️ **Fixed source or Follow Mouse** — mirror a selected monitor region, or automatically mirror whichever non-target display contains the cursor.
- 🔍 **Magnifier tab** — magnify a configurable area around the cursor on the mirrored output, using the original source texture for better readability after downscaling.
- 🪟 **Floating window mode** — mirror into a movable, resizable window instead of a dedicated output display.
- 🧷 **Target monitor guardrails** — optionally keep the cursor and normal application windows off the mirror monitor while mirroring is active.
- 🎚️ **Display controls** — FPS limit, V-Sync, low latency mode, cursor visibility, rotation, Flip X / Flip Y, and black background.
- 🪪 **Monitor identification** — identify monitors from the setup window and save selections using hardware identifiers where available.
- 🌗 **Windows theme-aware UI** — setup window follows the active Windows light/dark theme and accent color.
- 📌 **Tray operation** — pause/resume from the tray, reopen setup, and view performance diagnostics.

---

## Build

Build with a MinGW-w64 C++17 toolchain, such as **MSYS2 UCRT64**.

Install the compiler and resource tools in the MSYS2 UCRT64 terminal:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils
```

Add `C:\msys64\ucrt64\bin` to `PATH`, open a Windows command prompt in the repository folder, and run:

```bat
build.bat
```

This produces `mirror.exe`. The first launch opens the setup window.

## Mirroring options

| Option | Description |
|---|---|
| **FPS** | Fixed-source frame-rate limit. `0` means uncapped. |
| **V-Sync** | Synchronize presentation with the target monitor refresh rate. Off by default. |
| **Low latency** | Prefer the newest frame and skip unchanged-frame presentation work. Off by default. |
| **Show cursor** | Draw the mouse pointer on the mirrored output. |
| **Black background** | Fill unused parts of the target region with black in fixed-source mode. |
| **Rotation / Flip X / Flip Y** | Control mirrored image orientation in fixed-source mode. |
| **Lock cursor to active screens** | Keep the mouse pointer off the dedicated mirror monitor. |
| **Keep windows off mirror monitor** | Relocate ordinary application windows away from the mirror monitor. |
| **Floating window** | Show the mirror in a separate window instead of on a dedicated target monitor. |
| **Log to file** | Write diagnostic output to `mirror.log`. |

## Magnifier

The Magnifier tab adds a configurable lens on the mirrored output only.

- **Activation modes:** always on, toggle hotkey, or hold hotkey.
- **Default shortcut:** `Alt+Shift+M`.
- **Shapes:** ellipse, rectangle, or rounded rectangle.
- **Size controls:** independent lens width and height.
- **Magnification:** 125% to 800%.
- **Border:** adjustable thickness and RGB color picker.
- **Edge blur:** feather the lens edge from 0 to 64 pixels.
- **Sampling:** optional sharp-pixel mode for nearest-neighbor magnification.

## Configuration

Settings are stored in `mirror.cfg` beside the executable. Use **Import** and **Export** in the setup window to load or save configurations.

If setup is opened while mirroring is active, the stream continues until new settings are applied.

## Project layout

This repository contains the native C++ version only. Older Python-based files and other no-longer-used project files are intentionally omitted.

## License

[MIT](LICENSE). Original project by [finaea](https://github.com/finaea/monitor-mirror).
