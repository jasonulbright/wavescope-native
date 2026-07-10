# WaveScope native — adversarial review findings

Reviewed: every line of `src/` (main.cpp, viz_modes.inl, projectm_host.inl,
shader_host.inl, spotify.inl, ui_paint.inl) against the web engine at
`C:\projects\wavescope` and the parity contract. Severity-ranked; each entry
has the failure scenario that makes it real. Status column tracks the fix.

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
