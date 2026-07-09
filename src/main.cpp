// SpectraViz native: phase-1 shell.
//
// A classic, dependency-free Windows app: Win32 window, Direct2D rendering,
// WASAPI loopback capture (hears whatever the machine plays), own FFT.
// Everything below links against OS DLLs only; /MT static CRT means the
// shipped artifact is one .exe with zero runtime requirements.
//
// Keys: SPACE/RIGHT next (mode or preset), LEFT prev, P palette,
// M toggle MilkDrop (projectM) engine, F fullscreen, H HUD, ESC quit.
//
// All 34 WaveScope modes are ported (viz_modes.inl) across six palettes, and
// the real MilkDrop engine runs via projectM (projectm_host.inl).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------- utilities

static const float PI  = 3.14159265358979f;
static const float TAU = 6.28318530717959f;

template <typename T> static void SafeRelease(T** p) {
    if (*p) { (*p)->Release(); *p = nullptr; }
}

// HSL -> RGB, matching the web palettes' hsla() usage.
static D2D1_COLOR_F Hsla(float h, float s, float l, float a) {
    h = fmodf(h, 360.0f) / 360.0f;
    if (h < 0) h += 1.0f;
    auto hue = [](float p, float q, float t) {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1.0f / 6) return p + (q - p) * 6 * t;
        if (t < 1.0f / 2) return q;
        if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
        return p;
    };
    float r, g, b;
    if (s <= 0.0f) {
        r = g = b = l;
    } else {
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
        float p = 2 * l - q;
        r = hue(p, q, h + 1.0f / 3);
        g = hue(p, q, h);
        b = hue(p, q, h - 1.0f / 3);
    }
    return D2D1::ColorF(r, g, b, a);
}

// The brand trace palette: ultramarine, position 0..1 -> lightness ramp.
static D2D1_COLOR_F Trace(float pos, float alpha) {
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    return Hsla(232.0f, 0.64f, 0.38f + pos * 0.47f, alpha);
}

// ------------------------------------------------------------------- audio

// One analyzed slice per rendered frame; mirrors the web AudioFrame.
struct AudioFrame {
    float fft[1024];   // 0..1, log-ish dB mapping like the Web Audio analyser
    float wave[2048];  // -1..1 mono time domain
    float level = 0, bass = 0, mid = 0, treble = 0, beat = 0;
    float t = 0, dt = 0.016f;
};

// WASAPI loopback capture into a mono ring buffer + FFT analysis on demand.
class AudioTap {
public:
    bool Start() {
        m_thread = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
        return m_thread != nullptr;
    }

    void Stop() {
        m_quit = true;
        if (m_thread) { WaitForSingleObject(m_thread, 2000); CloseHandle(m_thread); m_thread = nullptr; }
    }

    UINT32 SampleRate() const { return m_sampleRate; }

    // Snapshot the newest 2048 samples and analyze them.
    void Analyze(AudioFrame& f, float now) {
        float dt = now - m_lastT;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f)   dt = 0.1f;
        m_lastT = now;
        f.t = now;
        f.dt = dt;

        float window[2048];
        {
            AcquireSRWLockShared(&m_lock);
            uint64_t w = m_writeIdx;
            for (int i = 0; i < 2048; i++) {
                uint64_t idx = w + RING - 2048 + i;
                window[i] = m_ring[idx % RING];
            }
            ReleaseSRWLockShared(&m_lock);
        }
        for (int i = 0; i < 2048; i++) f.wave[i] = window[i];

        // Hann window + radix-2 FFT (2048 point).
        static float re[2048], im[2048];
        for (int i = 0; i < 2048; i++) {
            float hann = 0.5f * (1.0f - cosf(TAU * i / 2047.0f));
            re[i] = window[i] * hann;
            im[i] = 0.0f;
        }
        Fft2048(re, im);

