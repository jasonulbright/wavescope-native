# Changelog

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
