<p align="center">
  <img src="docs/icon.png" width="96" alt="Monitor Mirror icon">
</p>

<h1 align="center">Active Monitor Mirror</h1>

<p align="center">
  A monitor mirroring tool — mirror a desktop region or automatically display
  the monitor under your cursor on a dedicated screen.
</p>

> **Windows 10 / 11 only** (64-bit). Not available for macOS or Linux.

---

## What it's for

Mirror your working display onto another monitor without changing Windows display
settings. Useful for pen displays, ultrawide monitors, secondary viewing screens
and OBS capture.

Choose a fixed source region, or use **Follow Mouse** to automatically mirror the
monitor you're currently working on. The image is scaled to fit the target while
preserving its aspect ratio.

Based on [Monitor Mirror by finaea](https://github.com/finaea/monitor-mirror).
The app mirrors pixels, not pen pressure or tablet input.

## Key features

- ⚡ **GPU mirroring** — Direct3D 11, DXGI Desktop Duplication and flip-model
  presentation, without video encoding.
- 🖥️ **Follow Mouse** — automatically switch between all non-target monitors as
  the cursor moves, fitting each display to the mirror.
- 🎯 **Visual setup** — select a source and target, identify physical displays,
  and drag or enter exact pixel regions in fixed-source mode.
- 🔍 **Magnifier** — a customizable cursor-following lens that samples the original
  capture texture, retaining detail otherwise lost when downscaling.
- 🪟 **Floating-window mode** — mirror into a movable, resizable window on any
  display, including the source.
- 🔄 **Rotation & flip** — 90° / 180° / 270° rotation and Flip X / Flip Y.
- 🧭 **Cursor & window guard** — keep the pointer off the dedicated mirror
  monitor and relocate application windows to accessible displays.
- 🎚️ **Tuning** — FPS limit, V-Sync, low latency, cursor visibility and black
  background.
- 🎨 **Windows appearance** — light/dark theme, accent color, tabbed settings
  and tooltips.
- 🧷 **Live configuration** — change settings without stopping the stream while
  editing. Apply changes when ready or cancel. Monitor selections use hardware
  identifiers where available; configurations can be imported and exported.
- 🛟 **Recovery & diagnostics** — automatic capture recovery, configuration
  rollback, performance statistics and optional logging.
- 📌 **Lives in the tray** — left-click to pause or resume; right-click for setup,
  diagnostics or quit.

---

## Build and run

The native C++ source is in [`cpp/`](cpp). Build with a MinGW-w64 C++17
toolchain, such as [MSYS2 UCRT64](https://www.msys2.org/).

In the MSYS2 UCRT64 terminal:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils
```

Add `C:\msys64\ucrt64\bin` to PATH, open a Windows command prompt in
`cpp/` and run:

```bat
build.bat
```

Run the resulting `mirror.exe`. Setup opens on the first run.

## The setup window

### Mirroring

Select the working monitor and mirror monitor. In fixed-source mode, drag
the source and target regions or enter pixel coordinates.

Select **Follow Mouse (all except target)** to automatically mirror the full
monitor under the cursor. All monitors must be connected to the same GPU;
manual crop, rotation, flip, FPS limit and floating-window controls are
unavailable in this mode.

**Identify monitors** displays the app's monitor numbers on the physical
screens. These may differ from Windows Display Settings.

| Option | What it does |
|---|---|
| FPS limit | Fixed-source frame-rate cap (0 = uncapped). |
| V-Sync | Synchronizes presentation with the target refresh rate. Off by default. |
| Low latency | Skips unchanged frames and bypasses the manual FPS cap. Off by default. |
| Show cursor | Includes the pointer in the mirror image. |
| Black background | Covers unused target space; forced on in Follow Mouse. |
| Floating window | Uses a movable, resizable mirror window. |
| Lock cursor to active screens | Keeps the pointer off the dedicated mirror monitor. |
| Keep windows off mirror monitor | Relocates existing and newly opened application windows. Some protected or elevated windows may resist relocation. |
| Log to file | Writes diagnostic information to `mirror.log`. |

Cursor confinement and window relocation are inactive while paused or in
floating-window mode.

### Magnifier

The Magnifier tab controls a lens on the mirrored output only.

| Option | What it does |
|---|---|
| Activation | Toggle hotkey, hold hotkey or always on. |
| Keyboard shortcut | Customizable; Alt+Shift+M by default. |
| Lens shape | Ellipse, rectangle or rounded rectangle. |
| Lens width / height | Independent sliders, 64–4096 target pixels (limited to the image size). |
| Magnification | 125–800% of the normal mirror scale. |
| Image edge behavior | Clip the lens or keep it inside the image. |
| Border | 0–20 pixels, with an RGB color wheel and exact RGB fields. |
| Edge blur | Feather the lens into the surrounding image over 0–64 pixels. |
| Sharp pixels | Nearest-neighbor lens sampling. Off by default. |

## Configuration and tray

Settings are saved in `mirror.cfg` beside the executable. Import and Export
allow configurations to be reused or backed up.

Open setup from the tray while mirroring is active. **Save & Apply** applies
the edited configuration; **Cancel** leaves it unchanged. Some changes
require rebuilding the capture pipeline.

Performance diagnostics report presentation and capture rates, active displays
and capture age. Capture age is not an end-to-end latency measurement.

## License

[MIT](LICENSE). A continuation of [finaea/monitor-mirror](https://github.com/finaea/monitor-mirror).
