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
- **GPU shader engine** (`G` key, `src/shader_host.inl`): three per-pixel
  fragment shaders (plasma field, warp tunnel, fractal fold) ported 1:1 from
  the web build's WebGPU modes to GLSL 330, compiled at runtime and drawn as
  a fullscreen triangle on their own GL context, audio-uniform driven. No new
  dependency — pure GPU + OS. `SPACE`/arrows cycle shaders.
- HUD, fullscreen toggle, mode + palette cycling.

The core app is one `/MT` static `.exe` (~258 KB) with zero runtime deps.
projectM is the one deliberate dependency (LGPL-2.1): `projectM-4.dll` +
`projectM-4-playlist.dll` ship beside the exe (staged by `build.bat`), and
they need the VC runtime (present wherever VS or the VC redist is installed).

Keys: `SPACE`/`RIGHT` next (mode or preset), `LEFT` prev, `P` palette,
`N` new custom palette (color picker), `1`/`2`/`3` source (loopback/mic/demo),
`4` open audio file, `S` shuffle timer, `C` calm mode, `R` resolution target,
`L` MilkDrop preset folder, `M` MilkDrop engine, `G` GPU shader engine,
`F` fullscreen, `H` HUD, `ESC` quit. Settings and custom palettes persist to
`spectraviz.ini`.

Launch flags: `demo` (synthetic signal), `file=<path>` (play a file),
`milkdrop` / `shader` (boot into that engine), `wall` (borderless video wall
across every display — keys act on the focused window, `ESC` quits all).

projectM is built from `C:\projects\projectm` via `build-native.bat` there
(MSVC/Ninja, shared DLL, installed to `install-native/`). `build.bat` links
its import lib and stages the DLLs.

## Feature parity with the web version

The goal is 1:1 with WaveScope (the frozen web v2). Status of every web
feature:

| Web feature | Native status |
|---|---|
| 34 built-in modes (5 families) | done |
| 6 trace palettes | done |
| MilkDrop, raw `.milk` (projectM) | done (2 bundled presets so far) |
| System-audio capture | done (WASAPI loopback) |
| Fullscreen | done |
| HUD / keyboard control | done (persists over both engines; deck UI pending) |
| Demo signal source | done (key 3, or `SpectraViz demo`) |
| Microphone source | done (key 2) |
| Audio-file source | done (key 4, Media Foundation decode + play) |
| **Spotify (quick + PKCE)** | MISSING |
| Resolution targets (1080-8K) | done (key R; offscreen render + scale) |
| Shuffle timer | done for modes (key S); palette-shuffle + include list with the deck |
| Calm mode (photosensitivity) | done (key C) |
| Custom palette creation | done (N: pick 1-3 colors; full stop editor with deck) |
| Settings / preset persistence | done (spectraviz.ini beside the exe) |
| projectM preset library | done (scans a folder of .milk; L picks your archive) |
| GPU shader modes (plasma/tunnel/fractal) | done (G: real GLSL fragment shaders on the GL context) |
| Multi-monitor (per-display windows) | done (`wall` flag: independent visual per screen) |
| **On-screen control deck (auto-hiding)** | MISSING |
| Shortcuts / help overlay | done (F1 / ? keymap card; built-in-mode view) |
| Landing / marketing page | N/A (desktop app) |
| Reduced-motion fallback | N/A (desktop) |

## Roadmap (ordered for parity)

1. ~~**Audio input sources.**~~ DONE. Loopback (1), mic (2), demo (3), and
   audio file (4, Media Foundation decode + WASAPI render + analyze, loops;
   also `SpectraViz.exe file=<path>`). All four switchable at runtime.
2. ~~**Playback controls.**~~ DONE. Resolution targets (R: AUTO/1080/1440/
   4K/8K, rendered into an offscreen bitmap then scaled to the window),
   shuffle timer (S: off/15s/30s/1m/5m, hops modes or projectM presets), calm
   mode (C: excludes the 5 beat-burst modes + softens the beat envelope).
   *Palette-shuffle + the per-palette include list come with the deck (step
   7), since they need UI.*
3. ~~**Custom palettes + persistence.**~~ DONE. `N` builds a custom palette
   from up to 3 colors via the standard Windows color picker (cancel to stop
   early); it joins the `P` rotation. Settings (mode, palette, resolution,
   shuffle, calm) + custom palettes persist to `spectraviz.ini` beside the
   exe, restored on launch. *A richer color-stop editor arrives with the
   deck (step 7); source is intentionally not restored (starts on loopback).*
4. ~~**projectM preset library.**~~ DONE. Scans a folder of raw `.milk`
   files and cycles all of them (SPACE/arrows/shuffle); `L` picks your own
   archive folder; preset name + count shown in the title bar. (Native has
   no "upload/persist" step — the folder on disk *is* the library.)
5. ~~**GPU shader modes.**~~ DONE. `G` toggles a GPU shader engine
   (`src/shader_host.inl`): the three WebGPU shaders (plasma field, warp
   tunnel, fractal fold) ported 1:1 to GLSL 330 and compiled at runtime,
   drawn as a fullscreen triangle over its own WGL/OpenGL 3.3 child window —
   the same host pattern as projectM, so the bitmap-font HUD overlays it too.
   True per-pixel at window resolution, audio-uniform driven. `SPACE`/arrows
   cycle shaders, shuffle rotates them, `SpectraViz.exe shader` boots into it.
   Pulls in **no** new dependency (pure GPU + OS, unlike projectM's DLL).
6. ~~**Multi-monitor.**~~ DONE. `SpectraViz.exe wall` spans one borderless
   window across every display (`EnumDisplayMonitors`), each an independent
   `App` with its own engine/mode/palette/resolution and its own loopback tap
   (no cross-window streaming — one process, shared machine audio). It's a
   video wall: extra screens start on distinct visuals; focus a window to
   control it (keys act on the focused window); `ESC` on any window quits all.
   Without the flag it stays a single 1440×900 window as before. *Per-monitor
   config persistence is a later refinement — today the primary window's setup
   is what's saved.*
7. **On-screen control deck.** *Help overlay DONE* — `F1` / `?` shows a
   centered keymap card over the built-in modes. Still to do: an auto-hiding,
   mouse-driven instrument deck (source, engine, mode, preset, palette,
   resolution, shuffle) custom-drawn in the same pipeline, which also unlocks
   the palette-shuffle include list + full color-stop palette editor.
8. **Spotify.** WinHTTP PKCE with a localhost redirect for the connected
   one-button mode; capture stays via loopback.
9. **Polish.** Tray icon, launch-at-login / kiosk flag, preset hot-reload.

## Beyond parity (optional, not required for 1:1)

- **Built-in mini-player.** Once the audio-file source exists (decode + play),
  grow it into a small player: a queue, transport (play/pause/seek/prev/next),
  drag-drop, now-playing metadata on screen. Decode via Media Foundation
  (zero new deps; covers MP3/AAC/WAV/FLAC on Win10+); reach for the
  single-header **miniaudio** (MIT-0) only if MF's format coverage or
  ergonomics get annoying. Deliberately NOT a foobar2000-class library
  manager (that's a different product; the loopback source already lets any
  external player feed the visuals). foobar2000 itself is closed source, so
  it can't be embedded.
- **No built-in playback EQ.** Point EQ-wanting users at Equalizer APO
  (open source, system-wide) + the loopback source, which visualizes the
  post-EQ output for free. A *visual-only* band-tilt knob (weights the
  analysis bands without touching the audio) is the only EQ-ish thing worth
  considering here.

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
