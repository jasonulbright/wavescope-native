# WaveScope native — dependency decisions

The "OS DLLs only, one static exe" goal survives this review essentially
intact: it is the right call for this app, and no hand-rolled component needs
replacing with a third-party library. Verdicts: **keep** (reviewed, correct,
boring), **fixed** (kept hand-rolled, defects repaired in place), **OS-native**
(the OS facility was already the implementation or replaces hand-rolling),
**vendored** (deliberate third-party), **removed**.

| Component | Implementation | Verdict | Reasoning / audit surface |
|---|---|---|---|
| TLS + HTTP (Spotify token & API) | WinHTTP (schannel) | OS-native, keep | Exactly what the security bar demands; no hand-rolled TLS anywhere. Cert validation at OS defaults, never relaxed. |
| Secrets at rest (refresh token) | DPAPI `CryptProtectData` | OS-native, keep | User-bound, zero deps. Already in place before this review. |
| SHA-256 + RNG (PKCE) | bcrypt.dll (CNG) | OS-native, keep | No hand-rolled crypto. |
| base64url / hex / URL-encode | ~40 lines hand-rolled | keep | Small, total, bounded; unit-testable. |
| OAuth loopback listener | winsock, hand-parsed request line | fixed | Untrusted local input, so it got the strictest review: loopback-only bind ✓, static responses ✓; fixed: bind-before-browse, accept-loop until valid `state`, `SO_EXCLUSIVEADDRUSE`, joined shutdown. Alternative http.sys (HttpAPI) adds URL-ACL friction for zero practical gain on 127.0.0.1. |
| JSON reads (Spotify responses) | flat key scan (~30 lines) | keep | Only trusted-server responses, only string/scalar extraction, bounded. Not used on any hostile input. |
| Audio capture/render | WASAPI | OS-native, keep | Loopback is the whole desktop advantage. Polling loop kept (5 ms, simple); event-driven adds complexity for ≤5 ms. |
| Audio file decode | Media Foundation SourceReader | OS-native, keep | MP3/AAC/WAV/FLAC on Win10+ with zero deps; miniaudio not needed. |
| Clip recording (new) | Media Foundation SinkWriter, hardware H.264 + AAC | OS-native | The desktop-right answer vs. the web's WebM. |
| FFT | hand-rolled radix-2, 2048-pt (~50 lines) | fixed, keep | Reviewed against the DFT definition; correct. Fixed the *calibration around it* (window choice, normalization, smoothing order) to match the Web Audio analyser. 2048-pt @60 Hz ≈ nothing on a modern core — kissfft would buy no observable win at this size; unit tests pin correctness (impulse, sine-bin, Parseval). |
| 2D renderer + text | Direct2D + DirectWrite + WIC | OS-native, keep | Retained-target trails port the canvas technique 1:1; DirectWrite gives crisp UI on every engine via the overlay texture. |
| GL hosts (projectM + shaders) | WGL compatibility contexts | keep | projectM requires GL; the shader host reuses the identical pattern. A DXGI-flip-model/D3D rewrite would be a renderer replacement (needs owner approval) and buys little for a vsynced visualizer; not pursued. |
| **libprojectM 4.x** | vendored DLLs (`projectM-4.dll`, `-playlist.dll`), LGPL-2.1, built from `C:\projects\projectm` via its `build-native.bat`, import-lib linked | vendored, pinned | The one deliberate third-party dependency; real MilkDrop parity is not hand-rollable. Audit surface added: the `.milk` parser + ns-eel VM run in-process on user-chosen preset files (1 MB read cap on our side). Ships beside the exe; needs VC runtime. Playlist DLL is staged but unused by our code — kept only because the main DLL links it. |
| ini persistence | Get/WritePrivateProfileString | OS-native, fixed | Boring and bounded; added a write mutex (worker vs UI thread). |
| Tray / registry / dialogs | shell32, advapi32, comdlg32 | OS-native, keep | |
| GL bitmap font (`wglUseFontBitmaps`) | dead code from the pre-overlay HUD | removed | The D2D overlay texture replaced it. |
| **Spout2** (OBS texture sharing) | third-party | NOT ADDED | Brief requires explicit owner approval first. Open question in the handoff. |
| Monospace UI font | system Consolas | keep | No font embedding → no license question; matches the instrument aesthetic. |

## The honest tradeoff assessment

The minimal-dependency constraint did **not** cause the app's real problems.
Everything serious found in review (FINDINGS.md) is an implementation defect —
DPI awareness, the OAuth listener ordering, the duplicated projectM PCM feed,
analyser calibration — not a missing library. The two places a library is
conventionally reached for (FFT, JSON) are small, closed, testable problems
here, and the hand-rolled versions after fixes are correct and fast enough by
large margins. Where the OS already ships the hard thing (TLS, crypto, codecs,
compositor), this codebase already uses it. The constraint is working; keep it.
