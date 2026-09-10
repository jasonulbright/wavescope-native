// Self-test harness: `WaveScope.exe selftest` exercises every engine on the
// demo signal, dumps what each one actually rendered (PNG beside the exe),
// asserts the frames are not black, and writes wavescope-selftest.log.
// Exit code 0 = every engine rendered. This is the build-and-verify loop the
// project runs after changes (README: Verifying).

#include <string>
#include <vector>

// Write a BGRA buffer as PNG via WIC (flipY for bottom-up GL readbacks).
static bool StWritePng(const wchar_t* path, const uint8_t* bgra, UINT w, UINT h,
                       bool flipY) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    do {
        if (FAILED(fac->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) break;
        if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&frame, nullptr))) break;
        if (FAILED(frame->Initialize(nullptr))) break;
        if (FAILED(frame->SetSize(w, h))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) break;
        std::vector<uint8_t> flipped;
        const uint8_t* src = bgra;
        if (flipY) {
            flipped.resize((size_t)w * h * 4);
            for (UINT y = 0; y < h; y++)
                memcpy(&flipped[(size_t)y * w * 4], bgra + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
            src = flipped.data();
        }
        if (FAILED(frame->WritePixels(h, w * 4, w * h * 4, (BYTE*)src))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(enc->Commit())) break;
        ok = true;
    } while (false);
    SafeRelease(&frame);
    SafeRelease(&enc);
    SafeRelease(&stream);
    SafeRelease(&fac);
    return ok;
}

// Percentage of pixels that are not near-black (any channel > 20).
static float StLitPct(const uint8_t* bgra, size_t n) {
    size_t lit = 0;
    for (size_t i = 0; i < n; i += 4)
        if (bgra[i] > 20 || bgra[i + 1] > 20 || bgra[i + 2] > 20) lit++;
    return n ? 100.0f * (float)lit / (float)(n / 4) : 0.0f;
}

// Capture the client area of a window (works for the D2D-presented view).
static bool StCaptureWindow(HWND hwnd, std::vector<uint8_t>& out, int& w, int& h) {
    RECT rc; GetClientRect(hwnd, &rc);
    w = rc.right; h = rc.bottom;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HGDIOBJ old = SelectObject(mdc, bmp);
    // PW_CLIENTONLY | PW_RENDERFULLCONTENT: composited content incl. D2D.
    BOOL got = PrintWindow(hwnd, mdc, 0x1 | 0x2);
    SelectObject(mdc, old);
    bool ok = false;
    if (got) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; // top-down
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        out.resize((size_t)w * h * 4);
        ok = GetDIBits(mdc, bmp, 0, h, out.data(), &bi, DIB_RGB_COLORS) == h;
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return ok;
}

// Re-open a recorded MP4 and report duration, size, audio presence; dump a
// decoded mid-file frame (assumed top-down for the PNG — eyeball the bars).
static bool StVerifyMp4(const wchar_t* path, const wchar_t* framePng,
                        double& durSec, UINT32& w, UINT32& h, bool& hasAudio) {
    durSec = 0; w = h = 0; hasAudio = false;
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFSourceReader* rd = nullptr;
    bool ok = false;
    // Advanced video processing lets the reader hand H.264 back as RGB32.
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 1);
    if (attr) attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    HRESULT openHr = MFCreateSourceReaderFromURL(path, attr, &rd);
    if (attr) attr->Release();
    if (SUCCEEDED(openHr) && rd) {
        PROPVARIANT var; PropVariantInit(&var);
        if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                   MF_PD_DURATION, &var)) && var.vt == VT_UI8)
            durSec = (double)var.uhVal.QuadPart / 1e7;
        PropVariantClear(&var);
        IMFMediaType* nat = nullptr;
        hasAudio = SUCCEEDED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &nat));
        if (nat) { nat->Release(); nat = nullptr; }
        // Decode video to RGB32 and pull a frame from mid-file.
        IMFMediaType* mt = nullptr; MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt))) {
            IMFMediaType* cur = nullptr;
            if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
                MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
                cur->Release();
            }
            PROPVARIANT pos; PropVariantInit(&pos);
            pos.vt = VT_I8; pos.hVal.QuadPart = 5000000;   // 0.5 s in
            rd->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);
            for (int tries = 0; tries < 10; tries++) {
                DWORD flags = 0; IMFSample* smp = nullptr;
                if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                          nullptr, &flags, nullptr, &smp))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
                if (!smp) continue;
                IMFMediaBuffer* buf = nullptr;
                if (SUCCEEDED(smp->ConvertToContiguousBuffer(&buf)) && buf) {
                    BYTE* data = nullptr; DWORD len = 0;
                    if (SUCCEEDED(buf->Lock(&data, nullptr, &len)) &&
                        len >= (DWORD)(w * h * 4) && w && h) {
                        StWritePng(framePng, data, w, h, false);
                        ok = true;
                    }
                    if (data) buf->Unlock();
                    buf->Release();
                }
                smp->Release();
                if (ok) break;
            }
        }
        mt->Release();
        rd->Release();
    }
    MFShutdown();
    return ok;
}

