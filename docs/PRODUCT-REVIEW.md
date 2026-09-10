# Product review — 2026-09-10

Reviewed the current C++ source and exercised the projectM picker, preset lab,
GPU controls and window lifecycle on a Windows desktop. This follow-up covers
discoverability and everyday usability; the broader security review and its
limits are in [ADVERSARIAL-REVIEW.md](ADVERSARIAL-REVIEW.md).

## Fixes in the current source

These changes are unreleased; v1.2.0 remains the published release.

| Problem | Result |
| --- | --- |
| Controls were invisible on launch and required discovering the bottom-edge gesture. | The HUD and deck appear for 15 seconds or until a keypress, with a hint for the gesture, Help and Options. |
| Turning the HUD back on after idle did not show it. | Re-enabling it restarts the visibility timer. |
| Removing the active preset during a library rescan left a stale picker index; opening the lab could access outside the list or edit another file. | The playing preset retains its full file path independently of the list. Removed rows detach, navigation recovers, and an unavailable lab source gets a visible message. |
| Choosing an unreadable or rejected preset updated the displayed name and editor selection before the engine accepted it. | The label and source change only after a successful load. |

The three new HUD/deletion/rejection checks failed before the fixes and passed
afterward. The full desktop selftest also passed: 34 built-in modes, 17 shaders,
projectM and classic presets, MP4 encode/decode, Spout, morphing and lab tests.
The deletion test avoids dereferencing the known stale index in an unfixed
build, so it reports the error without depending on a crash.

## First four priorities implemented

Implemented in order, with one commit per feature. These remain unreleased.

| Priority | Result | Validation |
| --- | --- | --- |
| 1 | F3 native preset browser: live filename search, favorites, favorites-only browsing and shuffle. Bundled/user favorites retain identity when moving a portable folder. | Native filtering, empty results, favorite persistence, filtered shuffle and empty-pool behavior. |
| 2 | Saved deck pin, Tab/arrow/Enter navigation, and F4 native controls with standard Windows names and keyboard access. | Visibility after timeout, keyboard action isolation, native controls and rendered layout. |
| 3 | Resizable native rich-edit lab: selection, clipboard, undo/redo, Unicode, scrolling, modified indicator and save/discard/cancel on close or exit. | Selection replacement, Unicode, undo/redo, cancel, failed-save preservation, atomic Save As and native screenshot. |
| 4 | F5 audio player: filename, elapsed/total time, pause, seek while paused, loop and replay. Ctrl+Space controls playback; Space keeps advancing visuals. | Real Media Foundation/WASAPI playback of a silent WAV: paused clock, seek, EOF drain/stop, replay, loop, shortcuts, load failure and recovery. |

All four also run through the full application selftest covering the rendering
engines, recording, Spout, preset loading and the editor. Windows supplies native
clipboard behavior; the automated editor test does not replace the user's clipboard.

## Remaining recommendations

| Priority | Addition | Why it matters / useful first scope |
| --- | --- | --- |
| 5 | **Audio device and signal controls** | Capture follows Windows' default endpoint, with no device picker or input-level control. A device selector, signal meter and gain/sensitivity setting would help microphone users and multi-interface setups. |
| 6 | **Recording feedback and destination controls** | The recording indicator shares the fading HUD. Add a persistent recording badge, clear stop/failure feedback, a destination picker and an Open recordings folder action. |

## README and screenshots

- Put installation, first-launch help and app usage ahead of developer steps.
- Explain silent demo mode, the bottom-edge gesture, Help/Options, and tray restore.
- Distinguish the unreleased startup hint from the behavior in v1.2.0.
- Lead with `wavescope-nebula.milk`; include the updated lab and a GPU Warp
  tunnel example. Capture from the native app without altering the visuals.
- Clarify that library changes refresh the picker; external edits to the
  playing file require selecting it again, while the lab can apply immediately.