        // Magnitudes -> dB -> 0..1 with the analyser's [-100,-30] dB range,
        // then per-bin exponential smoothing (0.8), matching the web engine.
        for (int i = 0; i < 1024; i++) {
            float mag = sqrtf(re[i] * re[i] + im[i] * im[i]) / 1024.0f;
            float db = 20.0f * log10f(mag + 1e-9f);
            float norm = (db + 100.0f) / 70.0f;
            if (norm < 0) norm = 0;
            if (norm > 1) norm = 1;
            m_smooth[i] = m_smooth[i] * 0.8f + norm * 0.2f;
            f.fft[i] = m_smooth[i];
        }

        // Band energies (split at 250 Hz / 4 kHz) + RMS level + beat.
        float sr = (float)(m_sampleRate ? m_sampleRate : 48000);
        float hzPerBin = sr / 2.0f / 1024.0f;
        auto band = [&](float lo, float hi) {
            int a = (int)fmaxf(0.0f, floorf(lo / hzPerBin));
            int b = (int)fminf(1023.0f, ceilf(hi / hzPerBin));
            float sum = 0;
            for (int i = a; i <= b; i++) sum += f.fft[i];
            return sum / (float)(b - a + 1);
        };
        f.bass   = band(20, 250);
        f.mid    = band(250, 4000);
        f.treble = band(4000, 16000);

        float rms = 0;
        for (int i = 0; i < 2048; i += 4) rms += window[i] * window[i];
        rms = sqrtf(rms / 512.0f);
        m_level += (rms - m_level) * 0.2f;
        f.level = m_level;

        m_bassAvg += (f.bass - m_bassAvg) * 0.04f;
        if (f.bass > m_bassAvg * 1.35f && f.bass > 0.08f) m_beat = 1.0f;
        m_beat *= expf(-dt * 6.0f);
        f.beat = m_beat;
    }

private:
    static const int RING = 8192;

    static DWORD WINAPI ThreadMain(LPVOID param) {
        return ((AudioTap*)param)->Run();
    }

    DWORD Run() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return 1;

        IMMDeviceEnumerator* devEnum = nullptr;
        IMMDevice* device = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* fmt = nullptr;

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&devEnum);
        if (SUCCEEDED(hr)) hr = devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
        if (SUCCEEDED(hr)) {
            m_sampleRate = fmt->nSamplesPerSec;
            // 100 ms buffer, shared mode, LOOPBACK: capture what plays.
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    1000000, 0, fmt, nullptr);
        }
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        if (SUCCEEDED(hr)) hr = client->Start();

        if (SUCCEEDED(hr)) {
            const int channels = fmt->nChannels;
            const bool isFloat =
                fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 ((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat.Data1 == 3 /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */);

            while (!m_quit) {
                Sleep(5);
                UINT32 packet = 0;
                while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

                    AcquireSRWLockExclusive(&m_lock);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        for (UINT32 i = 0; i < frames; i++)
                            m_ring[(m_writeIdx++) % RING] = 0.0f;
                    } else if (isFloat) {
                        const float* s = (const float*)data;
                        for (UINT32 i = 0; i < frames; i++) {
                            float mono = channels > 1 ? (s[i * channels] + s[i * channels + 1]) * 0.5f
                                                      : s[i];
                            m_ring[(m_writeIdx++) % RING] = mono;
                        }
                    } else { // 16-bit PCM fallback
                        const int16_t* s = (const int16_t*)data;
                        for (UINT32 i = 0; i < frames; i++) {
                            float l = s[i * channels] / 32768.0f;
                            float r = channels > 1 ? s[i * channels + 1] / 32768.0f : l;
                            m_ring[(m_writeIdx++) % RING] = (l + r) * 0.5f;
                        }
                    }
                    ReleaseSRWLockExclusive(&m_lock);
                    capture->ReleaseBuffer(frames);
                }
            }
            client->Stop();
        }

        if (fmt) CoTaskMemFree(fmt);
        SafeRelease(&capture);
        SafeRelease(&client);
        SafeRelease(&device);
        SafeRelease(&devEnum);
        CoUninitialize();
        return SUCCEEDED(hr) ? 0 : 1;
    }

    // Iterative radix-2 Cooley-Tukey, N=2048.
    static void Fft2048(float* re, float* im) {
        const int N = 2048, LOG2N = 11;
        for (int i = 0; i < N; i++) { // bit-reverse permutation
            unsigned r = 0, x = (unsigned)i;
            for (int b = 0; b < LOG2N; b++) { r = (r << 1) | (x & 1); x >>= 1; }
            if ((int)r > i) {
                float tr = re[i]; re[i] = re[r]; re[r] = tr;
                float ti = im[i]; im[i] = im[r]; im[r] = ti;
            }
        }
        for (int len = 2; len <= N; len <<= 1) {
            float ang = -TAU / (float)len;
            float wr = cosf(ang), wi = sinf(ang);
            for (int i = 0; i < N; i += len) {
                float cr = 1, ci = 0;
                for (int j = 0; j < len / 2; j++) {
                    int a = i + j, b = i + j + len / 2;
                    float tr = re[b] * cr - im[b] * ci;
                    float ti = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                    float ncr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = ncr;
                }
            }
        }
    }

    HANDLE m_thread = nullptr;
    std::atomic<bool> m_quit{false};
    SRWLOCK m_lock = SRWLOCK_INIT;
    float m_ring[RING] = {};
    uint64_t m_writeIdx = 0;
    UINT32 m_sampleRate = 48000;
    float m_smooth[1024] = {};
    float m_level = 0, m_bassAvg = 0, m_beat = 0, m_lastT = 0;
};

