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

## What works today (phase 1)

- **WASAPI loopback capture**: hears whatever the machine is playing
  (Spotify desktop included), silently, with no permission dialogs.
- **Own analysis chain**: Hann window + radix-2 FFT (2048 pt), log-axis
  sampling, band energies split at 250 Hz / 4 kHz, RMS level, and the same
  bass-flux beat envelope as the web engine. No FFT library; it's ~60 lines.
- **Direct2D renderer** with retained-contents phosphor trails (the fade
  wash technique ports 1:1 from the canvas engine).
- **3 of 34 modes ported**: Bars, Oscilloscope, Radial bars.
- HUD, fullscreen toggle, mode cycling.

Keys: `SPACE`/`RIGHT` next mode, `LEFT` prev, `F` fullscreen, `H` HUD,
`ESC` quit.

## Roadmap

1. **Port the remaining 31 modes** from
   `spectraviz/app/src/lib/viz/modes/*.ts`. The math translates nearly
   line-for-line; `DrawCtx` mirrors the web `VizPaint` contract. Add the
   palette system (six trace palettes + custom).
2. **MilkDrop via the Emscripten-portable engine.** Vendor projectM's
   libprojectM: the maintained C++ MilkDrop lineage whose codebase is the
   basis of the Emscripten builds. It renders through OpenGL, so this phase
   adds a WGL context path alongside Direct2D (or migrates rendering to one
   GL pipeline; decide when it lands). Raw `.milk` preset folder support.
   Note: libprojectM is LGPL-2.1; link it as a DLL to keep obligations
   simple, or evaluate static-link compliance then.
3. **Multi-monitor**: one borderless window + render loop per display
   (`EnumDisplayMonitors`), independent mode/palette per window, no
   permission prompts, no clicks.
4. **The console deck**: source select (loopback device picker, mic),
   shuffle timers, calm mode, resolution scaling; custom-drawn in the same
   pipeline, instrument style (mist paper + ultramarine brand).
5. **Spotify**: PKCE via WinHTTP with a localhost redirect; capture is
   already invisible thanks to loopback.
6. Tray icon, launch-at-login kiosk flag, `.milk`/preset hot-reload.

## Source map

- `src/main.cpp` — everything, on purpose (classic style):
  - `AudioTap` — WASAPI loopback thread, ring buffer, FFT, `AudioFrame`
  - `Trace()` — the ultramarine palette (HSL, matches the web tokens)
  - `MODES[]` — the mode registry; each mode is `void draw(DrawCtx&)`
  - `App` — window, D2D target, render loop, fullscreen
