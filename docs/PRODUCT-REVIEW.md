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

## Recommended feature order

These are design recommendations based on the implementation and GUI review,
not a commitment to ship every item in the next release.

| Priority | Addition | Why it matters / useful first scope |
| --- | --- | --- |
| 1 | **Preset search and favorites** | The picker pages through 551 classics with no text filter or favorites. Add instant filename search, a favorite toggle and a favorites-only shuffle pool. Keep favorites by full path so libraries can contain duplicate names. |
| 2 | **Keyboard access and persistent controls** | The custom deck is mouse-driven. Add an explicit show/hide shortcut, a pin-controls option, Tab/arrow/Enter navigation and accessible names/states. H only controls the status readout. The native Options dialog already has standard Windows navigation. |
| 3 | **A usable text editor in the preset lab** | The editor lacks selection, clipboard operations, undo/redo and horizontal scrolling. Closing it discards unsaved changes. Standard editing behavior, a modified indicator and a save/discard choice would make the lab practical for real presets. |
| 4 | **Audio-file playback controls** | File mode plays one file in a loop, without pause, seek, elapsed time or a loop toggle. Add a small transport and filename display; keep Space's existing next-visual shortcut or explicitly resolve that conflict. |
| 5 | **Audio device and signal controls** | Capture follows Windows' default endpoint, with no device picker or input-level control. A device selector, signal meter and gain/sensitivity setting would help microphone users and multi-interface setups. |
| 6 | **Recording feedback and destination controls** | The recording indicator shares the fading HUD. Add a persistent recording badge, clear stop/failure feedback, a destination picker and an Open recordings folder action. |

I would take search/favorites and keyboard/pinned controls first. Together they
make the existing visual library easier to find and operate. The lab and file
transport improvements deserve separate focused changes because they add
state, navigation and preservation behavior that need their own tests.

## README and screenshots

- Put installation, first-launch help and app usage ahead of developer steps.
- Explain silent demo mode, the bottom-edge gesture, Help/Options, and tray restore.
- Distinguish the unreleased startup hint from the behavior in v1.2.0.
- Lead with `wavescope-nebula.milk`; include the updated lab and a GPU Warp
  tunnel example. Capture from the native app without altering the visuals.
- Clarify that library changes refresh the picker; external edits to the
  playing file require selecting it again, while the lab can apply immediately.
