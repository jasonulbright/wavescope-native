# SpectraViz (native)

A music visualizer built like an instrument, and built like a classic
Windows app: one C++ source tree, one statically linked .exe, zero runtime
dependencies. Everything links against OS DLLs only (user32, d2d1, dwrite,
ole32); `/MT` means no MSVC redistributable, no .NET, no Java, no webview.
The audit surface is this repo plus Windows.

This is the native successor to the WaveScope web app (frozen at
https://wavescope.higgsfield.app) and supersedes the Tauri experiment in
`C:\projects\spectraviz`. Web work is done; all augmentation happens here.

## Build

```
build.bat
```

Requires Visual Studio 2026 (any edition) for the compiler. Output:
`SpectraViz.exe`, ~200 KB, runs on stock Windows 10/11 forever.

## What works today

- **WASAPI loopback capture**: hears whatever the machine is playing
  (Spotify desktop included), silently, with no permission dialogs.
- **Own analysis chain**: Hann window + radix-2 FFT (2048 pt), log-axis
  sampling, band energies split at 250 Hz / 4 kHz, RMS level, and the same
  bass-flux beat envelope as the web engine. No FFT library; it's ~60 lines.
- **Direct2D renderer** with retained-contents phosphor trails (the fade
  wash technique ports 1:1 from the canvas engine).
- **All 34 modes ported** (`src/viz_modes.inl`), across five families
  (spectrum, waveform, particles, geometry, field), faithful to the web
  math. Plasma and Waterfall use CPU pixel buffers blitted through D2D.
- **Six trace palettes** (ultramarine, green phosphor, amber CRT, paper
  white, full spectrum, magma), cycled with `P`.
- **Real MilkDrop via projectM** (`M` key): libprojectM built for native
  MSVC runs in a child window with its own WGL/OpenGL 3.3 context layered
  over the Direct2D surface, fed PCM from the same loopback tap, rendering
  raw `.milk` presets. `SPACE`/arrows cycle presets while it is active.
  Bundled starters in `presets/`. Launch with `SpectraViz.exe milkdrop` to
  boot straight into it.
- HUD, fullscreen toggle, mode + palette cycling.

The core app is one `/MT` static `.exe` (~258 KB) with zero runtime deps.
projectM is the one deliberate dependency (LGPL-2.1): `projectM-4.dll` +
`projectM-4-playlist.dll` ship beside the exe (staged by `build.bat`), and
they need the VC runtime (present wherever VS or the VC redist is installed).

Keys: `SPACE`/`RIGHT` next (mode or preset), `LEFT` prev, `P` palette,
`M` MilkDrop engine, `F` fullscreen, `H` HUD, `ESC` quit.

projectM is built from `C:\projects\projectm` via `build-native.bat` there
(MSVC/Ninja, shared DLL, installed to `install-native/`). `build.bat` links
its import lib and stages the DLLs.

## Roadmap

1. **Preset library**: point projectM at a folder of raw `.milk` files (the
   15-year archive), preset playlist via `projectM-4-playlist`, and preset
   name in a GL-drawn or title-bar HUD (the D2D HUD is hidden under the GL
   child while projectM is active).
2. **Multi-monitor**: one borderless window + render loop per display
   (`EnumDisplayMonitors`), independent mode/palette per window, no
   permission prompts, no clicks.
3. **The console deck**: source select (loopback device picker, mic),
   shuffle timers, calm mode, resolution scaling; custom-drawn in the same
   pipeline, instrument style (mist paper + ultramarine brand).
4. **Spotify**: PKCE via WinHTTP with a localhost redirect; capture is
   already invisible thanks to loopback.
5. Tray icon, launch-at-login kiosk flag, `.milk`/preset hot-reload.

## Source map

- `src/main.cpp` — infrastructure (classic single-file style):
  - `AudioTap` — WASAPI loopback thread, ring buffer, FFT, `AudioFrame`
  - `Hsla()` / `BinLog()` — color + log-FFT helpers
  - `App` — window, D2D target, render loop, fullscreen, mode/palette state
- `src/viz_modes.inl` — the visualization library (included by main.cpp):
  - `PALETTES[]` — the six trace palettes
  - `DrawCtx` — the Direct2D primitives (rects, lines, circles, filled and
    stroked paths, transforms, CPU-buffer blit) mirroring the web `VizPaint`
  - `MODES[]` — the 34 modes; each is `void draw(DrawCtx&)`
- `src/projectm_host.inl` — the MilkDrop engine host (included by main.cpp):
  - `ProjectMHost` — child GL window, WGL 3.3 context bootstrap, the
    projectM instance, PCM feed, `.milk` preset load/cycle
