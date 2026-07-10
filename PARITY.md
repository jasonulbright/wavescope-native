# WaveScope native — parity matrix vs. the web app

Source of truth: `C:\projects\wavescope` (app/src/lib/viz/*). Status values:
**done** (verified), **N/A-desktop**, **ASK OWNER**. Verification = unit
tests (`build.bat` runs them) + `WaveScope.exe selftest` + manual
walkthrough (`DEMO-CHECKLIST.md`).

## Analysis engine (must feel identical)

| Contract item | Status | Note |
|---|---|---|
| FFT 2048 → 1024 bins, smoothing 0.8 | done | Blackman, \|X\|/N, smoothing on linear magnitudes pre-dB — the exact analyser recipe; unit-tested |
| Waveform 2048 samples | done | float −1..1 |
| Bands by Hz from real device rate (20-250 / 250-4k / 4k-16k) | done | unit-tested at 48 k and 44.1 k |
| Level: RMS ¼-decimation, smooth 0.2 | done | unit-tested |
| Beat: avg 0.04, ×1.35 gate, 0.08 floor, e^−6dt decay | done | unit-tested (charge + decay slope) |
| Per-frame payload {fft, wave, level, bass, mid, treble, beat, t, dt} | done | `AudioFrame` |
| Silence behaves (decay, not freeze) | done | zeros fed on stalled loopback |

## Engines

| Feature | Status | Note |
|---|---|---|
| Built-in: 34 modes, 5 families (9/6/6/6/7) | done | math/fade/flash verified vs modes/*.ts; registry unit-tested |
| Per-window mode state | done | `VizState`, one per window |
| GPU engine: 17 shader modes | done | all WGSL bodies ported to GLSL 330; selftest asserts 17/17 compile |
| GPU FFT texture (1024×r8) + waveform texture (2048×r8), `spec()/specLog()/wav()` | done | selftest captures both texture-fed shaders |
| projectM: real .milk via native libprojectM | done | |
| Bundled presets (same files as web) | done | all 15 from `presets-milk/`; opens on WaveScope Tunnel like the web (not the alphabetical test fixtures) |
| Classic MilkDrop library (web: ~62 Butterchurn conversions) | done, exceeded | MILK ships the 551-preset ORIGINAL MilkDrop distribution pack + shared textures (presets-classic\ + textures\, pinned — see DEPENDENCIES.md); selftest sweeps 12 through the parser |
| User preset folder (library) + hot reload | done | exceeds web |
| Preset blend on switch | done* | deliberate hard cut: projectM's soft-cut blends before the incoming preset's first frame and flashes white; the clean cut is the better feel. Documented deviation. |
| Butterchurn as separate engine | N/A-desktop | native projectM covers MilkDrop |
| Demo signal per spec (kick 500 ms sweep 120→40, retune 8 kicks, A2/C3/G2/D3, LFO detune) | done | |

## Sources

| Feature | Status | Note |
|---|---|---|
| WASAPI loopback (silent, no picker) | done | superior to web `getDisplayMedia`; kept |
| Microphone | done | |
| Audio file (audible, looped) | done | re-pick switches tracks |
| Demo | done | |
| Source-ended / device-change handling | done | IMMNotificationClient + endpoint-failure reopen |

## Spotify

| Feature | Status | Note |
|---|---|---|
| Auth Code + PKCE, no secret, client ID in ini | done | listener hardened (bind-first, exclusive, state-validating accept loop) |
| Redirect 127.0.0.1:8888/callback | done | |
| Scopes + playback start w/ device transfer, 403 → Premium msg | done | |
| Now-playing in HUD | done | 10 s poller, silent resume at launch |
| Tokens DPAPI-protected | done | |

## Console / UX

| Feature | Status | Note |
|---|---|---|
| 6 palettes | done | curves unit-tested |
| Custom palettes (stops editor, include list) | done | |
| Shuffle off/15s/30s/1m/5m/**DJ** | done | DJ hops engine + visual (= Auto-DJ) |
| Calm mode | done | flash modes excluded, beat ×0.2 |
| Resolution targets auto/1080/1440/2160/4320 | done* | built-in modes render at the target (offscreen); the GL engines (GPU/projectM) render at window size — a deliberate deviation: fullscreen is already native-res, and an FBO indirection buys only supersampling. The HUD shows the true pixels either way. |
| Fullscreen | done | |
| Auto-hiding deck (3 s idle) | done | two-cluster wrap like the web; right cluster carries calm (C) / palettes + shuffle / displays / rec (R) / fullscreen (F) / ?; TRACE, calm and palettes hide under GPU/projectM (web behavior — keys still work) |
| HUD: SRC/MODE/RES/FPS/SHUFFLE/REC/now-playing | done | RES = actual rendered pixels, like the web |
| Shortcuts: Space/→ ← F H P S C, M=displays, R=record, ?, Esc=restore-then-quit | done | + native E/G/L/T/B/N (documented in README) |
| Help overlay | done | F1 / ? |
| Design language (instrument black, ultramarine, monospace readouts) | done | system Consolas — no font embedding |

## Persistence

| Item | Status |
|---|---|
| Mode/palette/resolution/shuffle/calm | done |
| Custom palettes + include list | done |
| Spotify auth (DPAPI) + client ID | done |
| Engine + MILK library folder | done |
| Window placement (validated against live monitors) | done |

## Desktop-only (Phase 5)

| Feature | Status | Note |
|---|---|---|
| DPAPI token storage | done | |
| Clip recording → MP4 (MF H.264 + AAC), hotkey R + deck button | done | selftest records + re-decodes a file; `wavescope-<timestamp>.mp4` (brief said `spectraviz-`; that predates the product rename — deliberate deviation, flagged). Recording clamps to 4K — H.264 encoders stop near 4096 px, so an 8K-target take lands as 3840-wide (verified). |
| Multi-monitor companions, one engine | done | one tap + one analysis for every window; companions vsync-off so windows don't stack vblank waits |
| Tray icon + minimize-to-tray | done | hidden windows stop rendering entirely |
| Always-on-top toggle | done | T + tray menu |
| Launch at login (HKCU Run, off by default) | done | |
| Auto-DJ | done | = DJ shuffle |
| Spout2 sender (OBS texture share) | done | owner-approved; O key, sender "WaveScope", vendored pinned SDK subset (third_party/spout), selftest does an in-process receiver roundtrip |

## Explicitly N/A on desktop

PWA/service worker; `?embed=1` (always-on-top covers the overlay use; the
Spout sender is the OBS answer); Butterchurn engine; browser capability
gating; landing/marketing page; reduced-motion fallback (calm mode covers
the intent).
