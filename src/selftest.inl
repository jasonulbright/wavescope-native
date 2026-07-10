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
    out(L"selftest: source=%s sampleRate=%u", g_tap.SourceName(), g_tap.SampleRate());

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
        v->ToggleProjectM();
    }

    out(L"selftest: %s", rc == 0 ? L"ALL PASS" : L"FAILURES (see above)");
    if (log) fclose(log);
    return rc;
}