// ------------------------------------------------------------------- modes

// Log-axis FFT sampling, matching the web engine's bin() helper. Defined here
// (before the include) because the ported modes rely on it.
static float BinLog(const AudioFrame& f, float pos) {
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    int idx = (int)powf(1023.0f, pos * 0.92f + 0.08f);
    if (idx > 1023) idx = 1023;
    return f.fft[idx];
}

// Palettes, the DrawCtx primitives, all 34 modes, and the MODES[] registry.
#include "viz_modes.inl"

// The projectM (MilkDrop) engine host: child GL window + libprojectM.
#include "projectm_host.inl"

// --------------------------------------------------------------------- app

struct App {
    HWND hwnd = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* hudFont = nullptr;
    AudioTap tap;
    HINSTANCE inst = nullptr;
    ProjectMHost pmHost;
    bool pmActive = false;
    int modeIdx = 0;
    int paletteIdx = 0;
    bool hud = true;
    bool fullscreen = false;
    WINDOWPLACEMENT savedPlacement = { sizeof(WINDOWPLACEMENT) };
    LARGE_INTEGER qpcFreq = {}, qpcStart = {};
    int fpsFrames = 0;
    float fpsWindowStart = 0;
    int fps = 0;

    float Now() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (float)((double)(t.QuadPart - qpcStart.QuadPart) / (double)qpcFreq.QuadPart);
    }

    bool CreateGraphics() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       (IUnknown**)&dwrite))) return false;
        dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 14.0f, L"en-us", &hudFont);
        return CreateTarget();
    }

    bool CreateTarget() {
        SafeRelease(&brush);
        SafeRelease(&rt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        // RETAIN_CONTENTS keeps last frame's pixels: phosphor trails are a
        // translucent black wash, exactly like the web engine's fade.
        HRESULT hr = d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size, D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS),
            &rt);
        if (FAILED(hr)) return false;
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        return true;
    }

    void Render() {
        if (!rt) return;
        D2D1_SIZE_F size = rt->GetSize();
        float now = Now();

        AudioFrame frame;
        tap.Analyze(frame, now);

        // projectM engine: render OpenGL on the child window instead of D2D.
        if (pmActive && pmHost.ready) {
            pmHost.RenderFrame(frame);
            fpsFrames++;
            if (now - fpsWindowStart >= 1.0f) { fps = fpsFrames; fpsFrames = 0; fpsWindowStart = now; }
            return;
        }

        const Mode& mode = MODES[modeIdx];

        rt->BeginDraw();
        // Fade wash (instrument black #0B0D0C).
        brush->SetColor(D2D1::ColorF(0.043f, 0.051f, 0.047f, mode.fade));
        rt->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), brush);

        DrawCtx g{ rt, d2d, brush, PALETTES[paletteIdx].color, size.width, size.height, &frame };
        mode.draw(g);

        if (hud && hudFont) {
            wchar_t text[256];
            swprintf_s(text, L"SPECTRAVIZ  MODE %02d/%d %s  TRACE %s  FPS %d  LEVEL %.2f\nSPACE next  \x2190 prev  P palette  F fullscreen  H hud  ESC quit",
                       modeIdx + 1, MODE_COUNT, mode.name, PALETTES[paletteIdx].name, fps, frame.level);
            brush->SetColor(D2D1::ColorF(0.95f, 0.96f, 0.95f, 0.75f));
            rt->DrawTextW(text, (UINT32)wcslen(text), hudFont,
                          D2D1::RectF(16, 12, size.width - 16, 80), brush);
        }

        HRESULT hr = rt->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET) CreateTarget();

        fpsFrames++;
        if (now - fpsWindowStart >= 1.0f) {
            fps = fpsFrames;
            fpsFrames = 0;
            fpsWindowStart = now;
        }
    }

    void ToggleFullscreen() {
        fullscreen = !fullscreen;
        DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        if (fullscreen) {
            GetWindowPlacement(hwnd, &savedPlacement);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        } else {
            SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(hwnd, &savedPlacement);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                         SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
    }

    // Switch between the built-in Direct2D modes and the projectM engine.
    void ToggleProjectM() {
        if (!pmActive) {
            if (!pmHost.ready && !pmHost.Init(inst, hwnd)) {
                SetWindowTextW(hwnd, L"SpectraViz  (projectM failed to start)");
                return;
            }
            RECT rc; GetClientRect(hwnd, &rc);
            pmHost.Resize(rc.right, rc.bottom);
            pmHost.Show(true);
            pmActive = true;
            SetWindowTextW(hwnd, L"SpectraViz  projectM (M to exit, SPACE next preset)");
        } else {
            pmActive = false;
            pmHost.Show(false);
            SetWindowTextW(hwnd, L"SpectraViz");
        }
    }
};

