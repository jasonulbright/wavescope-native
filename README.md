# WaveScope (native)

A music visualizer built like an instrument, and built like a classic
Windows app: one C++ source tree, one statically linked exe, OS facilities
everywhere they exist, and exactly two deliberate dependencies — libprojectM
for real MilkDrop and the Spout SDK for OBS texture sharing, both pinned.
The audit surface is this repo, Windows, and those two pins.

This is the native successor to the WaveScope web app (live at
https://wavescope.signalridgelabs.com, source at
github.com/jasonulbright/wavescope). It matches the web app feature for
feature — same analysis math, same 34 modes, same 17 GPU shaders, same
palettes, the same 15 `.milk` presets plus the full 551-preset classic
MilkDrop library in original form — and adds the things only a desktop app
can do: silent WASAPI loopback, MP4 clip recording, companion displays from
one engine, Spout output for OBS, always-on-top, tray, launch at login.

## Build

```
build.bat
```

Requires Visual Studio 2026 (any edition) and the projectM native build at
`C:\projects\projectm\install-native` (built there via its
`build-native.bat`; MSVC/Ninja, shared DLL). The batch file:

1. builds and **runs the unit tests** (`src/tests.cpp`) — a red test fails
   the build;
2. builds `WaveScope.exe` (`/MT`, no MSVC redistributable needed for the
   core app);
3. stages `projectM-4.dll` + `projectM-4-playlist.dll` beside the exe
   (these two need the VC runtime, present wherever VS or the VC redist is).

## Verify

```
WaveScope.exe selftest
```

Renders every engine on the demo signal and asserts real output: sweeps all
34 modes and all 17 shaders (17/17 must compile), records a 1.6 s MP4 and
re-opens/decodes it, does a Spout roundtrip through an in-process receiver,
sweeps a spread of the 551 classic presets through the ns-eel parser, dumps
PNGs of what actually rendered beside the exe, writes
`wavescope-selftest.log`, and exits nonzero on any failure. Run it after
every change; CI-in-a-flag. Selftest never writes `wavescope.ini` — your
engine/mode/palette survive it.

## The three engines

