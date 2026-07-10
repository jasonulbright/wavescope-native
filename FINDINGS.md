# WaveScope native — adversarial review findings

Reviewed: every line of `src/` (main.cpp, viz_modes.inl, projectm_host.inl,
shader_host.inl, spotify.inl, ui_paint.inl) against the web engine at
`C:\projects\wavescope` and the parity contract. Severity-ranked; each entry
has the failure scenario that makes it real. Status column tracks the fix.

Two passes: the original review (sections below), then a **second
independent pass (2026-07-10)** over the post-review codebase — every line
re-read, the app rebuilt, run, and probed live. Its findings are in the
"Second pass" section at the end; everything above it re-verified as fixed.

## Policy (sanitize)

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| Z1 | HIGH | README.md:10 | Stale vendor domain in the "frozen at" URL — violates the repo sanitize policy. | fixed |
| Z2 | HIGH | git history | All 21 pre-review commit messages carried `Co-Authored-By` trailers naming an AI vendor. The owner ran the one-time history rewrite (2026-07-10); verified clean — zero matches across every commit message. All commits from the review forward were already clean. | fixed (owner) |

## Security

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| S1 | HIGH | spotify.inl:276-288 | OAuth listener binds **after** `ShellExecute` opens the browser. An already-consented account redirects instantly → connection refused → auth fails. Also single-accept: any stray local connection during the 3-minute window (port scanner, curious process) consumes the one `accept()` and kills the flow even with a valid login in flight. Fix: bind+listen before opening the browser; loop accepting until a request with the correct `state` arrives or the deadline passes. | fixed |
| S2 | MED | spotify.inl:181 | `SO_REUSEADDR` on the callback listener. On Windows this allows another local process to bind-steal 127.0.0.1:8888 mid-flow (hijack → DoS/phish page). `SO_EXCLUSIVEADDRUSE` is the correct flag. | fixed |
| S3 | MED | spotify.inl:341-344 | The Spotify flow runs on a **detached** thread. Quit the app while the 3-minute listener is pending → thread keeps running through CRT/static teardown → use-after-free of `g_spotify` on exit. Fix: joinable thread + a quit event the listener selects on. | fixed |
| S4 | LOW | spotify.inl:281 | `clientId` read from the ini is spliced into the authorize URL without URL-encoding — a hand-edited ini value containing `&` injects extra OAuth params. Encode it. | fixed |
| S5 | LOW | spotify.inl:237 / main.cpp:1592 | Spotify worker (`SaveToken`) and UI thread (`SaveConfig`) both write `wavescope.ini` via profile APIs with no coordination; interleaved writes can corrupt the file. Serialize with a mutex. | fixed |
| S6 | INFO | spotify.inl | Reviewed, good: PKCE S256, random `state` validated, bind to `INADDR_LOOPBACK` only, WinHTTP/schannel for every token+API call, static HTTP responses (attacker input never echoed), refresh token DPAPI-encrypted at rest, client ID public-by-design in the ini. | kept |
| S7 | LOW | projectm_host.inl:205-224 | Raw `.milk` text (≤1 MB cap, good) goes straight into libprojectM's parser in-process. A hostile preset exploits any projectM parser bug. Accepted risk of the pinned dependency: presets come only from folders the user chooses; keep the size cap; track upstream. | accepted |

