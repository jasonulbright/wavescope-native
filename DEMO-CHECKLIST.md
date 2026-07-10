# WaveScope — 5-minute demo checklist

Two people, one machine, ~5 minutes. Every step lists the expected result;
anything else is a bug worth filing. Before starting: `build.bat` (tests
must print `tests: ALL PASS`), then `WaveScope.exe selftest` (must print
`ALL PASS` in `wavescope-selftest.log`).

## 1. Launch + sources (60 s)

- [ ] Start `WaveScope.exe` with music playing (Spotify/browser/anything).
      → Window opens where you last left it, HUD top-left shows
      `SRC SYSTEM AUDIO`, visuals move with the music. No permission dialogs.
- [ ] Pause the music. → The picture decays to dark within ~2 s (no frozen
      bars). Resume: it comes back.
- [ ] Press `3` (demo). → Motion continues on the synthetic chord+kick, beat
      pulses roughly twice a second.
- [ ] Press `2` (mic) and speak. → Visuals follow your voice.
- [ ] Press `4`, pick an MP3/FLAC. → It plays audibly and drives the
      visuals; pick a different file the same way → the track switches.
- [ ] Press `1` to return to system audio.

## 2. Engines (60 s)

- [ ] Press `SPACE` a few times. → Modes advance, names change in the HUD
      (`MODE …  (nn/34)`).
- [ ] Press `E`. → GPU engine (HUD `GPU …`). `SPACE` through a few shaders —
      Spectrum bars and Scope lines must clearly follow the FFT/waveform.
- [ ] Press `E` again. → projectM (HUD `PROJM …`), real MilkDrop preset
      running. `SPACE` cycles presets with no white flash.
- [ ] Mouse to the bottom edge. → Deck appears; click `milkdrop`, then the
      picker dropdown → the CLASSIC library (551 originals — Geiss, Flexi,
      Martin, Aderrasi…) in columns; pick one. Deck hides ~3 s after the
      mouse leaves it. Click `projectM` → picker shows the 15 WaveScope
      presets again.
- [ ] Press `E` once more → back to WaveScope modes. The deck + HUD look
      identical over every engine.

## 3. Palettes, shuffle, calm (45 s)

- [ ] Press `P` twice. → Trace palette changes (HUD unchanged, colors swap).
- [ ] Press `N`. → PALETTES + SHUFFLE panel. Click a swatch, pick a color,
      name it, `add palette` → it becomes active and appears in the list.
      `ESC` closes the panel.
- [ ] Press `S` until the HUD shows `SHUF DJ`. → Within 30 s the app hops to
      a different ENGINE and visual on its own. Press `S` until `SHUF` line
      disappears (off).
- [ ] Press `C`. → HUD shows `CALM`; burst modes (Fountain, Tunnel, Shards…)
      vanish from the picker and cycling; the beat softens. Press `C` again.

## 4. Record a clip (45 s)

- [ ] Press `R`, let it run ~10 s, press `R` again. → HUD showed
      `● REC mm:ss`; a `wavescope-<timestamp>.mp4` appears beside the exe.
- [ ] Open the file in Media Player. → Correct orientation, matches what was
      on screen (without the UI), audio in sync.

## 4b. OBS via Spout (optional, 30 s — needs OBS + obs-spout2-plugin)

- [ ] Press `O`. → HUD shows `SPOUT OUT`. In OBS, add a *Spout2 Capture*
      source → sender "WaveScope" appears; the live visual (without the
      UI) streams into OBS.
- [ ] Press `O` again. → HUD line clears, the OBS source goes idle.

## 5. Displays + window management (45 s)

- [ ] Press `M`, pick another `DISPLAY n` (if present). → The window jumps
      there fullscreen-borderless. `F` restores.
- [ ] Press `M` → `ALL DISPLAYS` (multi-monitor machines). → Borderless
      companions open on the other screens with different visuals, all in
      sync with the same audio. Focus a companion: keys drive it alone.
      `Esc` on the companion closes only it; `M` → `CLOSE EXTRA DISPLAYS`
      also works.
- [ ] Press `T`. → Window stays over other apps. `T` again to release.
- [ ] Minimize. → Window hides to the tray; CPU/GPU drop to ~zero (check
      Task Manager if curious). Double-click the tray icon to bring it back.

## 6. Spotify (30 s, needs a Spotify account)

- [ ] Press `B`. → Browser opens Spotify authorize once; after approving,
      the tab says "WaveScope connected". Playback starts on your device
      (Premium) or a clear message explains what's needed. HUD gains
      `♪ track — artist` within ~10 s.
- [ ] Quit and relaunch WaveScope with Spotify still playing. → Now-playing
      returns on its own (no browser) — the token survived, DPAPI-encrypted.

## 7. Abuse it (60 s)

- [ ] Unplug the audio device / switch Windows default output mid-run.
      → No crash; capture follows to the new default within a second or two.
- [ ] Change display scale (Settings → Display → 125%) while running.
      → Window stays crisp, UI scales, clicks still land on the right
      buttons.
- [ ] Drag-resize the window wildly; `F` in and out of fullscreen. → No
      flicker of the deck layout, no stuck resize cursor, trails keep
      working.
- [ ] Drop a new `.milk` file into `presets\` while the projectM engine is
      showing. → It appears in the picker within a second (hot reload).
- [ ] Close the app, relaunch. → Same window position, same engine, same
      mode/palette/resolution/shuffle/calm as you left them.
