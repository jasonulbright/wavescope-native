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
- HUD, fullscreen toggle, mode + palette cycling. Still one dependency-free
  static `.exe` (~240 KB).

Keys: `SPACE`/`RIGHT` next mode, `LEFT` prev, `P` palette, `F` fullscreen,
`H` HUD, `ESC` quit.

## Roadmap

1. **MilkDrop via the Emscripten-portable engine.** Vendor projectM's
   libprojectM: the maintained C++ MilkDrop lineage whose codebase is the
   basis of the Emscripten builds. It renders through OpenGL, so this phase
   adds a WGL context path alongside Direct2D (or migrates rendering to one
   GL pipeline; decide when it lands). Raw `.milk` preset folder support.
   Note: libprojectM is LGPL-2.1; link it as a DLL to keep obligations
   simple, or evaluate static-link compliance then. (We already built
   libprojectM to WASM for the web app; the native build targets MSVC and
   links through a WGL/OpenGL context — see `C:\projects\projectm`.)
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