## Crashes / races / robustness

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| C1 | HIGH | (absent) | No per-monitor-v2 DPI awareness (no manifest, no `SetProcessDpiAwarenessContext`). On any >100% display, Windows bitmap-stretches the app (blurry) and client-rect pixels are virtualized; DPI change mid-session is unhandled. Non-negotiable #5. | fixed |
| C2 | HIGH | main.cpp:1931-1943 | Hidden ≠ idle: minimize-to-tray hides the window but the render loop keeps drawing flat-out (occluded D2D `EndDraw` doesn't vsync-block) → a "hidden" visualizer pegs a CPU core + GPU indefinitely. | fixed |
| C3 | MED | main.cpp:745,762,799 | `swprintf_s` into 300-wchar HUD buffers with preset filenames up to 259 chars leaves ~3 chars of headroom; overflow calls the CRT invalid-parameter handler = **process abort**. A long `.milk` filename in a user library is enough. Use truncating formatting. | fixed |
| C4 | MED | viz_modes.inl (all modes) | Per-mode state lives in function-local statics ("single-instance app") but wall mode creates N Apps: two windows on the same mode share particle pools/history → double-stepped particles, corrupted trails. | fixed (per-view state) |
| C5 | MED | main.cpp:315-320,365 | Picking a new audio file while the source is already FILE never switches tracks: `RunFile` reads the path once and loops `while source==FILE`. Fix: generation counter on the path. | fixed |
| C6 | MED | main.cpp:243-296 | Default-device change is only noticed when the old endpoint hard-fails. Switching the default output device (no unplug) leaves capture on the old, now-silent endpoint. Add `IMMNotificationClient` → restart capture on default change. | fixed |
| C7 | LOW | main.cpp:1860 | `CreateView` failure path deletes the App while its already-created window still holds the pointer in GWLP_USERDATA → any late message is a use-after-free. Destroy the window first. | fixed |
| C8 | LOW | main.cpp:1778-1812 | Typing in the palette-name field still fires WM_KEYDOWN shortcuts: naming a palette "Sunset" cycles shuffle on 'S', 'B' launches the Spotify flow, '4' opens a file dialog. Suppress shortcuts while the name field has focus. | fixed |
| C9 | LOW | both GL hosts | No device-loss/TDR recovery: after a GPU reset `wglMakeCurrent` fails silently forever → permanently black engine view until app restart. D2D path handles `D2DERR_RECREATE_TARGET`; GL paths should detect+reinit (best effort). | fixed (detect + reinit) |
| C10 | LOW | main.cpp:1355 | `rand()` never seeded → identical shuffle sequence every launch. | fixed |

## Correctness vs. the web contract

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| P1 | HIGH | projectm_host.inl:261-268 | projectM is fed the **full 2048-sample sliding window every rendered frame** (~30× duplicated PCM at 60 fps). projectM's internal beat/bass detection sees 120k samples/s of overlapped audio → presets react hot and wrong vs. real MilkDrop. Feed only samples that newly arrived since the last frame. | fixed |
| P2 | MED | main.cpp:160-179 | Analyser mismatch vs. Web Audio: magnitude normalized by N/2 instead of N (+6 dB), Hann window instead of Blackman, and the 0.8 smoothing applied **after** the dB mapping instead of on linear magnitudes. Bands read hotter and attack/decay feels different → beat thresholds behave differently than the web app on the same music. | fixed |
| P3 | MED | main.cpp:271-296 | Silence freezes the picture: when nothing plays, loopback delivers no packets, the ring stays stale, and every frame re-analyzes the same window → bars stand still. Web decays to dark. Feed zeros when the stream is silent/stalled. | fixed |
| P4 | LOW | main.cpp:216-231 | Demo signal drift: chord retunes every 2 s (spec: every 8 kicks = 4 s), kick is a fixed 55 Hz sine (web: 120→40 Hz sweep), no LFO detune. | fixed |
| P5 | INFO | main.cpp / viz_modes.inl | Reviewed, matching: band edges (floor/ceil, Hz-per-bin from real sample rate), RMS ¼-decimation, level smoothing 0.2, beat constants (1.35/0.08/e^-6dt), `BinLog` = web `bin()`, `Bands` smoothing factors, all 34 mode fade/flash flags, palette math (hue/sat/lightness ramps identical). | kept |

## Performance

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| F1 | MED | main.cpp:1939 | Wall mode renders N windows sequentially on one thread, each vsync-blocking → 3 monitors = 20 fps each. Fix with the companion-window rework (one tap, per-window present without stacked vsync waits). | fixed (companion rework) |
| F2 | LOW | viz_modes.inl:141-149 | `Blit` creates a new D2D bitmap every frame (Plasma/Waterfall). Cache and update instead. | fixed |
| F3 | LOW | both GL hosts | Overlay upload uses `glTexImage2D` (full realloc) on every change; `glTexSubImage2D` after first allocation. | fixed |
| F4 | INFO | main.cpp:271 | Capture thread polls with `Sleep(5)` instead of event-driven WASAPI. Adds ≤5 ms latency; simple and robust. | kept |

## UX / contract deviations (folded into parity work)

| # | Where | Finding | Status |
|---|-------|---------|--------|
| U1 | main.cpp:1778 | Key map diverges from the web contract (web: M=displays, R=record, Esc=restore; native had M=milkdrop-engine, R=resolution, Esc=quit). Realigned — see README key table. | fixed |
| U2 | main.cpp:1808 | Esc quit-anywhere → now: panel → menu → fullscreen → quit. | fixed |
| U3 | (absent) | No "DJ" shuffle option (hop engine AND visual). | added |
| U4 | (absent) | No now-playing readout from `/v1/me/player/currently-playing`. | added |
| U5 | main.cpp:799 | HUD lacks SHUFFLE/REC/now-playing lines from the contract readout. | fixed |
| U6 | main.cpp:1588 | Engine, MILK library folder, window placement not persisted. | fixed |
| U7 | main.cpp:1920-1929 | `file=` swallows the rest of the command line (flags become part of the path); bare-substring flag matching ("demo" inside a filename triggers demo source). | fixed |
| U8 | projectm_host.inl:173 | Preset scan order is filesystem-dependent; sort for stable cycling. | fixed |
| U9 | README.md | Overstates ("zero runtime dependencies" vs. the projectM DLLs + VC runtime), stale key list. | rewritten |
| U10 | main.cpp:84, hosts | Dead code: `Trace()`, GL bitmap-font display lists (`fontBase`, `wglUseFontBitmaps`) left from the removed GL HUD painter, stale GLPainter comment. | removed |

## Second pass (2026-07-10, post-review verification)

Method: full re-read of all 11 source files, build, unit tests, `selftest`,
plus live probes (engine restore, 8K recording, deck wrap at narrow widths,
ini before/after runs, `git log --all` sweeps). Findings ranked; status
tracks this pass's fixes.

### Policy (sanitize)

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| Z3 | HIGH | git history | Z2's "verified clean" checked **messages only**. (a) 10 commits on `main` — already force-pushed to the public GitHub remote — carry an AI-vendor **author/committer identity**; `git log --format=%an` leaks it. (b) `refs/original/refs/heads/master` (the history-rewrite backup ref) still holds the entire pre-rewrite history, trailers included: `git log --all` shows everything the rewrite was meant to remove. | **OWNER ACTION** — history rewrites are yours to run. One-time, from the repo root: see "Z3 remediation" below. |

**Z3 remediation (run once, owner — Git Bash, from this repo's root; NOT
PowerShell, which expands `$GIT_…` inside double quotes and mangles the
script):**

```
git filter-branch -f --env-filter '
if [ "$GIT_AUTHOR_EMAIL" != "jason.ulbright@gmail.com" ]; then
  GIT_AUTHOR_NAME="Jason Ulbright"; GIT_AUTHOR_EMAIL="jason.ulbright@gmail.com"
  export GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL
fi
if [ "$GIT_COMMITTER_EMAIL" != "jason.ulbright@gmail.com" ]; then
  GIT_COMMITTER_NAME="Jason Ulbright"; GIT_COMMITTER_EMAIL="jason.ulbright@gmail.com"
  export GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL
fi
' -- main
git update-ref -d refs/original/refs/heads/master
git reflog expire --expire=now --all
git gc --prune=now
git update-ref refs/remotes/origin/main main
git push --force origin main
```

(The `!=` form maps every non-owner identity to the owner — this repo has
exactly two identities, so it needs no third-party strings in this file.)
Verify afterwards in Git Bash — both must print nothing / `0`:

```
git log --all --format="%ae %ce" | grep -v jason.ulbright@gmail.com
git log --all --format="%s %b" | grep -ci co-authored
```

Note the pre-rewrite `wavescope-native.7z` backup also contains the old
history — keep it private or delete it.

### Crashes / correctness

| # | Sev | Where | Finding / failure scenario | Status |
|---|-----|-------|----------------------------|--------|
| N1 | MED | main.cpp ToggleSpout/selftest | `selftest` silently rewrites the user's persisted settings: the Spout leg calls `ToggleSpout()` → `SaveConfig()` with the test's transient state. Observed live: owner's `engine=2` (projectM) clobbered to `0` by one selftest run — next launch boots the wrong engine. Any settings write during selftest has this problem. Fix: selftest runs with persistence suppressed. | fixed |
| N2 | MED | record.inl Start / main.cpp ToggleRecord | Recording at the 8K render target always fails ("recording failed to start"): H.264 encoders cap near 4096 px wide, and 7680x4320 BGRA also queues ~1 GB at depth 8. Verified live at 8K. Fix: clamp the recording surface to 4K height (built-ins render through it while recording; on-screen look unchanged — it scales to the window either way). | fixed |
| N3 | LOW | main.cpp:1629,1650 | Stale engine window titles: "projectM (M to exit, ...)" / "GPU shader (G to exit, ...)". M has been the displays menu since the key realignment — the title teaches a wrong key. | fixed |
| N4 | LOW | main.cpp RenderFrame `extra[]` | HUD extras accumulate with `off += _snwprintf_s(..., _TRUNCATE, ...)`; on truncation that returns −1 and the size_t offset wraps → the **next** append writes through a wild pointer. Unreachable today only because now-playing is capped at 60 chars (worst case ≈93/220); one cap change away from memory corruption. Harden the accumulation. | fixed |
| N5 | LOW | projectm_host.inl / shader_host.inl Init | Init failure paths (`SetPixelFormat`, context creation, `projectm_create`) return false leaving the child window/DC/context alive; each retry of G / engine buttons leaks another set. Fix: tear down on failure. | fixed |
| N6 | LOW | spotify.inl StartPlayback/Notify | Worker-thread `MessageBoxW` with no owner: quit while a notice is up and `Shutdown()`'s join waits on the dialog — app lingers until someone clicks OK. Also `StartPlayback` notifies even mid-shutdown. Fix: skip notices once `quitFlag` is set. | fixed |
| N7 | INFO | spotify.inl SpHttps | WinHTTP's default redirect policy would resend the bearer on a cross-host redirect. Spotify's API doesn't redirect cross-host, and TLS + host pinning bound the risk; hardened anyway: every WinHTTP call (token + API) now uses `WINHTTP_OPTION_REDIRECT_POLICY_NEVER` — an unexpected redirect fails loud instead of leaking a bearer. | fixed |

### Parity deviations found live (deck/HUD vs. the web console)

| # | Sev | Where | Finding | Status |
|---|-----|-------|---------|--------|
| D1 | MED | main.cpp deck | The right cluster is missing the web's `rec (R)` button — recording exists but is undiscoverable (hotkey-only, documented nowhere on screen). | fixed |
| D2 | LOW | main.cpp deck | Picker group label reads MODE always; web switches it to PRESET when a preset engine is active. | fixed |
| D3 | LOW | main.cpp deck | TRACE / calm / palettes+shuffle stay visible under GPU/projectM where trace palettes don't apply; the web hides them there (keys keep working). | fixed |
| D4 | LOW | projectm_host.inl | PROJM's first preset is alphabetical: `102-per_frame3.milk` — a test fixture. Web defaults to WaveScope Tunnel and sorts the Test presets last. | fixed |
| D5 | LOW | main.cpp HUD | HUD RES line shows the target label (AUTO/4K); web shows the real backing pixels. Misleading for the GL engines, which render at window size regardless of target. Now shows actual pixels, like the web. | fixed |
| D6 | INFO | shader/projectm hosts | The GL engines ignore the render-resolution target (window-size render); the web renders GPU/projectM at the target for supersampling. Real-world impact is low (fullscreen = native monitor res) and an FBO indirection isn't free; documented as a deliberate deviation in PARITY.md instead of built. | documented |
