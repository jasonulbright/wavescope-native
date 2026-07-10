# WaveScope native — parity matrix vs. the web app

Source of truth: `C:\projects\wavescope` (app/src/lib/viz/*). Status values:
**done** (verified matching), **partial**, **missing**, **N/A-desktop**.
"Plan" says what this pass builds. Updated as work lands.

## Analysis engine (must feel identical)

| Contract item | Status | Plan / note |
|---|---|---|
| FFT 2048 → 1024 bins, smoothing 0.8 | partial | Fix normalization (÷2048), Blackman window, smooth linear magnitudes pre-dB (FINDINGS P2) |
| Waveform 2048 samples | done | float −1..1 (web bytes; same information) |
| Bands by Hz from real device rate (20-250 / 250-4k / 4k-16k) | done | floor/ceil bin edges match `audio-engine.ts` |
| Level: RMS ¼-decimation, smooth 0.2 | done | |
| Beat: avg 0.04, ×1.35 gate, 0.08 floor, e^−6dt decay | done | |
| Per-frame payload {fft, wave, level, bass, mid, treble, beat, t, dt} | done | `AudioFrame` |
| Silence behaves (decay, not freeze) | missing | Feed zeros on stalled loopback (FINDINGS P3) |

## Engines

| Feature | Status | Plan / note |
|---|---|---|
| Built-in: 34 modes, 5 families (9/6/6/6/7) | done | Math, fade, flash flags spot-verified 1:1 against modes/*.ts |
| Per-window mode state (companions don't share pools) | missing | Replace function statics with per-view state (FINDINGS C4) |
| GPU engine: 17 shader modes | partial (3/17) | Port remaining 14 WGSL bodies to GLSL 330 |
| GPU uniforms: res/time/level/bass/mid/treble/beat | done | |
| GPU FFT texture (1024×r8) + waveform texture (2048×r8), `spec()/specLog()/wav()` | missing | Add two 1-D textures updated per frame + prelude helpers |
| projectM: real .milk via native libprojectM | done | DLL pinned beside exe |
| 13 bundled presets (same files as web) | partial (2) | Copy the 13 from `app/public/presets-milk/` |
| User preset folder (library) + hot reload | done | Exceeds web (folder watch) |
| Preset blend on switch | partial | Native hard-cuts to dodge a white flash in soft-cut; investigate and restore 2.7 s blend if clean |
| Butterchurn as separate engine | N/A-desktop | Native projectM covers MilkDrop |
| Demo signal per spec (kick 500 ms, retune 8 bars, A2/C3/G2/D3, LFO detune) | partial | Retune cadence + kick sweep + detune (FINDINGS P4) |

## Sources

| Feature | Status | Plan / note |
|---|---|---|
| WASAPI loopback (silent, no picker) | done | Superior to web `getDisplayMedia`; keep |
| Microphone | done | raw (no AGC/EC — WASAPI default matches web constraints) |
| Audio file (audible, looped) | done | MF decode → WASAPI render; fix same-source re-pick (C5) |
| Demo | done | |
| Source-ended / device-change handling | partial | IMMNotificationClient on default-device change (C6) |

## Spotify

| Feature | Status | Plan / note |
|---|---|---|
| Auth Code + PKCE, no secret, client ID in ini | done | Hardening in FINDINGS S1-S5 |
| Redirect 127.0.0.1:8888/callback | done | |
| Scopes user-modify/read-playback-state | done | + user-read-currently-playing for the HUD line |
| Play w/ device transfer fallback, 403 → Premium msg | done | Uses transfer-with-play (equivalent to spec's play→404→transfer) |
| Now-playing in HUD | missing | Poll currently-playing while authed; show track — artist |
| Tokens DPAPI-protected | done | Already implemented (Phase 5.1 satisfied) |

## Console / UX

| Feature | Status | Plan / note |
|---|---|---|
| 6 palettes (Ultramarine default … Magma) | done | Colors identical to palettes.ts |
| Custom palettes (stops editor, include list) | done | N panel |
| Shuffle off/15s/30s/1m/5m | done | |
| Shuffle **DJ** (hops engine + visual) | missing | Add; doubles as Phase-5 Auto-DJ |
| Calm mode (no bursts, softened beat) | done | flash-flagged modes excluded; beat ×0.2 |
| Resolution targets auto/1080/1440/2160/4320 | done | Offscreen render + scale |
| Fullscreen | done | |
| Auto-hiding deck (3 s idle) | done | idle timing aligned to 3 s |
| Monospace HUD: SRC/MODE/RES/FPS/SHUFFLE/REC/now-playing | partial | Add SHUFFLE, REC, now-playing lines |
| Shortcuts: Space/→ ← P S C F H ? Esc | partial | Realign: M=displays, R=record; engine keys move (see README); Esc restores before quitting |
| Help overlay | done | F1 / ? |
| Design language (instrument black, ultramarine, monospace) | done | Consolas (system) as the monospace readout — no font embedding, no license risk |

## Persistence

| Item | Status | Plan / note |
|---|---|---|
| Mode/palette/resolution/shuffle/calm | done | wavescope.ini |
| Custom palettes + include list | done | |
| Spotify auth (DPAPI blob) + client ID | done | |
| Engine (+ MILK library folder) | missing | Add |
| Window placement | missing | Add |

## Desktop-only (Phase 5)

| Feature | Status | Plan / note |
|---|---|---|
| DPAPI token storage | done | Verified in review |
| Clip recording → MP4 (MF H.264 + AAC), hotkey R | missing | Render-target capture + analysed-audio mix; `wavescope-<timestamp>.mp4` (brief says `spectraviz-`; that predates the product rename — deviating deliberately, flagged for the owner) |
| Multi-monitor companion windows, one engine | partial | `wall` exists but runs N independent taps; rework: one tap drives every window, M/displays menu spawns companions |
| Tray icon + minimize-to-tray | done | |
| Always-on-top toggle | missing | Add (tray menu + key) |
| Launch at login (HKCU Run, off by default) | done | install-startup / remove-startup |
| Auto-DJ | missing | = DJ shuffle option |
| Spout2 sender (OBS texture share) | ASK OWNER | Third-party dep; brief requires explicit approval — not built |

## Explicitly N/A on desktop

PWA/service worker; `?embed=1` (superseded by always-on-top / future Spout);
Butterchurn engine; browser capability gating; landing/marketing page;
reduced-motion fallback (calm mode covers the intent).