// Drive one view through every engine on the demo signal; verify output.
static int RunSelfTest(App* v) {
    // PrintWindow/DWM needs a visible presentation surface. The harness may
    // be launched by a hidden packaging process; show its own test window
    // without taking focus rather than mistaking an occluded target for black.
    ShowWindow(v->hwnd, SW_SHOWNOACTIVATE);
    std::wstring dir = PmExeDir();
    FILE* log = _wfopen((dir + L"wavescope-selftest.log").c_str(), L"w, ccs=UTF-8");
    auto out = [&](const wchar_t* fmt, auto... args) {
        wchar_t line[512];
        _snwprintf_s(line, _TRUNCATE, fmt, args...);
        if (log) { fwprintf(log, L"%s\n", line); fflush(log); }
        OutputDebugStringW(line); OutputDebugStringW(L"\n");
    };
    auto pump = [] {
        MSG m;
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m); DispatchMessage(&m);
        }
    };
    int rc = 0;
    g_tap.SetSource(AudioTap::SRC_DEMO);
    out(L"selftest: WaveScope %s", WAVESCOPE_VERSION);
    out(L"selftest: source=%s sampleRate=%u", g_tap.SourceName(), g_tap.SampleRate());

    // Discoverability: no mouse movement is needed, the full 15 seconds begin
    // at the first frame, and dismissing the preview must not eat a shortcut.
    {
        v->Render();
        bool pass = v->StartupUiVisible() && v->DeckShown() && v->HudFade() == 1;
        std::vector<uint8_t> px; int w = 0, h = 0;
        pass &= StCaptureWindow(v->hwnd, px, w, h) &&
            StWritePng((dir + L"selftest-startup.png").c_str(), px.data(), w, h, false);

        // The launch deck must be clickable before any hover event arrives.
        v->deckVisible = false; v->hudActivity = -3;
        float sc = v->UiScale(); D2D1_SIZE_F sz = v->rt->GetSize();
        D2D1_RECT_F buttons[App::DK_N]; v->DeckLayout(sz.width / sc, sz.height / sc, buttons);
        D2D1_RECT_F helpButton = buttons[App::DK_HELP];
        v->OnClick((int)((helpButton.left + helpButton.right) * sc * 0.5f),
                   (int)((helpButton.top + helpButton.bottom) * sc * 0.5f));
        pass &= v->help;
        v->help = false;
        v->startupUiUntil = -1;
        v->UpdateUiVisibility(100); // simulate a slow startup before the first frame
        v->UpdateUiVisibility(114.99f);
        pass &= v->StartupUiVisible() && v->DeckShown() && v->HudFade() == 1;
        v->UpdateUiVisibility(115);
        pass &= !v->StartupUiVisible() && !v->DeckShown() && v->HudFade() == 0;
        v->UpdateUiVisibility(200);
        pass &= !v->StartupUiVisible(); // expiry never starts another preview

        int mode = v->modeIdx;
        v->startupUiUntil = -1; // key delivered even before the first frame
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_RIGHT, 0);
        v->UpdateUiVisibility(v->Now());
        pass &= !v->StartupUiVisible() && !v->DeckShown() && v->modeIdx != mode;
        v->modeIdx = mode;
        v->startupUiUntil = -1; v->UpdateUiVisibility(v->Now());
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_F1, 0);
        pass &= v->help && !v->StartupUiVisible() && !v->DeckShown();
        v->help = false;
        v->startupUiUntil = -1; v->UpdateUiVisibility(v->Now());
        SendMessageW(v->hwnd, WM_KEYDOWN, 'H', 0);
        pass &= !v->hud && v->HudFade() == 0 && !v->StartupUiVisible();
        SendMessageW(v->hwnd, WM_KEYDOWN, 'H', 0);
        bool hudPass = v->hud && v->HudFade() == 1;
        out(L"gui: re-enabling the HUD after idle makes it visible -> %s", hudPass ? L"PASS" : L"FAIL");
        if (!hudPass) rc = 1;
        v->hudActivity = -3;

        // Menus and recent mouse activity outlive the launch timer. Expiring
        // just the hint must invalidate the GL cache even with the deck shown.
        v->startupUiUntil = -1; v->UpdateUiVisibility(100);
        v->OpenMenu(App::DK_RES);
        v->UpdateUiVisibility(115);
        pass &= v->DeckShown() && v->deckMenu == App::DK_RES;
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        v->startupUiUntil = -1; v->UpdateUiVisibility(100);
        v->deckVisible = true; v->deckActivity = 114; v->hud = false;
        RECT client; GetClientRect(v->hwnd, &client);
        v->BuildOverlay(client.right, client.bottom, L"fixed", L"");
        v->UpdateUiVisibility(115);
        pass &= v->DeckShown() && v->BuildOverlay(client.right, client.bottom, L"fixed", L"");
        v->UpdateUiVisibility(118);
        pass &= !v->DeckShown();
        v->hud = true; v->deckVisible = false; v->deckActivity = 0; v->olSig.clear();
        out(L"gui: launch controls, 15-second timeout, first-key actions, overlay expiry -> %s", pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
    }

    // Small-window layout and modal input regressions, independent of a GPU.
    {
        bool pass = true;
        auto inside = [](const D2D1_RECT_F& r, float w, float h) {
            return r.left >= 0 && r.top >= 0 && r.right <= w + 0.1f && r.bottom <= h + 0.1f;
        };
        for (int engine = 0; engine < 3; ++engine) {
            v->pmActive = engine == 2; v->shaderActive = engine == 1;
            v->pmHost.curName = std::wstring(250, L'A');
            for (float width : {760.0f, 960.0f, 1440.0f}) {
                D2D1_RECT_F buttons[App::DK_N]; v->DeckLayout(width, 560, buttons);
                for (int i = 0; i < App::DK_N; ++i) if (!v->DeckHidden(i)) pass &= inside(buttons[i], width, 560);
            }
        }
        v->pmActive = v->shaderActive = false;
        for (int i = 0; i < 64; ++i) { CustomPalette cp; cp.name = L"test"; v->customPalettes.push_back(cp); }
        v->palOffset = 69;
        App::PalRects palette; v->PalLayout(760, 560, palette);
        pass &= inside(palette.card, 760, 560) && inside(palette.addBtn, 760, 560);
        pass &= v->palOffset + palette.rows == v->PaletteCount();
        v->palPanel = true; v->nameFocused = false;
        int source = g_tap.Source();
        SendMessageW(v->hwnd, WM_KEYDOWN, '2', 0);
        pass &= g_tap.Source() == source; // a modal panel cannot switch to mic
        v->palPanel = false; v->customPalettes.clear(); v->palInclude.clear(); v->palOffset = 0;
        int modeBeforeMenu = v->modeIdx;
        v->OpenMenu(App::DK_RES); // one-page menus must not leak navigation or shortcuts
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_RIGHT, 0);
        SendMessageW(v->hwnd, WM_KEYDOWN, '2', 0);
        bool menuPass = v->modeIdx == modeBeforeMenu && g_tap.Source() == source;
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        menuPass &= v->deckMenu == -1;
        v->modeIdx = modeBeforeMenu; g_tap.SetSource(source);
        out(L"gui: dropdown navigation and source shortcut isolation -> %s", menuPass ? L"PASS" : L"FAIL");
        pass &= menuPass;
        out(L"gui: small-window deck, palette paging, modal input -> %s", pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
    }

    // File transport exercises the real decoder and output clock with a
    // three-second silent WAV (no test tones through the user's speakers).
    {
        auto until = [&](auto predicate, DWORD timeout = 2500) {
            ULONGLONG end = GetTickCount64() + timeout;
            do { if (predicate()) return true; pump(); Sleep(10); } while (GetTickCount64() < end);
            return predicate();
        };
        wchar_t temp[MAX_PATH], id[40]; GetTempPathW(MAX_PATH, temp);
        GUID guid; CoCreateGuid(&guid); StringFromGUID2(guid, id, 40);
        std::wstring path = std::wstring(temp) + L"WaveScope-transport-" + id + L".wav";
        const DWORD bytes = 48000 * 3 * 2;
        std::vector<uint8_t> wav(44 + bytes, 0);
        memcpy(wav.data(), "RIFF", 4); memcpy(wav.data()+8, "WAVEfmt ", 8); memcpy(wav.data()+36, "data", 4);
        auto u32 = [&](int at, DWORD n) { memcpy(wav.data()+at, &n, 4); };
        auto u16 = [&](int at, WORD n) { memcpy(wav.data()+at, &n, 2); };
        u32(4,36+bytes); u32(16,16); u16(20,1); u16(22,1); u32(24,48000);
        u32(28,96000); u16(32,2); u16(34,16); u32(40,bytes);
        FILE* audio = _wfopen(path.c_str(), L"wb");
        bool pass = audio != nullptr;
        if (audio) { pass &= fwrite(wav.data(),1,wav.size(),audio) == wav.size(); fclose(audio); }
        g_tap.SetFile(path.c_str()); auto state = g_tap.FileState();
        pass &= until([&] { return state->ready && state->position > 1000000; });
        pass &= state->duration == 30000000 && state->seekable;
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_F5, 0);
        pass &= v->transportDialog != nullptr;
        SendDlgItemMessageW(v->transportDialog, IDC_AUDIO_PLAY, BM_CLICK, 0, 0);
        pass &= state->paused;
        Sleep(50); LONGLONG pausedAt = state->position; Sleep(100);
        pass &= state->position == pausedAt;
        AudioTap::TapCursor silentCursor; silentCursor.idx = UINT64_MAX;
        float quiet[2048]; g_tap.ReadNew(silentCursor, quiet, 2048); Sleep(30);
        int quietCount = g_tap.ReadNew(silentCursor, quiet, 2048);
        pass &= quietCount > 0; // paused recordings still receive real-time silence
        for (int i = 0; i < quietCount; ++i) pass &= quiet[i] == 0;
        SendDlgItemMessageW(v->transportDialog, IDC_AUDIO_SEEK, TBM_SETPOS, TRUE, 5000);
        SendMessageW(v->transportDialog, WM_HSCROLL, TB_ENDTRACK, (LPARAM)GetDlgItem(v->transportDialog, IDC_AUDIO_SEEK));
        pass &= until([&] { return state->seek < 0 && state->position == 15000000; });
        pass &= state->paused;
        // Space still advances the visual; Ctrl+Space only toggles audio.
        int mode = v->modeIdx; BYTE keys[256]; GetKeyboardState(keys);
        BYTE ctrl[256]; memcpy(ctrl, keys, sizeof(keys)); ctrl[VK_CONTROL] |= 0x80; SetKeyboardState(ctrl);
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_SPACE, 0); SetKeyboardState(keys);
        pass &= !state->paused && v->modeIdx == mode;
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_SPACE, 0);
        pass &= v->modeIdx != mode && !state->paused; v->modeIdx = mode;
        SendDlgItemMessageW(v->transportDialog, IDC_AUDIO_LOOP, BM_CLICK, 0, 0);
        pass &= !state->loop;
        g_tap.SeekFile(28000000);
        pass &= until([&] { return state->ended && state->paused; });
        pass &= state->position == state->duration;
        SendDlgItemMessageW(v->transportDialog, IDC_AUDIO_PLAY, BM_CLICK, 0, 0);
        pass &= until([&] { return !state->ended && state->position > 0 && state->position < 10000000; });
        SendDlgItemMessageW(v->transportDialog, IDC_AUDIO_LOOP, BM_CLICK, 0, 0);
        pass &= state->loop;
        g_tap.SeekFile(28000000);
        pass &= until([&] { return state->position >= 28000000; });
        pass &= until([&] { return state->seek < 0 && state->position < 10000000 && !state->paused; });
        g_tap.ToggleFilePause(); v->RefreshTransport();
        std::vector<uint8_t> pixels; int width = 0, height = 0;
        if (StCaptureWindow(v->transportDialog, pixels, width, height))
            StWritePng((dir + L"selftest-audio-player.png").c_str(), pixels.data(), width, height, false);
        DestroyWindow(v->transportDialog);
        out(L"audio: pause clock, seek while paused, shortcuts, drain/stop, replay, loop -> %s", pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;

        // Errors belong to their load, and choosing a valid file recovers.
        g_tap.SetFile((path + L".missing").c_str()); auto broken = g_tap.FileState();
        bool recovery = until([&] { return FAILED(broken->error.load()); });
        v->OpenTransport(); v->RefreshTransport();
        recovery &= !IsWindowEnabled(GetDlgItem(v->transportDialog, IDC_AUDIO_PLAY)) &&
            WsControlText(GetDlgItem(v->transportDialog, IDC_AUDIO_STATUS)).find(L"Playback failed") != std::wstring::npos;
        g_tap.SetFile(path.c_str()); state = g_tap.FileState();
        recovery &= until([&] { return state->ready && state->position > 0; });
        recovery &= SUCCEEDED(state->error.load()) && !state->paused && state != broken;
        v->RefreshTransport(); recovery &= IsWindowEnabled(GetDlgItem(v->transportDialog, IDC_AUDIO_PLAY)) != FALSE;
        DestroyWindow(v->transportDialog);
        g_tap.SetSource(AudioTap::SRC_DEMO);
        recovery &= until([&] { return DeleteFileW(path.c_str()) != FALSE; });
        recovery &= g_tap.Source() == AudioTap::SRC_DEMO;
        out(L"audio: failed load, new-file recovery, source change and file cleanup -> %s", recovery ? L"PASS" : L"FAIL");
        if (!recovery) rc = 1;
    }

    // Keyboard and persistent controls.
    {
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_TAB, 0);
        bool pass = v->DeckShown() && v->deckKeyboard && v->deckFocus >= 0;
        int oldMode = v->modeIdx;
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_RIGHT, 0);
        pass &= v->modeIdx == oldMode; // focused arrows navigate, not switch visuals
        v->deckFocus = App::DK_PIN;
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(v->hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        v->UpdateUiVisibility(v->Now() + 60);
        pass &= v->deckPinned && v->DeckShown() && !v->deckKeyboard;
        v->OpenControls();
        pass &= v->controlsDialog && GetDlgItem(v->controlsDialog, IDC_CONTROL_BASE + App::DK_SYS);
        pass &= GetNextDlgTabItem(v->controlsDialog, GetDlgItem(v->controlsDialog, IDC_CONTROLS_PIN), FALSE) != nullptr;
        std::vector<uint8_t> pixels; int w = 0, h = 0;
        if (StCaptureWindow(v->controlsDialog, pixels, w, h))
            StWritePng((dir + L"selftest-controls.png").c_str(), pixels.data(), w, h, false);
        CheckDlgButton(v->controlsDialog, IDC_CONTROLS_PIN, BST_UNCHECKED);
        v->ControlsCommand(IDC_CONTROLS_PIN, BN_CLICKED);
        DestroyWindow(v->controlsDialog);
        v->deckVisible = false; v->deckKeyboard = false;
        pass &= !v->deckPinned && !v->DeckShown();
        out(L"controls: keyboard navigation, persistent visibility, native tab controls -> %s", pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
    }

    // --- built-in D2D modes ---
    if (v->pmActive) v->ToggleProjectM();
    if (v->shaderActive) v->ToggleShader();
    // Sweep every mode for a few frames first: exercises each one's state
    // path (pools, history, scratch buffers) as a crash/hang smoke test.
    int keepMode = v->modeIdx;
    for (int mi = 0; mi < MODE_COUNT; mi++) {
        v->modeIdx = mi;
        for (int k = 0; k < 8; k++) { pump(); v->Render(); }
    }
    out(L"builtin: swept all %d modes", MODE_COUNT);
    v->modeIdx = keepMode;
    float t0 = v->Now(); int frames = 90;
    for (int i = 0; i < frames; i++) { pump(); v->Render(); }
    float fps = frames / (v->Now() - t0 + 1e-6f);
    {
        std::vector<uint8_t> px; int w = 0, h = 0;
        bool cap = StCaptureWindow(v->hwnd, px, w, h);
        float lit = cap ? StLitPct(px.data(), px.size()) : 0.0f;
        if (cap) StWritePng((dir + L"selftest-builtin.png").c_str(), px.data(), w, h, false);
        bool pass = cap && lit > 0.2f;
        out(L"builtin: mode=%s %dx%d fps=%.0f lit=%.1f%% -> %s",
            MODES[v->modeIdx].name, w, h, fps, lit, pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
    }

    // --- clip recording (built-in engine, BARS: bright at the bottom) ---
    {
        v->modeIdx = 0;
        for (int i = 0; i < 20; i++) { pump(); v->Render(); }
        v->ToggleRecord();
        if (!v->rec.Active()) {
            out(L"record: sink writer failed to start -> FAIL");
            rc = 1;
        } else {
            std::wstring mp4 = v->rec.Path();
            float r0 = v->Now();
            while (v->Now() - r0 < 1.6f) { pump(); v->Render(); }
            v->ToggleRecord();   // stop + finalize (joins the writer)
            double dur = 0; UINT32 vw = 0, vh = 0; bool hasAudio = false;
            bool frameOk = StVerifyMp4(mp4.c_str(), (dir + L"selftest-record-frame.png").c_str(),
                                       dur, vw, vh, hasAudio);
            bool pass = frameOk && dur > 0.8 && vw > 0 && hasAudio;
            out(L"record: %s dur=%.2fs %ux%u audio=%d frame=%d -> %s",
                mp4.c_str(), dur, vw, vh, hasAudio ? 1 : 0, frameOk ? 1 : 0,
                pass ? L"PASS" : L"FAIL");
            if (!pass) rc = 1;
        }
        v->modeIdx = keepMode;
    }

    // A worker may stop without Stop() when a window becomes too small.
    // Restart must reap that thread rather than std::terminate the process.
    {
        Recorder recorder;
        bool pass = recorder.Start(128, 128, 48000, nullptr);
        std::vector<uint8_t> pixels(128 * 128 * 4, 80);
        if (pass) {
            recorder.PushVideo(pixels.data(), 128, 128);
            recorder.PushVideo(pixels.data(), 64, 64);
            ULONGLONG deadline = GetTickCount64() + 5000;
            while (recorder.Active() && GetTickCount64() < deadline) Sleep(10);
            pass &= !recorder.Active();
            pass &= recorder.Start(128, 128, 48000, nullptr);
            recorder.PushVideo(pixels.data(), 128, 128);
            recorder.Stop();
        }
        out(L"record: restart after automatic stop -> %s", pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
    }

    // --- Spout output (shared-texture roundtrip, in-process receiver) ---
    {
        v->modeIdx = 0;   // BARS again: obviously-lit frames
        v->ToggleSpout();
        if (!v->spout.on) {
            out(L"spout: sender failed to start -> FAIL");
            rc = 1;
        } else {
            spoutDX rx;
            bool rxOk = rx.OpenDirectX11();
            rx.SetReceiverName("WaveScope");
            std::vector<uint8_t> px;
            unsigned int rw = 0, rh = 0;
            bool got = false;
            float lit = 0;
            // Render/pump until the receiver connects, resizes, and a real
            // frame lands (the double-staging pipeline needs a few frames).
            for (int i = 0; i < 120 && rxOk; i++) {
                pump(); v->Render();
                if (rx.ReceiveImage(px.empty() ? nullptr : px.data(), rw, rh)) {
                    if (rx.IsUpdated()) {
                        rw = rx.GetSenderWidth();
                        rh = rx.GetSenderHeight();
                        px.assign((size_t)rw * rh * 4, 0);
                    } else if (!px.empty() && i > 20) {
                        lit = StLitPct(px.data(), px.size());
                        if (lit > 0.2f) { got = true; break; }
                    }
                }
            }
            bool pass = rxOk && got && rw > 0 && rh > 0;
            out(L"spout: sender=WaveScope %ux%u lit=%.1f%% roundtrip=%d -> %s",
                rw, rh, lit, got ? 1 : 0, pass ? L"PASS" : L"FAIL");
            if (!pass) rc = 1;
            rx.ReleaseReceiver();
            rx.CloseDirectX11();
            v->ToggleSpout();
        }
        v->modeIdx = keepMode;
    }

    // --- GPU shader engine ---
    v->ToggleShader();
    if (!v->shaderActive || !v->shaderHost.ready) {
        out(L"shader: engine failed to start -> FAIL"); rc = 1;
    } else {
        int compiled = v->shaderHost.CompiledCount();
        if (compiled != v->shaderHost.ShaderCount()) {
            out(L"shader: only %d/%d programs compiled -> FAIL (see debugger output)",
                compiled, v->shaderHost.ShaderCount());
            rc = 1;
        }
        int keepShader = v->shaderIdx;
        for (int si = 0; si < v->shaderHost.ShaderCount(); si++) {
            v->shaderIdx = si;
            for (int k = 0; k < 8; k++) { pump(); v->Render(); }
        }
        out(L"shader: swept all %d shaders (%d compiled)", v->shaderHost.ShaderCount(), compiled);
        // Capture the two texture-fed shaders (FFT + waveform paths) and one
        // pure-field shader; each must produce a lit frame.
        const int capIdx[3] = { 14 /*Spectrum bars*/, 16 /*Scope lines*/, 1 /*Warp tunnel*/ };
        const wchar_t* capName[3] = { L"specbars", L"scopelines", L"warptunnel" };
        for (int c = 0; c < 3; c++) {
            v->shaderIdx = capIdx[c] % v->shaderHost.ShaderCount();
            for (int i = 0; i < 40; i++) { pump(); v->Render(); }
            AudioFrame f; g_tap.Analyze(f, v->Now());
            v->shaderHost.RenderEngine(v->shaderIdx, f);   // render, don't present
            std::vector<uint8_t> px; int w = 0, h = 0;
            bool cap = v->shaderHost.ReadBack(px, w, h);
            float lit = cap ? StLitPct(px.data(), px.size()) : 0.0f;
            if (cap) StWritePng((dir + L"selftest-shader-" + capName[c] + L".png").c_str(),
                                px.data(), w, h, true);
            bool pass = cap && lit > 0.2f;
            out(L"shader: %s %dx%d lit=%.1f%% -> %s",
                v->shaderHost.ShaderName(v->shaderIdx), w, h, lit, pass ? L"PASS" : L"FAIL");
            if (!pass) rc = 1;
        }
        v->shaderIdx = keepShader;
        v->ToggleShader();
    }

    // --- projectM engine ---
    v->ToggleProjectM();
    if (!v->pmActive || !v->pmHost.ready) {
        out(L"projectm: engine failed to start -> FAIL"); rc = 1;
    } else {
        for (int i = 0; i < 60; i++) { pump(); v->Render(); }
        // Real native browser: filtering, persisted identity, and shuffle pool.
        v->OpenPresetBrowser();
        SetDlgItemTextW(v->presetDialog, IDC_PRESET_QUERY, L"NEBULA");
        bool browserPass = v->browserPaths.size() == 1 &&
            v->browserPaths[0].find(L"nebula") != std::wstring::npos;
        v->PresetBrowserCommand(IDC_PRESET_FAVORITE, BN_CLICKED);
        v->presetFavorites.shuffleOnly = true;
        int favoritePick = v->RandomPresetOther();
        browserPass &= favoritePick >= 0 && v->IsPresetFavorite(v->pmHost.presetPaths[favoritePick]);
        v->pmHost.LoadPreset(favoritePick, false);
        browserPass &= v->RandomPresetOther() == -1; // one favorite: hold, don't escape the pool
        SetDlgItemTextW(v->presetDialog, IDC_PRESET_QUERY, L"no such preset 937462");
        browserPass &= v->browserPaths.empty() && !IsWindowEnabled(GetDlgItem(v->presetDialog, IDOK));
        wchar_t favoriteTemp[MAX_PATH], favoriteRoot[MAX_PATH];
        GetTempPathW(MAX_PATH, favoriteRoot); GetTempFileNameW(favoriteRoot, L"wsf", 0, favoriteTemp);
        v->presetFavorites.Save(favoriteTemp);
        PresetFavorites restored; restored.Load(favoriteTemp);
        browserPass &= restored.keys == v->presetFavorites.keys && restored.shuffleOnly;
        DeleteFileW(favoriteTemp);
        browserPass &= WsPresetKey(L"C:\\old\\presets\\X.milk", L"C:\\old", L"C:\\old") ==
                       WsPresetKey(L"D:\\new\\presets\\x.milk", L"D:\\new", L"D:\\new");
        DestroyWindow(v->presetDialog); v->presetFavorites.keys.clear(); v->presetFavorites.shuffleOnly = false;
        v->pmHost.LoadPreset(v->pmHost.PreferredStart(), false);
        out(L"presets: native search, no matches, portable favorites, persisted shuffle pool -> %s", browserPass ? L"PASS" : L"FAIL");
        if (!browserPass) rc = 1;
        float pcm[4800];
        int n = g_tap.ReadNew(v->pmCursor, pcm, 4800);
        v->pmHost.RenderEngine(pcm, n);
        std::vector<uint8_t> px; int w = 0, h = 0;
        bool cap = v->pmHost.ReadBack(px, w, h);
        float lit = cap ? StLitPct(px.data(), px.size()) : 0.0f;
        if (cap) StWritePng((dir + L"selftest-projectm.png").c_str(), px.data(), w, h, true);
        // projectM presets can legitimately open near-black; require capture only,
        // and report lit% for the human eye.
        bool pass = cap;
        out(L"projectm: preset=%s presets=%d %dx%d lit=%.1f%% -> %s",
            v->pmHost.PresetName(), v->pmHost.PresetCount(), w, h, lit,
            pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;

        // MILK flavor: the bundled classic MilkDrop library. Assert the pack
        // is present and sweep a spread of originals through the real parser
        // (ns-eel heavy) as a crash/hang smoke test.
        v->EnterProjectM(true);
        int classics = v->pmHost.PresetCount();
        if (classics < 500) {
            out(L"milk: classic library missing or short (%d presets) -> FAIL", classics);
            rc = 1;
        } else {
            for (int k = 0; k < 12; k++) {
                v->pmHost.LoadPreset(k * (classics / 12), false);
                for (int i = 0; i < 3; i++) { pump(); v->Render(); }
            }
            std::vector<uint8_t> cpx; int cw2 = 0, ch2 = 0;
            float pcm2[4800];
            int n2 = g_tap.ReadNew(v->pmCursor, pcm2, 4800);
            v->pmHost.RenderEngine(pcm2, n2);
            bool ccap = v->pmHost.ReadBack(cpx, cw2, ch2);
            float clit = ccap ? StLitPct(cpx.data(), cpx.size()) : 0.0f;
            if (ccap) StWritePng((dir + L"selftest-milk-classic.png").c_str(),
                                 cpx.data(), cw2, ch2, true);
            out(L"milk: %d classics swept, preset=%s lit=%.1f%% -> %s",
                classics, v->pmHost.PresetName(), clit, ccap ? L"PASS" : L"FAIL");
            if (!ccap) rc = 1;
        }
        v->EnterProjectM(false);   // back to the bundled WaveScope presets
        v->ToggleProjectM();
    }

    // --- morph transition (deliberate soft cut) ---
    // The deck's morph button, VJ lulls and timed shuffle blend presets with
    // projectM's soft cut. Native shipped hard cuts everywhere because that
    // cross-fade historically flashed white on switch; this drives the real
    // morph across a spread of preset pairs and durations in BOTH libraries
    // (bundled + the 551 classics, where the diverse init states most likely
    // provoke a flash), asserts it renders, and logs the peak near-white pixel
    // share right after each switch so any flash shows up in the log (it would
    // spike far past both endpoints).
    v->ToggleProjectM();
    if (!v->pmActive || !v->pmHost.ready) {
        out(L"morph: engine failed to start -> FAIL"); rc = 1;
    } else {
        // Mean luminance (0..255) and near-white pixel share (all channels
        // > 235) of a BGRA readback — a white flash pushes both toward the top.
        auto stats = [](const uint8_t* bgra, size_t bytes, float& meanLum, float& nearWhite) {
            size_t px = bytes / 4; double sum = 0; size_t white = 0;
            for (size_t i = 0; i + 3 < bytes; i += 4) {
                int b = bgra[i], g = bgra[i + 1], r = bgra[i + 2];
                sum += (r + g + b) / 3.0;
                if (r > 235 && g > 235 && b > 235) white++;
            }
            meanLum = px ? (float)(sum / (double)px) : 0.0f;
            nearWhite = px ? 100.0f * (float)white / (float)px : 0.0f;
        };
        // Hard-cut to `idx`, let its phosphor trails settle, and report the
        // settled near-white share (the endpoint baseline to compare against).
        auto settleWhite = [&](int idx) {
            v->pmHost.LoadPreset(idx, false);
            for (int i = 0; i < 30; i++) { pump(); v->Render(); }
            float pcm[4800]; int nn = g_tap.ReadNew(v->pmCursor, pcm, 4800);
            v->pmHost.RenderEngine(pcm, nn);
            std::vector<uint8_t> px; int w = 0, h = 0; float ml = 0, nw = 0;
            if (v->pmHost.ReadBack(px, w, h)) stats(px.data(), px.size(), ml, nw);
            return nw;
        };
        bool morphRendered = false;
        float worstFlash = 0;   // peak (transition nearWhite - endpoint nearWhite)
        // Sweep the current library: a spread of preset pairs, each morphed at
        // three durations; log per-run flash and track the worst.
        auto sweep = [&](const wchar_t* lib) {
            int n = v->pmHost.PresetCount();
            if (n < 2) { out(L"morph[%s]: <2 presets, skipped", lib); return; }
            const double durs[] = { 0.5, 3.0 };   // short + typical cross-fade
            int pairs[2][2] = { { 0, n / 2 }, { n / 3, n - 1 } };
            for (auto& pr : pairs) {
                int a = ((pr[0] % n) + n) % n, b = ((pr[1] % n) + n) % n;
                if (a == b) continue;
                float baseW = settleWhite(a), wb = settleWhite(b);
                if (wb > baseW) baseW = wb;   // flash vs the brighter endpoint
                for (double dur : durs) {
                    settleWhite(a);   // start each run settled on A
                    v->pmHost.MorphToPreset(b, dur);
                    // Sample the frames right after the switch — where the
                    // historic flash lived (incoming preset blended pre-init).
                    float peakW = 0, peakLum = 0; int peakFrame = -1;
                    for (int i = 0; i < 40; i++) {
                        pump();
                        float pcm[4800]; int nn = g_tap.ReadNew(v->pmCursor, pcm, 4800);
                        v->pmHost.RenderEngine(pcm, nn);
                        std::vector<uint8_t> px; int w = 0, h = 0;
                        if (v->pmHost.ReadBack(px, w, h)) {
                            float ml = 0, nw = 0; stats(px.data(), px.size(), ml, nw);
                            if (nw > peakW) { peakW = nw; peakFrame = i; }
                            if (ml > peakLum) peakLum = ml;
                            if (ml > 2.0f) morphRendered = true;
                        }
                    }
                    float flash = peakW - baseW;
                    if (flash > worstFlash) worstFlash = flash;
                    out(L"morph[%s]: %s -> %s dur=%.1fs endWhite=%.1f%% peakWhite=%.1f%%@f%d peakLum=%.0f flash=%.1f%%",
                        lib, v->pmHost.PresetNameAt(a), v->pmHost.PresetNameAt(b),
                        dur, baseW, peakW, peakFrame, peakLum, flash);
                }
            }
        };
        v->EnterProjectM(false); sweep(L"projm");   // bundled WaveScope presets
        v->EnterProjectM(true);  sweep(L"milk");     // the 551 classics
        v->EnterProjectM(false);
        // The morph must render a real transition, and the soft cut must stay
        // flash-free: measured peak near-white share sits near 0 (endpoints'
        // own brightness aside), so a generous 25% ceiling is a regression
        // tripwire — if a projectM update reintroduces the pre-init white
        // flash, this goes red and the app-level dissolve is back on the table.
        bool pass = morphRendered && worstFlash < 25.0f;
        out(L"morph: rendered=%d worstFlash=%.1f%% -> %s",
            morphRendered ? 1 : 0, worstFlash, pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
        v->ToggleProjectM();
    }

    // --- preset lab apply/reject contract ---
    // The lab applies edited .milk text as a hard cut and reports rejections so
    // a bad edit keeps the previous preset up. projectM tolerates lenient junk
    // (loads it as a degenerate preset) but rejects genuine eel syntax errors
    // synchronously, with a message (often a line/col) — the case the lab
    // actually guards. Assert both directions and that the engine survives.
    v->ToggleProjectM();
    if (!v->pmActive || !v->pmHost.ready) {
        out(L"lab: engine failed to start -> FAIL"); rc = 1;
    } else {
        v->EnterProjectM(false);
        for (int i = 0; i < 20; i++) { pump(); v->Render(); }
        std::wstring original = WsLabLines(v->pmHost.CurrentPresetText());
        SendMessageW(v->hwnd, WM_KEYDOWN, 'L', 0);
        SendMessageW(v->hwnd, WM_CHAR, 'l', 0);
        bool inputPass = v->labPanel && v->labDialog && v->labText == original;
        RECT client; GetClientRect(v->hwnd, &client);
        v->BuildOverlay(client.right, client.bottom, L"fixed", L"");
        HWND editor = GetDlgItem(v->labDialog, IDC_LAB_EDITOR);
        SendMessageW(editor, EM_SETSEL, 0, 0);
        SendMessageW(editor, EM_REPLACESEL, TRUE, (LPARAM)L"z");
        bool editPass = v->labText == L"z" + original && v->labText != v->labSavedText;
        SendMessageW(editor, EM_UNDO, 0, 0);
        editPass &= v->labText == original && v->labText == v->labSavedText;
        SendMessageW(editor, EM_REDO, 0, 0);
        editPass &= v->labText == L"z" + original;
        SendMessageW(editor, EM_SETSEL, 0, 1);
        SendMessageW(editor, EM_REPLACESEL, TRUE, (LPARAM)L"\x03a9\xd83d\xde00");
        editPass &= v->labText == L"\x03a9\xd83d\xde00" + original;
        editPass &= !v->CloseLab(IDCANCEL) && IsWindow(v->labDialog);
        SetDlgItemTextW(v->labDialog, IDC_LAB_NAME, L"CON");
        editPass &= !v->CloseLab(IDYES) && IsWindow(v->labDialog) && v->labStatusErr;
        // Restore the known source for a representative editor capture.
        SetWindowTextW(editor, original.c_str()); v->SyncLabEditor();
        SetDlgItemTextW(v->labDialog, IDC_LAB_NAME, L"wavescope-tunnel edit");
        v->labStatus = L"Ctrl+Enter applies; Ctrl+S saves a new preset."; v->RefreshLabStatus();
        std::vector<uint8_t> labPixels; int lw = 0, lh = 0;
        if (StCaptureWindow(v->labDialog, labPixels, lw, lh))
            StWritePng((dir + L"selftest-lab-native.png").c_str(), labPixels.data(), lw, lh, false);
        v->CloseLab(IDNO);
        bool closeRedraw = v->BuildOverlay(client.right, client.bottom, L"fixed", L"");
        bool guiPass = inputPass && editPass && closeRedraw && IsWindowEnabled(v->hwnd);
        out(L"gui: native editor selection, Unicode, undo/redo, cancel and failed-save guard -> %s", guiPass ? L"PASS" : L"FAIL");
        if (!guiPass) rc = 1;
        // Exercise the actual Save As command, using a new isolated data folder.
        wchar_t tempRoot[MAX_PATH], tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempRoot); GetTempFileNameW(tempRoot, L"wsl", 0, tempDir);
        DeleteFileW(tempDir); CreateDirectoryW(tempDir, nullptr);
        AppPaths savedPaths = g_paths; g_paths.data = std::wstring(tempDir) + L"\\";
        v->labText = original; v->labSaveName = L"save-test";
        v->LabSaveAs();
        bool savePass = !v->labStatusErr;
        v->labText = L"must not overwrite the first save";
        v->LabSaveAs();
        savePass &= v->labStatusErr && v->labStatus.find(L"already exists") != std::wstring::npos;
        std::wstring userDir = g_paths.data + L"presets\\user";
        std::wstring savedFile = userDir + L"\\save-test.milk";
        FILE* saved = _wfopen(savedFile.c_str(), L"rb");
        std::string stored;
        if (saved) { char chunk[4096]; size_t n; while ((n = fread(chunk, 1, sizeof(chunk), saved)) != 0) stored.append(chunk, n); fclose(saved); }
        savePass &= stored == App::WideToUtf8(original);
        v->EnterProjectM(false); v->libraryFolder.clear();
        DeleteFileW(savedFile.c_str()); RemoveDirectoryW(userDir.c_str());
        RemoveDirectoryW((g_paths.data + L"presets").c_str()); RemoveDirectoryW(tempDir);
        g_paths = savedPaths;
        out(L"lab: Save As refuses overwrite and preserves original bytes -> %s", savePass ? L"PASS" : L"FAIL");
        if (!savePass) rc = 1;

        // A watched library can shrink while its last preset is active. The
        // lab must not index the removed row, or silently edit its replacement.
        GUID reloadGuid; CoCreateGuid(&reloadGuid);
        wchar_t reloadId[40]; StringFromGUID2(reloadGuid, reloadId, 40);
        std::wstring reloadDir = std::wstring(tempRoot) + L"WaveScope-reload-" + reloadId;
        std::string presetText = App::WideToUtf8(original);
        bool reloadPass = WsSaveNewPreset(reloadDir, L"a.milk", presetText) == ERROR_SUCCESS &&
            WsSaveNewPreset(reloadDir, L"b.milk", presetText) == ERROR_SUCCESS;
        if (reloadPass) {
            v->pmHost.SetPresetFolder(reloadDir.c_str()); v->pmHost.LoadPreset(1, false);
            reloadPass &= v->pmHost.PresetIndex() == 1;
            reloadPass &= DeleteFileW((reloadDir + L"\\b.milk").c_str()) != FALSE;
            reloadPass &= WaitForSingleObject(v->pmHost.presetWatch, 2000) == WAIT_OBJECT_0;
            v->pmHost.PollReload();
            bool detached = v->pmHost.PresetCount() == 1 && v->pmHost.PresetIndex() == -1;
            reloadPass &= detached;
            // Avoid dereferencing the known stale index in an unfixed build.
            if (detached) reloadPass &= v->pmHost.CurrentPresetText().empty();
            v->pmHost.NextPreset(1);
            reloadPass &= v->pmHost.PresetIndex() == 0 && !v->pmHost.CurrentPresetText().empty();
        }
        out(L"projectm: removing the active preset detaches it safely -> %s", reloadPass ? L"PASS" : L"FAIL");
        if (!reloadPass) rc = 1;
        bool rejectPass = WsSaveNewPreset(reloadDir, L"bad.milk", "[preset00]\nper_frame_1=zoom = 1 +* ;\n") == ERROR_SUCCESS;
        rejectPass &= WsSaveNewPreset(reloadDir, L"c.milk", presetText) == ERROR_SUCCESS;
        v->pmHost.SetPresetFolder(reloadDir.c_str()); v->pmHost.LoadPreset(0, false);
        v->pmHost.LoadPreset(v->pmHost.PresetIndexByName(L"bad.milk"), false);
        rejectPass &= v->pmHost.loadFailed && v->pmHost.curName == L"a.milk" && v->pmHost.PresetIndex() == 0;
        v->pmHost.NextPreset(1);
        rejectPass &= !v->pmHost.loadFailed && v->pmHost.curName == L"c.milk" && v->pmHost.PresetIndex() == 2;
        out(L"projectm: rejected preset preserves selection; navigation skips it -> %s", rejectPass ? L"PASS" : L"FAIL");
        if (!rejectPass) rc = 1;
        v->EnterProjectM(false);
        DeleteFileW((reloadDir + L"\\a.milk").c_str());
        DeleteFileW((reloadDir + L"\\b.milk").c_str());
        DeleteFileW((reloadDir + L"\\bad.milk").c_str());
        DeleteFileW((reloadDir + L"\\c.milk").c_str()); RemoveDirectoryW(reloadDir.c_str());
        std::wstring wtext = v->pmHost.CurrentPresetText();
        int u8n = WideCharToMultiByte(CP_UTF8, 0, wtext.data(), (int)wtext.size(), nullptr, 0, nullptr, nullptr);
        std::string valid((size_t)(u8n > 0 ? u8n : 0), '\0');
        if (u8n > 0) WideCharToMultiByte(CP_UTF8, 0, wtext.data(), (int)wtext.size(), &valid[0], u8n, nullptr, nullptr);
        bool okApply = v->pmHost.ApplyData(valid.c_str(), false);          // valid: accepted
        bool okBad = v->pmHost.ApplyData("[preset00]\nper_frame_1=zoom = 1 +* ;\n", false); // rejected
        bool haveMsg = !v->pmHost.loadFailMsg.empty();
        bool captured;
        {
            float pcm[4800]; int nn = g_tap.ReadNew(v->pmCursor, pcm, 4800);
            v->pmHost.RenderEngine(pcm, nn);
            std::vector<uint8_t> px; int w = 0, h = 0;
            captured = v->pmHost.ReadBack(px, w, h);   // still renders after the reject
        }
        bool pass = okApply && !okBad && haveMsg && captured;
        out(L"lab: apply-valid=%d reject-bad-eel=%d msg=%d captured=%d -> %s",
            okApply ? 1 : 0, !okBad ? 1 : 0, haveMsg ? 1 : 0, captured ? 1 : 0,
            pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
        v->ToggleProjectM();
    }

    // --- auto-VJ detector (drop -> hop) ---
    // Drive the VJ detector on a synthetic clock with a controlled bass
    // envelope: a quiet-but-present baseline must hold (no drop), then a spike
    // past the 8 s hold must fire a drop that hops the preset.
    v->ToggleProjectM();
    if (v->pmActive && v->pmHost.ready) {
        v->EnterProjectM(false);
        for (int i = 0; i < 10; i++) { pump(); v->Render(); }
        v->ArmVj();
        AudioFrame f{};
        float t = 100.0f;
        int startIdx = v->pmHost.PresetIndex();
        for (int i = 0; i < 90; i++) { f.bass = 0.05f; v->VjTick(f, t); t += 0.15f; }  // ~13 s quiet
        int quietIdx = v->pmHost.PresetIndex();
        for (int i = 0; i < 8; i++)  { f.bass = 0.60f; v->VjTick(f, t); t += 0.15f; }  // spike -> drop
        int dropIdx = v->pmHost.PresetIndex();
        bool pass = (quietIdx == startIdx) && (dropIdx != quietIdx);
        out(L"vj: quiet-hold=%d drop-hop=%d idx %d/%d/%d -> %s",
            quietIdx == startIdx ? 1 : 0, dropIdx != quietIdx ? 1 : 0,
            startIdx, quietIdx, dropIdx, pass ? L"PASS" : L"FAIL");
        if (!pass) rc = 1;
        v->ToggleProjectM();
    }

    out(L"selftest: %s", rc == 0 ? L"ALL PASS" : L"FAILURES (see above)");
    if (log) fclose(log);
    return rc;
}
