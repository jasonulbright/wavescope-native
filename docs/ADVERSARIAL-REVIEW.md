# GUI and adversarial review — 2026-09-09

Scope: application-owned C++ code, GUI input/layout paths, Spotify OAuth and
persistence, recording lifetime, build/packaging and release automation.
This is the first review pass, not a security certification or a full audit
of Windows, projectM, Spout, codecs or every community preset.

## Findings addressed

| Finding | Impact | Change / regression coverage |
| --- | --- | --- |
| Recorder worker left joinable after failure/automatic stop | Restart or destruction could call `std::terminate` | Always join before restart/destruction; selftest forces automatic stop then restarts |
| OAuth socket accepted a peer then blocked indefinitely in `recv` | Local idle peer could prevent app shutdown | Bounded request size/deadline and cancellation ticks; live loopback test with an idle peer |
| OAuth query parser matched suffixes and ignored method/path | Unrelated parameters or paths could be accepted as callback fields | Exact GET callback path and unique decoded parameter keys; negative tests |
| Worker-thread error message boxes | Shutdown could wait for an unseen dialog | Nonmodal status displayed in the app; workers never show modal UI |
| Bundled Spotify client ID | Users inherited the project's development-app restrictions | Removed default; accessible native Options dialog with validation and disconnect |
| Token loader started polling before reading configured client ID; refresh races | Wrong app ID and concurrent refresh-token rotation | Read configuration first; serialize sign-in/refresh/config writes |
| Flat JSON scans and unsafe string conversions | Incorrect track/artist names, broken escapes, out-of-contract string writes | Windows JSON parser, explicit-length UTF conversion and Unicode/order tests |
| Signed shifts and unchecked crypto errors | Undefined encoding behavior; failure could produce predictable auth values | Unsigned base64 accumulator; fail closed on RNG/hash failure; RFC 7636 vector test |
| Lab hotkey's queued character entered the editor | Pressing L changed the preset before editing | Consume the opening character; reproduced by GUI and covered by selftest |
| GL overlay omitted lab edit/caret/scroll/status state | Editor changes could remain stale | Include all lab state in redraw invalidation; edit/close regression checks |
| Palette list and long preset labels exceeded window bounds | Controls became unreachable; labels overlapped | Pagination, wrapping, clipping and minimum window size; 760/960/1440-width layout tests |
| Modal panels allowed unrelated shortcuts; slider lacked capture | Unexpected source/engine changes, stuck drag | Isolate panel input, capture mouse drags, forward GL wheel input |
| Selftest loaded Spotify tokens and personal settings | Test runs could access network or rewrite configuration | No Spotify/config load during selftest; close capture thread before exit |
| Audio sample-rate data race and timed thread abandonment | Undefined access and possible teardown while thread still running | Atomic sample rate and actual join during teardown |
| Second-resolution recording names | Rapid takes could overwrite an earlier clip | Milliseconds, process ID and per-process sequence in filenames |
| Package batch used ungrouped conditional exits | Package exited before building/staging | PowerShell packer with explicit error handling and path checks |
| Packaging copied all local presets | Personal presets could ship inadvertently | Stage tracked assets only; reject settings in staged release |
| No signing workflow or Windows version resource | Unsigned release, missing executable metadata | Versioned resources and Azure OIDC signing with signature/timestamp checks |

The only bundled Spotify identifier found was a public client ID, not a
client secret. Removing it does not remove copies in Git history. Personal
login tokens remain protected with Windows DPAPI; that does not protect
against another process already running as the same Windows user.

## Verification and remaining limits

`build.bat` runs deterministic math/registry tests and adversarial protocol,
Unicode, token-configuration and real loopback socket tests. `package.bat`
runs the staged rendering/recording/Spout test with the new GUI and recorder
regressions. Manual GUI checks exercise the native Options dialog and lab.

Real Spotify authorization/playback was not exercised by these local tests.
Azure signing is verified separately by the release workflow; local test
results alone do not establish that a build is signed. See the
[release verification procedure](RELEASING.md) and each release's workflow run.
Hosted CI cannot replace the local GPU selftest. The custom-drawn deck/lab
still lack full keyboard traversal and screen-reader semantics; the native
Options dialog supports standard Windows controls.

Synchronous Windows media decoding and WinHTTP calls can still delay shutdown
while the OS completes an operation; the OAuth idle-socket and modal-dialog
deadlocks are removed. Third-party preset/codec parsing runs in-process.
Calm mode reduces selected effects and is not a photosensitivity guarantee.

Final validation: MSVC build and all unit/adversarial tests passed; the staged
selftest reported ALL PASS for 34 modes, 17 shaders, MP4 decode, automatic
recording restart, Spout roundtrip, preset morphing, lab redraw/input, and VJ.
The portable ZIP contained no settings, personal presets or test artifacts.
PowerShell parsing and actionlint passed. The signature verification gate
correctly rejected the unsigned local build. The pinned dependency build
script was also built and tested locally with Visual Studio 2026.
