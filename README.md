<p align="center">
  <img src="docs/icon.png" width="96" alt="Monitor Mirror icon">
</p>

<h1 align="center">Active Monitor Mirror</h1>

<p align="center">
  Mirror a monitor or desktop region to another display, with optional automatic source switching and a cursor-following magnifier.
</p>

> **Windows 10 / 11 (64-bit).** Use extended displays for a dedicated mirror monitor.

A continuation of [Monitor Mirror by finaea](https://github.com/finaea/monitor-mirror), using Direct3D 11 and DXGI Desktop Duplication.

## Features

- **Fixed source:** Mirror an entire monitor or a selected region, with aspect-ratio-preserving scaling, rotation and Flip X / Flip Y.
- **Follow Mouse:** Automatically mirror the display under the cursor, excluding the designated target.
- **Floating window:** Mirror into a movable, resizable window, including on the source monitor.
- **Magnifier:** Enlarge a customizable area around the cursor using the original source texture.
- **Cursor and window guard:** Keep the pointer and application windows off the dedicated mirror monitor.
- **Visual setup:** Identify displays, select regions and adjust settings while mirroring continues.
- **Performance options:** FPS limit, V-Sync, low latency mode and tray diagnostics.

## Build and run

The native C++ source is in [`cpp/`](cpp). Build with a MinGW-w64 C++17 toolchain, such as [MSYS2 UCRT64](https://www.msys2.org/).

In the MSYS2 UCRT64 terminal, install the compiler and resource tools:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-binutils
```

Add `C:\msys64\ucrt64\bin` to PATH, open a Windows command prompt in `cpp/`, and run `build.bat`. Start the resulting `mirror.exe`. The first run opens the setup window.

## Mirroring

Select a source and target monitor. In fixed-source mode, drag the source and target regions or enter pixel coordinates. Use rotation and Flip X / Flip Y to adjust image orientation.

Select **Follow Mouse (all except target)** to automatically fit the full display under the cursor to the target. Manual crop, orientation, FPS cap and floating-window controls are unavailable in this mode. All monitors must be connected to the same GPU.

**Identify monitors** shows the program's monitor numbers on the physical screens. These may differ from Windows Settings. Monitor selections use hardware identifiers where available.

| Option | Description |
| --- | --- |
| FPS | Fixed-source frame-rate limit; 0 is uncapped. |
| V-Sync | Synchronizes presentation with the target refresh. Off by default. |
| Low latency | Skips unchanged frames and bypasses the manual FPS cap. Off by default. |
| Show cursor | Includes the mouse pointer in the mirrored image. |
| Black background | Covers unused portions of the target display in fixed-source mode. |
| Lock cursor to active screens | Keeps the pointer off the dedicated target monitor. |
| Keep windows off mirror monitor | Relocates application windows to accessible displays. Protected or elevated windows may resist relocation. |
| Log to file | Writes diagnostic messages to `mirror.log`. |

Cursor confinement and window relocation are suspended when mirroring is paused and are not used in floating-window mode.

## Magnifier

Enable the lens on the Magnifier tab to enlarge the captured area around the mouse pointer on the mirror only. It samples the original capture texture rather than the downscaled image.

- **Activation:** Toggle shortcut, hold shortcut or always on. The default shortcut is Alt+Shift+M and can be changed.
- **Shape and size:** Ellipse, rectangle or rounded rectangle, with independent width and height in target pixels.
- **Magnification:** 125–800% of the normal mirror scale.
- **Image edge behavior:** Clip the lens at the image boundary or shift it inside the image.
- **Border:** Adjust thickness (0 hides it) and choose any RGB color using the color wheel or numerical RGB fields.
- **Edge blur:** Feather the lens into the normal image over 0–64 target pixels.
- **Sharp pixels:** Nearest-neighbor lens sampling instead of linear interpolation. Off by default.

## Tray and configuration

Left-click the tray icon to pause or resume mirroring. Right-click for setup, performance diagnostics and quit.

The stream continues while setup is open. **Save & Apply** applies changes; **Cancel** discards them. Some changes require rebuilding capture resources.

Settings are stored in `mirror.cfg` beside the executable. Use Import and Export in setup to load or back up configurations. Existing settings take precedence over defaults. If a display is disconnected or capture fails, the application attempts to recover automatically.

## License

[MIT](LICENSE). Based on [finaea/monitor-mirror](https://github.com/finaea/monitor-mirror).