| Engine | What it is |
|---|---|
| **WaveScope** | 34 built-in modes in five families (spectrum 9, waveform 6, particles 6, geometry 6, field 7), Direct2D, faithful ports of the web canvas math, phosphor-trail retained rendering. |
| **GPU** | All 17 web WGSL shader modes ported 1:1 to GLSL 330 (Plasma field … Scope lines), compiled at runtime on a WGL context. Audio uniforms plus per-frame FFT (1024×R8) and waveform (2048×R8) textures with `spec()/specLog()/wav()`. |
| **projectM / MILK** | Real MilkDrop via native libprojectM. PROJM plays the 15 bundled WaveScope presets (same files as the web app). MILK plays the **original classic library — 551 presets from the official MilkDrop distribution** (`presets-classic\`, with the shared texture pack in `textures\`) — or your own `.milk` folder (`L` to pick, hot-reloads on change). These are the true originals of the ~60 converted classics the web's Butterchurn engine ships, running on the real ns-eel VM. |

## Sources

`1` system audio (WASAPI loopback — hears whatever the machine plays,
silently, no permission dialogs), `2` microphone, `3` demo signal
(oscillator bank + synthetic kick, analysis-only, matches the web spec),
`4` audio file (Media Foundation decode, audible, looped). The capture
survives device unplugs and follows Windows default-device changes; silence
decays to dark instead of freezing.

Analysis matches the web app's Web Audio analyser exactly — Blackman
window, |X|/N, 0.8 smoothing on linear magnitudes, [-100,-30] dB mapping,
Hz-based bands (250 Hz / 4 kHz splits) from the real device sample rate,
quarter-decimated RMS level, bass-flux beat envelope (1.35× gate, 0.08
floor, e^-6dt decay). Same music, same numbers, both apps.

## Keys

| Key | Action |
|---|---|
| `SPACE` / `→` | next mode / preset / shader |
| `←` | previous |
| `E` | cycle engine (WaveScope → GPU → projectM) |
| `G` | GPU shader engine |
| `P` | cycle trace palette |
| `N` | palettes + shuffle panel (include list, 3-stop editor) |
| `1 2 3 4` | source: system / mic / demo / file picker |
| `M` | displays menu (move to a monitor, ALL DISPLAYS companions) |
| `R` | record clip → `wavescope-<timestamp>.mp4` (H.264 + AAC) |
| `O` | Spout output — OBS/Resolume ingest the visual as sender "WaveScope" |
| `B` | Spotify: authorize once + start playback (Premium) |
| `S` | shuffle timer (off / 15s / 30s / 1m / 5m / **DJ**) |
| `C` | calm mode (photosensitive-safe: no burst modes, softened beat) |
| `L` | MilkDrop preset folder |
| `T` | always on top |
| `F` | fullscreen — `H` HUD — `F1` / `?` help |
| `ESC` | close panel / menu / help / fullscreen, then quit |

Resolution targets (AUTO / 1080 / 1440 / 4K / 8K) live in the deck's
`RES ▾` dropdown; they drive the built-in modes' offscreen render. The GL
engines (GPU / projectM) always render at window size — fullscreen is
already native resolution — and the HUD's RES line reports the pixels
actually rendered either way. The deck reveals when the mouse nears the
bottom edge and hides after 3 s idle; under GPU/projectM the trace-palette
controls hide (they color the built-in modes), web-style. DJ shuffle hops
engine AND visual every 30 s.

## Recording

`R` (or the deck's `rec (R)` button) starts/stops an MP4 beside the exe:
hardware H.264 where available plus AAC of the analysed audio, via Media
Foundation (no new dependencies). It captures the engine's render target
*before* the UI, at the current render resolution (built-ins) or window
size (GL engines), clamped to 4K — H.264 encoders stop near 4096 px a
side, so an 8K-target take lands as a 3840-wide file instead of failing.
Switching engine or resolution mid-take finalizes the file cleanly.
`● REC mm:ss` shows in the HUD.

## OBS / Spout

`O` publishes the pre-UI visual as a Spout shared texture named
**WaveScope** — in OBS add a *Spout2 Capture* source (obs-spout2-plugin)
and pick it; Resolume/TouchDesigner see it the same way. This is the
desktop analog of the web app's `?embed=1` browser source, without the
browser. The state persists across launches. Implementation is the
vendored, pinned Spout SDK subset in `third_party/spout` (BSD-2-Clause;
see `PIN.md` there) — the frame feed reuses the recorder's capture path,
so the readback cost is only paid while the output is on. `selftest`
verifies the sender end-to-end with an in-process Spout receiver doing a
pixel roundtrip.

## Displays

`M` opens the displays menu: move the window to any monitor, or
**ALL DISPLAYS** to open a borderless companion on every other screen — all
driven by the one audio engine (one capture, one analysis; the web app
needs BroadcastChannel + PCM streaming for this). Each companion keeps its
own engine/mode/palette: focus it and use the normal keys; `Esc` closes
just that companion. `WaveScope.exe wall` boots straight into that state;
`kiosk` is a single borderless fullscreen with idle cursor-hide.

## Spotify

`B` runs OAuth2 Authorization Code + PKCE (no client secret; the client ID
lives in `wavescope.ini`, yours to change). The loopback listener binds
127.0.0.1:8888 exclusively *before* the browser opens, validates the CSRF
`state`, and never echoes attacker-controlled input. Tokens travel over
WinHTTP (schannel); the refresh token is DPAPI-encrypted at rest. After
connecting, playback starts on your device (Premium required for control —
capture of already-playing audio needs nothing) and the HUD shows
now-playing (`♪ track — artist`), silently resumed on later launches.

## Persistence

`wavescope.ini` beside the exe: mode, engine (incl. MILK library folder),
palette + custom palettes, resolution, shuffle (incl. include-list), calm,
window placement, Spotify client ID + DPAPI-wrapped refresh token. The
primary window's setup is what persists; source intentionally resets to
loopback each launch.

## Launch flags

`demo` · `file=<path>` · `milkdrop` · `shader` · `wall` · `kiosk` ·
`install-startup` / `remove-startup` (HKCU Run key, off by default) ·
`selftest`. Tray icon: right-click for Show/Hide, Always on top, Quit;
minimize hides to tray (and rendering fully stops while hidden).

## Source map

- `src/analysis.inl` — the pure math shared with the tests: `AudioFrame`,
  HSL, `BinLog`, radix-2 FFT, `AnalyzeWindow` (the analyser contract).
- `src/main.cpp` — infrastructure: `AudioTap` (WASAPI loopback/mic thread,
  ring buffer, file playback, device-change + silence handling, per-consumer
  `ReadNew` cursors), `App` (window, D2D, deck/HUD/panel UI, engines,
  persistence), companion windows, tray, message loop.
- `src/viz_modes.inl` — palettes, `DrawCtx`, `VizState` (per-window mode
  state), all 34 modes.
- `src/shader_host.inl` — the GPU engine: WGL host, 17 GLSL ports, audio
  textures.
- `src/projectm_host.inl` — the MilkDrop engine host: WGL host, libprojectM,
  preset scan/sort/hot-reload.
- `src/spotify.inl` — PKCE flow, loopback listener, WinHTTP, DPAPI tokens,
  now-playing poller.
- `src/record.inl` — MP4 clip recorder (Media Foundation SinkWriter).
- `src/spout_out.inl` — Spout output wrapper (`third_party/spout`).
- `src/ui_paint.inl` — the abstract UI painter (one UI, every engine).
- `src/selftest.inl` — the `selftest` harness.
- `src/tests.cpp` — unit tests for the pure math (run by `build.bat`).

## Dependencies

See `DEPENDENCIES.md` for the full decision table. Short version: OS
facilities everywhere they exist (WinHTTP, DPAPI, bcrypt, Media Foundation,
WASAPI, D2D/DirectWrite/WIC, WGL), two vendored dependencies (libprojectM
4.x, LGPL-2.1, pinned, built from `C:\projects\projectm`; the Spout SDK's
DX subset, BSD-2-Clause, pinned in-tree), and small hand-rolled code only
where it is boring and testable (ini, flat JSON reads, base64url) — with
the FFT and analysis pinned by unit tests.

## Licensing

[MIT](LICENSE) © 2026 Signal Ridge Labs — same as the web app. Third-party:

- **libprojectM 4.x** — LGPL-2.1, satisfied by dynamic linking
  (`projectM-4.dll` ships beside the exe, never statically linked).
- **Spout SDK (DX subset)** — BSD-2-Clause, vendored verbatim with its
  license at `third_party/spout/LICENSE.txt` (pin in `PIN.md` there).
- **MilkDrop preset + texture packs** — community-distributed content,
  redistributed from the projectM-visualizer org (`presets-classic/` and
  `textures/`, provenance and pins in `DEPENDENCIES.md`; the pack's own
  README ships alongside).

## Project docs

`FINDINGS.md` records the adversarial review this codebase went through;
`PARITY.md` tracks the feature matrix against the web app;
`DEPENDENCIES.md` is the dependency decision table;
`DEMO-CHECKLIST.md` is the 5-minute walkthrough script.