static App g_app;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_app.rt) {
            g_app.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        }
        if (g_app.pmHost.ready) g_app.pmHost.Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:
        case VK_RIGHT:
            if (g_app.pmActive) g_app.pmHost.NextPreset(1);
            else g_app.modeIdx = (g_app.modeIdx + 1) % MODE_COUNT;
            break;
        case VK_LEFT:
            if (g_app.pmActive) g_app.pmHost.NextPreset(-1);
            else g_app.modeIdx = (g_app.modeIdx + MODE_COUNT - 1) % MODE_COUNT;
            break;
        case 'P':      g_app.paletteIdx = (g_app.paletteIdx + 1) % PAL_COUNT; break;
        case 'M':      g_app.ToggleProjectM(); break;
        case 'F':      g_app.ToggleFullscreen(); break;
        case 'H':      g_app.hud = !g_app.hud; break;
        case VK_ESCAPE: PostQuitMessage(0); break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    g_app.inst = hInstance;
    QueryPerformanceFrequency(&g_app.qpcFreq);
    QueryPerformanceCounter(&g_app.qpcStart);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SpectraVizWindow";
    RegisterClassW(&wc);

    g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"SpectraViz",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1440, 900, nullptr, nullptr, hInstance, nullptr);
    if (!g_app.hwnd) return 1;

    if (!g_app.CreateGraphics()) return 1;
    g_app.tap.Start();

    ShowWindow(g_app.hwnd, nCmdShow);

    // Launch straight into the MilkDrop engine with "milkdrop" on the command line.
    if (pCmdLine && wcsstr(pCmdLine, L"milkdrop")) g_app.ToggleProjectM();

    // Render loop: D2D presents on vsync, so this paces itself at 60.
    MSG msg = {};
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        g_app.Render();
    }
done:
    g_app.pmHost.Destroy();
    g_app.tap.Stop();
    SafeRelease(&g_app.brush);
    SafeRelease(&g_app.rt);
    SafeRelease(&g_app.hudFont);
    SafeRelease(&g_app.dwrite);
    SafeRelease(&g_app.d2d);
    return 0;
}
