# Changelog

## 1.2.1 — 2026-09-10

- Added an audio-file player (F5) with filename, elapsed/total time, pause,
  seeking, replay and a loop toggle. Ctrl+Space pauses/resumes; Space keeps
  advancing visuals. Paused files feed silence to the visual and recorder.
- File playback now drains buffered audio before stopping or looping, displays
  decode/output errors, and isolates each load's state when switching files.
- Replaced the preset lab's custom text field with a resizable native editor:
  selection, copy/cut/paste, undo/redo, Unicode and horizontal/vertical scrolling.
  Modified text gets an asterisk and a save/discard/cancel prompt on close or
  exit; failed saves keep the editor open. Shuffle pauses while editing.
- Added saved pinned controls, Tab/arrow/Enter navigation on the deck and
  menus, and an accessible native Controls window (F4).
- Added a native preset browser (F3) with live search, saved favorites,
  favorites-only filtering and a favorites-only shuffle pool.
- Show the status display and bottom controls for 15 seconds on launch, or
  until the first keypress. Added an on-screen hint for finding controls, help
  and Options. Mouse activity and open menus retain their normal behavior.
- Fixed re-enabling the HUD after idle leaving it invisible until mouse movement.
- Fixed preset-library rescans leaving an invalid selection after the active
  file was removed. The lab reads the playing preset's path independently of
  the picker and reports an unavailable file instead of indexing a removed row.
- Failed preset loads now preserve the playing preset's label and editor source;
  next/previous navigation skips unreadable or rejected presets.
- Reorganized the README around first launch and app usage, refreshed screenshots,
  and documented the next product priorities.

## 1.2.0 — 2026-09-10

- Added a signed per-user Windows MSI installer with a Start menu shortcut,
  repair, upgrades, downgrade protection and standard uninstall support.
- Clearly named both downloads: `-installer-win64.msi` and `-portable-win64.zip`,
  each with a SHA-256 checksum. Both contain the same signed app/projectM binaries.
- Installed settings and saved presets now use `%LOCALAPPDATA%\WaveScope`;
  recordings use `Videos\WaveScope`. Uninstall preserves user files. Portable
  copies retain folder-local settings, presets and recordings.
- Fixed preset lab Save As silently overwriting existing files. Completed
  writes are now published atomically without replacement; invalid/reserved
  filenames and write failures are reported. Added concurrent-save regressions.
- Fixed single-page dropdowns leaking navigation and audio-source shortcuts
  to the visualizer behind them; added a reproducing GUI regression test.
- Added installer lifecycle tests and documented portable-to-installed migration.

## 1.1.1 — 2026-09-09

First signed Windows release, with GUI fixes and the repository's first
adversarial review of application code and release automation.

- Native Spotify Options dialog (F2); removed the bundled client ID. Old login
  tokens are cleared when the ID changes or the user disconnects.
- Hardened OAuth callback parsing, bounded local/network reads, serialized
  refresh requests, nonmodal worker errors, and correct Unicode/JSON metadata.
- Fixed recorder crashes after automatic stop and rapid recording filename collisions.
- Fixed the lab opening shortcut modifying preset text and stale editor overlays.
- Added palette pagination, modal input isolation, long-label clipping, narrow
  deck wrapping, minimum window dimensions, and reliable slider capture.
- Selftest runs without loading or writing personal configuration or Spotify tokens.
- Repaired packaging; stage tracked assets only and produce SHA-256 checksums.
- Added pinned Windows CI and Azure OIDC signing with signer/timestamp verification
  and draft releases. Added Windows file version metadata and compiler hardening.

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
