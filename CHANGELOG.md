# Changelog

## 1.1.0 — 2026-07-12

MilkDrop deck true-up: morph transitions, a live preset lab, and audio-driven
VJ shuffle.

- Morph deck (projectM engine): a MORPH row with a target dropdown (shuffle,
  or any preset in the library), a 0.5–10 s blend slider, and a morph button
  that cross-fades between presets using the engine's soft cut. The blend
  length persists in `wavescope.ini`.
- Preset lab (`L`): a modal editor over the active preset's full `.milk`
  source. Apply (Ctrl+Enter) recompiles live against the signal; a rejected
  edit keeps the running preset and shows the engine's message (with a line
  and column when it has one). Save writes to `presets\user\`.
- Auto-VJ (`S` → VJ): audio-driven shuffle instead of a timer — a bass drop
  hard-cuts to another visual, a long lull gently morphs. Applies to whichever
  engine is active.
- Timed shuffle now blends projectM preset hops over the morph slider length
  instead of hard-cutting; DJ still hops the engine, VJ keeps its own design.
- `L` now opens the lab (it used to open the MilkDrop preset folder); point
  MILK at your own `.milk` archive by clicking the `milkdrop` deck button
  again.

## 1.0.0 — 2026-07-12

First public release.

- Three engines: 34 built-in Direct2D modes, all 17 GPU shader modes, and
  real MilkDrop via libprojectM — the 15 WaveScope presets plus the
  551-preset classic library with the shared texture pack.
- Sources: silent WASAPI system loopback, microphone, demo signal, audio
  file (Media Foundation, looped).
- MP4 clip recording (hardware H.264 + AAC), Spout output for
  OBS/Resolume/TouchDesigner, multi-monitor companion displays, always on
  top, tray, kiosk and video-wall launch modes, DJ shuffle, calm mode.
- Spotify one-button sign-in (browser PKCE flow, no client secret; the
  stored login is encrypted to your Windows user account).
- Portable by design: settings live in `wavescope.ini` beside the exe; no
  installer, no registry writes except the opt-in launch-at-login key.
- `WaveScope.exe selftest` verifies every engine end to end (renders,
  records, decodes, Spout roundtrip); `build.bat` runs the unit tests as a
  build gate.
