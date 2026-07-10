// WaveScope native: phase-1 shell.
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
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

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
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

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

    enum { SRC_LOOPBACK = 0, SRC_MIC = 1, SRC_DEMO = 2, SRC_FILE = 3 };
    void SetSource(int s) { m_source = s; }
    int Source() const { return m_source.load(); }
    const wchar_t* SourceName() const {
        switch (m_source.load()) {
        case SRC_MIC:  return L"MICROPHONE";
        case SRC_DEMO: return L"DEMO";
        case SRC_FILE: return L"AUDIO FILE";
        default:       return L"SYSTEM AUDIO";
        }
    }
    // Play + analyze a decoded audio file (Media Foundation).
    void SetFile(const wchar_t* path) {
        AcquireSRWLockExclusive(&m_lock);
        wcsncpy_s(m_filePath, path, _TRUNCATE);
        ReleaseSRWLockExclusive(&m_lock);
        m_source = SRC_FILE;
    }

    // Snapshot the newest 2048 samples and analyze them.
    void Analyze(AudioFrame& f, float now) {
        float dt = now - m_lastT;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.1f)   dt = 0.1f;
        m_lastT = now;
        f.t = now;
        f.dt = dt;

        float window[2048];
        if (m_source.load() == SRC_DEMO) {
            // Synthesize a coherent window ending at the current time.
            float sr = (float)(m_sampleRate ? m_sampleRate : 48000);
            for (int i = 0; i < 2048; i++)
                window[i] = DemoSynth((double)now - (double)(2048 - i) / sr);
        } else {
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

    // The demo signal: a slow chord (saw + sine partials) with a bass kick,
    // so the app is never dead-silent when no real audio is playing.
    static float DemoSynth(double t) {
        static const float roots[4] = { 110.0f, 130.81f, 98.0f, 146.83f };
        int bar = (int)(t / 2.0);
        float root = roots[((bar % 4) + 4) % 4];
        const float ratios[5] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.02f };
        float s = 0.0f;
        for (int k = 0; k < 5; k++) {
            float freq = root * ratios[k];
            float ph = (float)(t * freq - floor(t * freq));
            float osc = ratios[k] < 2.0f ? (2.0f * ph - 1.0f) : sinf(TAU * ph);
            s += osc * (0.18f / ratios[k]);
        }
        double kt = t - floor(t / 0.5) * 0.5;      // 0..0.5, kick every 500 ms
        s += sinf(TAU * 55.0f * (float)t) * expf(-(float)kt * 9.0f) * 0.7f;
        return s * 0.6f;
    }

    // Capture thread: (re)opens WASAPI for loopback or mic, and reinitializes
    // when the source changes. Demo needs no capture (Analyze synthesizes).
    DWORD Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        while (!m_quit) {
            int src = m_source.load();
            if (src == SRC_DEMO) { Sleep(30); continue; }
            if (src == SRC_FILE) { RunFile(); continue; }

            IMMDeviceEnumerator* devEnum = nullptr;
            IMMDevice* device = nullptr;
            IAudioClient* client = nullptr;
            IAudioCaptureClient* capture = nullptr;
            WAVEFORMATEX* fmt = nullptr;

            EDataFlow flow = (src == SRC_MIC) ? eCapture : eRender;
            DWORD streamFlags = (src == SRC_MIC) ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;

            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator), (void**)&devEnum);
            if (SUCCEEDED(hr)) hr = devEnum->GetDefaultAudioEndpoint(flow, eConsole, &device);
            if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
            if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
            if (SUCCEEDED(hr)) {
                m_sampleRate = fmt->nSamplesPerSec;
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 1000000, 0, fmt, nullptr);
            }
            if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
            if (SUCCEEDED(hr)) hr = client->Start();

            if (SUCCEEDED(hr)) {
                const int channels = fmt->nChannels;
                const bool isFloat =
                    fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     ((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat.Data1 == 3);

                while (!m_quit && m_source.load() == src) {
                    Sleep(5);
                    UINT32 packet = 0;
                    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                        BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                        AcquireSRWLockExclusive(&m_lock);
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            for (UINT32 i = 0; i < frames; i++) m_ring[(m_writeIdx++) % RING] = 0.0f;
                        } else if (isFloat) {
                            const float* s = (const float*)data;
                            for (UINT32 i = 0; i < frames; i++)
                                m_ring[(m_writeIdx++) % RING] =
                                    channels > 1 ? (s[i * channels] + s[i * channels + 1]) * 0.5f : s[i];
                        } else {
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
            if (FAILED(hr)) Sleep(200); // failed to open: pause before retry
        }
        MFShutdown();
        CoUninitialize();
        return 0;
    }

    // Decode a file with Media Foundation, render it to the default output,
    // and feed the same samples (mono) into the analysis ring in sync. Loops
    // the file. Runs until the source changes or the app quits.
    void RunFile() {
        wchar_t path[MAX_PATH];
        AcquireSRWLockShared(&m_lock);
        wcsncpy_s(path, m_filePath, _TRUNCATE);
        ReleaseSRWLockShared(&m_lock);
        if (!path[0]) { Sleep(50); m_source = SRC_LOOPBACK; return; }

        const UINT32 RATE = 48000, CH = 2;
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = (WORD)CH; wf.nSamplesPerSec = RATE; wf.wBitsPerSample = 32;
        wf.nBlockAlign = (WORD)(CH * 4); wf.nAvgBytesPerSec = RATE * CH * 4;

        // Media Foundation source reader -> float PCM at our format.
        IMFSourceReader* reader = nullptr;
        if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader)) || !reader) {
            m_source = SRC_LOOPBACK; return;
        }
        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        IMFMediaType* outType = nullptr; MFCreateMediaType(&outType);
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
        outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
        outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        outType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, CH * 4);
        outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RATE * CH * 4);
        HRESULT hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outType);
        outType->Release();
        if (FAILED(hr)) { reader->Release(); m_source = SRC_LOOPBACK; return; }

        // WASAPI render on the default output; the engine converts our format.
        IMMDeviceEnumerator* devEnum = nullptr; IMMDevice* device = nullptr;
        IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr;
        UINT32 bufFrames = 0;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&devEnum);
        if (SUCCEEDED(hr)) hr = devEnum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            1000000, 0, &wf, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufFrames);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
        if (SUCCEEDED(hr)) hr = client->Start();

        if (SUCCEEDED(hr)) {
            m_sampleRate = RATE;
            std::vector<float> pending; size_t pos = 0; // interleaved stereo
            while (!m_quit && m_source.load() == SRC_FILE) {
                if ((pending.size() - pos) < (size_t)bufFrames * 2 * 4) {
                    DWORD flags = 0; IMFSample* sample = nullptr;
                    if (SUCCEEDED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                                     nullptr, &flags, nullptr, &sample))) {
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                            PROPVARIANT var; PropVariantInit(&var);
                            var.vt = VT_I8; var.hVal.QuadPart = 0;
                            reader->SetCurrentPosition(GUID_NULL, var);
                            PropVariantClear(&var);
                        } else if (sample) {
                            IMFMediaBuffer* buf = nullptr;
                            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf) {
                                BYTE* data = nullptr; DWORD len = 0;
                                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                                    const float* fp = (const float*)data;
                                    pending.insert(pending.end(), fp, fp + len / 4);
                                    buf->Unlock();
                                }
                                buf->Release();
                            }
                        }
                        if (sample) sample->Release();
                    }
                }
                UINT32 pad = 0;
                if (SUCCEEDED(client->GetCurrentPadding(&pad))) {
                    UINT32 avail = bufFrames - pad;
                    size_t haveFrames = (pending.size() - pos) / 2;
                    UINT32 toWrite = (UINT32)(haveFrames < avail ? haveFrames : avail);
                    if (toWrite > 0) {
                        BYTE* out = nullptr;
                        if (SUCCEEDED(render->GetBuffer(toWrite, &out))) {
                            memcpy(out, pending.data() + pos, (size_t)toWrite * 2 * sizeof(float));
                            AcquireSRWLockExclusive(&m_lock);
                            for (UINT32 i = 0; i < toWrite; i++)
                                m_ring[(m_writeIdx++) % RING] =
                                    (pending[pos + i * 2] + pending[pos + i * 2 + 1]) * 0.5f;
                            ReleaseSRWLockExclusive(&m_lock);
                            render->ReleaseBuffer(toWrite, 0);
                            pos += (size_t)toWrite * 2;
                        }
                    }
                }
                if (pos > (1 << 18)) { pending.erase(pending.begin(), pending.begin() + pos); pos = 0; }
                Sleep(3);
            }
            client->Stop();
        }
        SafeRelease(&render);
        SafeRelease(&client);
        SafeRelease(&device);
        SafeRelease(&devEnum);
        reader->Release();
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
    std::atomic<int> m_source{ SRC_LOOPBACK };
    SRWLOCK m_lock = SRWLOCK_INIT;
    float m_ring[RING] = {};
    uint64_t m_writeIdx = 0;
    UINT32 m_sampleRate = 48000;
    float m_smooth[1024] = {};
    float m_level = 0, m_bassAvg = 0, m_beat = 0, m_lastT = 0;
    wchar_t m_filePath[MAX_PATH] = {};
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
#include "ui_paint.inl"
#include "projectm_host.inl"
#include "shader_host.inl"
#include "spotify.inl"

// --------------------------------------------------------------------- app

// Shuffle timer choices, in seconds; 0 = off.
static const int SHUFFLE_SECS[] = { 0, 15, 30, 60, 300 };
static const int SHUFFLE_COUNT = (int)(sizeof(SHUFFLE_SECS) / sizeof(SHUFFLE_SECS[0]));
static const wchar_t* SHUF_LABELS[] = { L"OFF", L"15s", L"30s", L"1m", L"5m" };

// Render-resolution targets (backing-store height); 0 = auto (window size).
struct ResOpt { const wchar_t* name; int height; };
static const ResOpt RES_OPTS[] = {
    { L"AUTO",  0 }, { L"1080", 1080 }, { L"1440", 1440 }, { L"4K", 2160 }, { L"8K", 4320 },
};
static const int RES_COUNT = (int)(sizeof(RES_OPTS) / sizeof(RES_OPTS[0]));

// A user-created palette: 1-3 RGB stops (0..1), persisted to the config file.
struct CustomPalette { std::wstring name; int n = 1; float stops[3][3] = {}; };

static int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Appends each display's bounding rect (defined after App, used by DisplayRects).
static BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp);

struct App {
    HWND hwnd = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* hudFont = nullptr;
    IDWriteTextFormat* deckFont = nullptr;   // centered, for deck buttons
    IDWriteTextFormat* tinyFont = nullptr;   // small, right-aligned group descriptors
    AudioTap tap;
    HINSTANCE inst = nullptr;
    ProjectMHost pmHost;
    bool pmActive = false;
    ShaderHost shaderHost;
    bool shaderActive = false;
    int shaderIdx = 0;       // index into the GPU shader modes
    int deckMenu = -1;       // DK_ index whose dropdown is open, -1 = none
    bool milkMode = false;   // projectM engine: false=PROJM (bundled), true=MILK (library)
    std::wstring libraryFolder;  // MILK's .milk library folder (defaults to bundled)
    int displayIdx = 0;      // monitor the window currently targets
    ID2D1BitmapRenderTarget* offscreen = nullptr;
    ID2D1SolidColorBrush* offBrush = nullptr;
    UINT offW = 0, offH = 0;
    // UI overlay rendered with D2D into a WIC bitmap, then composited as a GL
    // texture over the projectM/shader engines so the UI is identical (crisp
    // DirectWrite) in every engine, not an aliased GL bitmap font.
    IWICImagingFactory* wic = nullptr;
    IWICBitmap* olBmp = nullptr;
    ID2D1RenderTarget* olRT = nullptr;
    ID2D1SolidColorBrush* olBrush = nullptr;
    UINT olW = 0, olH = 0;
    std::vector<BYTE> olPixels;   // last rendered BGRA, uploaded on change
    std::wstring olSig;           // signature of what's drawn (dirty check)
    std::vector<CustomPalette> customPalettes;
    // Palettes + shuffle panel (the web "PALETTES + SHUFFLE" window).
    bool palPanel = false;             // panel open
    bool shufflePalettes = false;      // shuffle tick also re-rolls the palette
    std::vector<char> palInclude;      // per-palette shuffle inclusion (1=in)
    float newStops[3][3] = { { 0.20f, 0.25f, 0.79f },   // new-palette editor:
                             { 0.30f, 0.80f, 0.55f },   // three color stops,
                             { 0.95f, 0.96f, 0.94f } };  // dark -> bright
    std::wstring newName;              // new-palette name field
    bool nameFocused = false;          // name field has keyboard focus
    int modeIdx = 0;
    int paletteIdx = 0;
    int resIdx = 0;          // index into RES_OPTS
    bool calm = false;
    int shuffleIdx = 0;      // index into SHUFFLE_SECS
    float lastShuffle = 0;
    bool hud = true;         // master enable (H); when on, auto-fades on idle
    float hudActivity = 0;   // last mouse-move time; HUD fades out after it
    bool help = false;       // F1 / ? full keymap overlay
    bool deckVisible = false;// auto-hiding bottom control bar
    int deckHover = -1;      // button under the cursor, or -1
    float deckActivity = 0;  // last time the deck was touched (for auto-hide)
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
        dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 11.0f, L"en-us", &deckFont);
        if (deckFont) {
            deckFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            deckFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            deckFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);  // clip, never wrap
        }
        // Tiny dim group descriptors (SRC / MODE / TRACE / SHUFFLE / RES),
        // right-aligned so they tuck against the group they label.
        dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 9.0f, L"en-us", &tinyFont);
        if (tinyFont) {
            tinyFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            tinyFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tinyFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(&wic));
        return CreateTarget();
    }

    void ReleaseOverlay() {
        SafeRelease(&olBrush);
        SafeRelease(&olRT);
        SafeRelease(&olBmp);
        olW = olH = 0;
        olSig.clear();
    }

    // (Re)create the WIC bitmap + D2D render target the UI overlay is drawn
    // into, at the native window size so overlay pixels map 1:1 to window
    // pixels (exact hit-testing). It only re-renders/re-uploads when the UI
    // changes (dirty-checked), so full-res is cheap.
    bool EnsureOverlayRT(int clientW, int clientH) {
        if (!wic) return false;
        UINT w = (UINT)(clientW > 0 ? clientW : 1);
        UINT h = (UINT)(clientH > 0 ? clientH : 1);
        if (olRT && olW == w && olH == h) return true;
        ReleaseOverlay();
        if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &olBmp))) return false;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateWicBitmapRenderTarget(olBmp, props, &olRT))) { ReleaseOverlay(); return false; }
        olRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &olBrush);
        olW = w; olH = h;
        return true;
    }

    // Render the UI overlay with D2D when it changed; copy the pixels out for
    // the GL side. Returns true if the texture must be re-uploaded.
    bool BuildOverlay(int clientW, int clientH, const wchar_t* l1, const wchar_t* l2) {
        if (!EnsureOverlayRT(clientW, clientH)) return false;
        std::wstring sig = std::to_wstring(olW) + L"|" + std::to_wstring(olH) + L"|" +
            std::to_wstring(deckVisible) + L"|" + std::to_wstring(deckHover) + L"|" +
            std::to_wstring(help) + L"|" + std::to_wstring(deckMenu) + L"|" +
            std::to_wstring((int)(HudFade() * 12)) + L"|" +   // fade steps → re-render as HUD fades
            std::to_wstring(DeckActive(DK_SYS)) + std::to_wstring(DeckActive(DK_MIC)) +
            std::to_wstring(DeckActive(DK_DEMO)) + std::to_wstring(DeckActive(DK_FILE)) +
            L"|" + (l1 ? l1 : L"") + L"|" + (l2 ? l2 : L"");
        if (palPanel) {   // capture everything the panel draws so it stays live
            sig += L"|P" + std::to_wstring(shufflePalettes) + std::to_wstring(nameFocused) +
                   std::to_wstring(paletteIdx) + L"|" + newName + L"|";
            for (char v : palInclude) sig += (v ? L'1' : L'0');
            for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++)
                sig += L"." + std::to_wstring((int)(newStops[k][c] * 255));
        }
        if (olSig == sig) return false; // unchanged — reuse the cached texture
        olSig = sig;

        olRT->BeginDraw();
        olRT->Clear(D2D1::ColorF(0, 0.0f));   // transparent
        D2DPainter p(olRT, olBrush, hudFont, deckFont, tinyFont);
        DrawUI(p, l1, l2);
        olRT->EndDraw();

        WICRect rc = { 0, 0, (INT)olW, (INT)olH };
        IWICBitmapLock* lock = nullptr;
        if (SUCCEEDED(olBmp->Lock(&rc, WICBitmapLockRead, &lock))) {
            UINT cb = 0; BYTE* data = nullptr;
            lock->GetDataPointer(&cb, &data);
            olPixels.assign(data, data + cb);
            lock->Release();
        }
        return true;
    }

    void ReleaseOffscreen() {
        SafeRelease(&offBrush);
        SafeRelease(&offscreen);
        offW = offH = 0;
    }

    // The offscreen target the modes draw into, sized to the resolution
    // setting; it holds the phosphor-trail content between frames.
    bool EnsureOffscreen(int clientW, int clientH) {
        int th = RES_OPTS[resIdx].height;
        UINT w, h;
        if (th <= 0) { w = (UINT)(clientW > 0 ? clientW : 1); h = (UINT)(clientH > 0 ? clientH : 1); }
        else {
            h = (UINT)th;
            w = (UINT)((float)th * (float)clientW / (float)(clientH > 0 ? clientH : 1) + 0.5f);
            if (w > 7680) { h = (UINT)((float)h * 7680.0f / (float)w); w = 7680; }
            if (w < 1) w = 1; if (h < 1) h = 1;
        }
        if (offscreen && offW == w && offH == h) return true;
        ReleaseOffscreen();
        D2D1_SIZE_U px = D2D1::SizeU(w, h);
        if (FAILED(rt->CreateCompatibleRenderTarget(nullptr, &px, nullptr,
                D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &offscreen)))
            return false;
        offscreen->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &offBrush);
        offW = w; offH = h;
        return true;
    }

    bool CreateTarget() {
        SafeRelease(&brush);
        SafeRelease(&rt);
        ReleaseOffscreen(); // tied to the old rt via CreateCompatibleRenderTarget
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        HRESULT hr = d2d->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &rt);
        if (FAILED(hr)) return false;
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        return true;
    }

    void Render() {
        if (!rt) return;
        D2D1_SIZE_F size = rt->GetSize();
        float now = Now();
        if (hudActivity == 0) hudActivity = now;   // show the HUD at launch, then fade

        // Shuffle timer: hop to a random other visual (or preset) on interval.
        if (SHUFFLE_SECS[shuffleIdx] > 0 && now - lastShuffle >= (float)SHUFFLE_SECS[shuffleIdx]) {
            lastShuffle = now;
            if (pmActive) pmHost.NextPreset(1);
            else if (shaderActive) shaderIdx = (shaderIdx + 1) % shaderHost.ShaderCount();
            else modeIdx = PickRandomMode();
            if (shufflePalettes) paletteIdx = PickRandomIncludedPalette();
        }

        AudioFrame frame;
        tap.Analyze(frame, now);
        if (calm) frame.beat *= 0.2f; // photosensitivity: tame the pulse

        // Auto-hide the deck once idle (shared by every engine); the open
        // shader picker keeps it alive.
        if (deckVisible && deckMenu < 0 && now - deckActivity > 2.5f) { deckVisible = false; deckHover = -1; }
        RECT crc; GetClientRect(hwnd, &crc);

        // projectM engine: render OpenGL on the child window, then composite
        // the D2D-rendered UI overlay (crisp, identical to the built-in view)
        // as a texture on top.
        if (pmActive && pmHost.ready) {
            wchar_t l1[300];
            swprintf_s(l1, L"SRC %s\n%s %s\nRES %s\nFPS %d",
                       tap.SourceName(), milkMode ? L"MILK" : L"PROJM", pmHost.PresetName(),
                       RES_OPTS[resIdx].name, fps);
            bool dirty = BuildOverlay(crc.right, crc.bottom, l1, nullptr);
            pmHost.RenderEngine(frame);
            pmHost.BeginOverlay();
            pmHost.DrawOverlayTex(olPixels.empty() ? nullptr : olPixels.data(), (int)olW, (int)olH, dirty);
            pmHost.Present();
            fpsFrames++;
            if (now - fpsWindowStart >= 1.0f) { fps = fpsFrames; fpsFrames = 0; fpsWindowStart = now; }
            return;
        }

        // GPU shader engine: a fullscreen fragment shader, same UI over it.
        if (shaderActive && shaderHost.ready) {
            wchar_t l1[300];
            swprintf_s(l1, L"SRC %s\nGPU %s\nRES %s\nFPS %d",
                       tap.SourceName(), shaderHost.ShaderName(shaderIdx), RES_OPTS[resIdx].name, fps);
            bool dirty = BuildOverlay(crc.right, crc.bottom, l1, nullptr);
            shaderHost.RenderEngine(shaderIdx, frame);
            shaderHost.BeginOverlay();
            shaderHost.DrawOverlayTex(olPixels.empty() ? nullptr : olPixels.data(), (int)olW, (int)olH, dirty);
            shaderHost.Present();
            fpsFrames++;
            if (now - fpsWindowStart >= 1.0f) { fps = fpsFrames; fpsFrames = 0; fpsWindowStart = now; }
            return;
        }

        const Mode& mode = MODES[modeIdx];

        RECT rc; GetClientRect(hwnd, &rc);
        EnsureOffscreen(rc.right, rc.bottom);

        // Draw the visualizer into the offscreen target at the chosen
        // resolution. It retains its content between frames, so the fade wash
        // produces phosphor trails.
        offscreen->BeginDraw();
        offBrush->SetColor(D2D1::ColorF(0.043f, 0.051f, 0.047f, mode.fade));
        offscreen->FillRectangle(D2D1::RectF(0, 0, (float)offW, (float)offH), offBrush);
        DrawCtx g{ offscreen, d2d, offBrush, PaletteFn(), (float)offW, (float)offH, &frame };
        mode.draw(g);
        offscreen->EndDraw();

        // Present: blit the offscreen scaled to the window, then the HUD.
        rt->BeginDraw();
        ID2D1Bitmap* bmp = nullptr;
        offscreen->GetBitmap(&bmp);
        if (bmp) {
            rt->DrawBitmap(bmp, D2D1::RectF(0, 0, size.width, size.height), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            bmp->Release();
        }

        wchar_t l1[300];
        swprintf_s(l1, L"SRC %s\nMODE %s  (%02d/%d)\nRES %s\nFPS %d%s",
                   tap.SourceName(), mode.name, modeIdx + 1, MODE_COUNT,
                   RES_OPTS[resIdx].name, fps, calm ? L"   CALM" : L"");
        D2DPainter dp(rt, brush, hudFont, deckFont, tinyFont);
        DrawUI(dp, l1, nullptr);

        HRESULT hr = rt->EndDraw();
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET) CreateTarget();

        fpsFrames++;
        if (now - fpsWindowStart >= 1.0f) {
            fps = fpsFrames;
            fpsFrames = 0;
            fpsWindowStart = now;
        }
    }

    // ----- auto-hiding control deck (bottom bar) -----
    // Clicking a button runs the same action as its keyboard shortcut. Drawn
    // via the shared UIPainter, so it appears identically over every engine
    // (D2D built-in modes and the GL projectM / shader engines alike).
    // Laid out left-to-right in the same logical order as the web control bar:
    // SRC | MODE-picker | ENGINE | selectors | actions | HUD.
    enum { DK_SYS, DK_MIC, DK_FILE, DK_DEMO, DK_SPOT,   // SRC
           DK_PREV, DK_PICK, DK_NEXT,                    // adaptive mode/preset picker
           DK_WAVE, DK_GPU, DK_PROJM, DK_MILK,           // engines
           DK_TRACE, DK_SHUF, DK_RES,                    // selectors
           DK_CALM, DK_PALS, DK_DISP, DK_FULL, DK_HELP,  // actions (right cluster)
           DK_N };
    // The HUD has no button (web parity): H toggles it and it auto-fades when the
    // mouse is idle. The ? overlay documents the H shortcut.

    // Each logical group ends here — used for row-break points AND (when the
    // next group carries one) the tiny descriptor label before it.
    static bool DeckGroupEnd(int i) {
        return i == DK_SPOT || i == DK_NEXT || i == DK_MILK ||
               i == DK_TRACE || i == DK_SHUF || i == DK_RES || i == DK_HELP;
    }
    // Tiny dim descriptor drawn before a group's first button (web-style).
    // Empty groups (engines, actions, HUD) carry no label.
    static const wchar_t* DeckDescriptor(int firstBtn) {
        switch (firstBtn) {
        case DK_SYS:   return L"SRC";
        case DK_PREV:  return L"MODE";
        case DK_TRACE: return L"TRACE";
        case DK_SHUF:  return L"SHUFFLE";
        case DK_RES:   return L"RES";
        default:       return L"";
        }
    }
    // Width reserved for a descriptor slot (0 when the group has none).
    static float DescWidth(int firstBtn) {
        const wchar_t* d = DeckDescriptor(firstBtn);
        return *d ? (float)wcslen(d) * 6.0f + 12.0f : 0.0f;
    }

    // The center picker mirrors whichever engine is active; the value buttons
    // show their current selection inline with a ▾, like the web dropdowns.
    std::wstring DeckLabel(int i) {
        const wchar_t* CH = L" \x25BE";   // ▾ dropdown chevron
        switch (i) {
        case DK_SYS:  return L"system"; case DK_MIC:  return L"mic";
        case DK_FILE: return L"file";   case DK_DEMO: return L"demo"; case DK_SPOT: return L"spotify";
        case DK_PREV: return L"\x2039"; case DK_NEXT: return L"\x203A";
        case DK_PICK: return (pmActive   ? std::wstring(pmHost.PresetName())
                            : shaderActive ? std::wstring(shaderHost.ShaderName(shaderIdx))
                                           : std::wstring(MODES[modeIdx].name)) + CH;
        case DK_WAVE:  return L"WaveScope"; case DK_GPU:   return L"GPU";
        case DK_PROJM: return L"projectM";  case DK_MILK:  return L"milkdrop";
        case DK_TRACE: return std::wstring(PaletteName()) + CH;
        case DK_SHUF:  return std::wstring(SHUF_LABELS[shuffleIdx]) + CH;
        case DK_RES:   return std::wstring(RES_OPTS[resIdx].name) + CH;
        case DK_CALM:  return L"calm"; case DK_PALS: return L"palettes + shuffle";
        case DK_DISP:  return std::wstring(L"displays") + CH; case DK_FULL: return L"fullscreen";
        case DK_HELP:  return L"?";
        default: return L"";
        }
    }

    // Fill `out` with each button's rect and return the bar bounding rect.
    // Two clusters, web-style: the LEFT cluster (SRC…RES) flows left-justified
    // and wraps across as many rows as needed; the RIGHT cluster (actions)
    // is pinned to the right edge — riding the last left row if it fits there,
    // else dropping to its own right-aligned row. Buttons take their natural
    // (monospace content) width; gaps are small and uniform — the tiny group
    // descriptors do the visual grouping, not fat gaps.
    D2D1_RECT_F DeckLayout(float W, float H, D2D1_RECT_F out[DK_N]) {
        const float margin = 22, rowH = 28, gap = 7, groupGap = 12, rowGap = 8, bottom = 16;
        const float charW = 6.1f, padX = 12, minBtn = 20;
        float x0 = margin, x1 = W - margin, avail = x1 - x0;

        float nat[DK_N];
        for (int i = 0; i < DK_N; i++) {
            float w = (float)DeckLabel(i).length() * charW + padX;
            nat[i] = w < minBtn ? minBtn : w;
        }

        // Left-cluster groups (SRC…RES); the right cluster is DK_CALM..DK_HELP.
        int gStart[8], gEnd[8], gCount = 0; float gNat[8];
        for (int i = 0, s = 0; i <= DK_RES; i++) {
            if (DeckGroupEnd(i)) {
                gStart[gCount] = s; gEnd[gCount] = i;
                float sum = DescWidth(s) + gap * (i - s);
                for (int k = s; k <= i; k++) sum += nat[k];
                gNat[gCount++] = sum; s = i + 1;
            }
        }
        float rNat = gap * (DK_HELP - DK_CALM);
        for (int i = DK_CALM; i <= DK_HELP; i++) rNat += nat[i];

        // Wrap the left cluster into rows, left-justified.
        int rowOfG[8]; float rowEnd[8]; for (float& e : rowEnd) e = x0;
        int r = 0; float used = 0;
        for (int g = 0; g < gCount; g++) {
            float add = (used > 0 ? groupGap : 0) + gNat[g];
            if (used > 0 && used + add > avail) { r++; used = 0; add = gNat[g]; }
            rowOfG[g] = r; used += add; rowEnd[r] = x0 + used;
        }
        int leftRows = r + 1;

        // Right cluster: same row as the last left row if it fits, else a new row.
        int lastRow = leftRows - 1;
        bool rightNewRow = (rowEnd[lastRow] + groupGap + rNat > x1);
        int totalRows = leftRows + (rightNewRow ? 1 : 0);
        float totalH = totalRows * rowH + (totalRows - 1) * rowGap;
        float yTop = H - bottom - totalH;

        // Place the left cluster.
        float bxRow[8]; bool firstInRow[8];
        for (int i = 0; i < 8; i++) { bxRow[i] = x0; firstInRow[i] = true; }
        for (int g = 0; g < gCount; g++) {
            int rr = rowOfG[g]; float y = yTop + rr * (rowH + rowGap);
            float bx = bxRow[rr];
            if (!firstInRow[rr]) bx += groupGap;
            firstInRow[rr] = false;
            bx += DescWidth(gStart[g]);   // reserve the descriptor slot
            for (int i = gStart[g]; i <= gEnd[g]; i++) {
                out[i] = D2D1::RectF(bx, y, bx + nat[i], y + rowH);
                bx += nat[i] + (i < gEnd[g] ? gap : 0);
            }
            bxRow[rr] = bx;
        }
        // Place the right cluster, right-aligned.
        int rr = rightNewRow ? leftRows : lastRow;
        float y = yTop + rr * (rowH + rowGap);
        float bx = x1 - rNat;
        for (int i = DK_CALM; i <= DK_HELP; i++) {
            out[i] = D2D1::RectF(bx, y, bx + nat[i], y + rowH);
            bx += nat[i] + (i < DK_HELP ? gap : 0);
        }
        return D2D1::RectF(x0 - 8, yTop - 8, x1 + 8, H - bottom + 8);
    }

    int DeckHitTest(int mx, int my) {
        if (!rt) return -1;
        D2D1_SIZE_F size = rt->GetSize();
        D2D1_RECT_F b[DK_N];
        DeckLayout(size.width, size.height, b);
        for (int i = 0; i < DK_N; i++)
            if (mx >= b[i].left && mx <= b[i].right && my >= b[i].top && my <= b[i].bottom)
                return i;
        return -1;
    }

    // Is a button currently the active selection (for highlight)?
    bool DeckActive(int i) {
        switch (i) {
        case DK_SYS:   return tap.Source() == AudioTap::SRC_LOOPBACK;
        case DK_MIC:   return tap.Source() == AudioTap::SRC_MIC;
        case DK_DEMO:  return tap.Source() == AudioTap::SRC_DEMO;
        case DK_FILE:  return tap.Source() == AudioTap::SRC_FILE;
        case DK_SPOT:  return g_spotify.busy.load();
        case DK_WAVE:  return !pmActive && !shaderActive;
        case DK_GPU:   return shaderActive;
        case DK_PROJM: return pmActive && !milkMode;
        case DK_MILK:  return pmActive && milkMode;
        case DK_CALM:  return calm;
        case DK_PALS:  return palPanel;
        case DK_FULL:  return fullscreen;
        case DK_HELP:  return help;
        case DK_PICK: case DK_TRACE: case DK_SHUF: case DK_RES: case DK_DISP:
            return deckMenu == i;   // highlight whichever dropdown is open
        default:       return false;
        }
    }

    void DeckDispatch(int i) {
        int prev = deckMenu; deckMenu = -1;   // any click closes an open dropdown by default
        switch (i) {
        case DK_SYS:   tap.SetSource(AudioTap::SRC_LOOPBACK); break;
        case DK_MIC:   tap.SetSource(AudioTap::SRC_MIC); break;
        case DK_DEMO:  tap.SetSource(AudioTap::SRC_DEMO); break;
        case DK_FILE:  OpenFile(); break;
        case DK_SPOT:  g_spotify.OneButton(); break;
        case DK_PREV:  CyclePick(-1); break;
        case DK_NEXT:  CyclePick(1); break;
        case DK_WAVE:  if (pmActive) ToggleProjectM(); if (shaderActive) ToggleShader(); deckMenu = DK_PICK; break;
        case DK_GPU:   if (pmActive) ToggleProjectM(); if (!shaderActive) ToggleShader(); deckMenu = DK_PICK; break;
        case DK_PROJM: EnterProjectM(false); deckMenu = DK_PICK; break;
        case DK_MILK:  EnterProjectM(true);  deckMenu = DK_PICK; break;
        case DK_PICK: case DK_TRACE: case DK_SHUF: case DK_RES: case DK_DISP:
                       deckMenu = (prev == i ? -1 : i); break;   // toggle this dropdown
        case DK_CALM:  SetCalm(!calm); break;
        case DK_PALS:  OpenPalettePanel(); break;
        case DK_FULL:  ToggleFullscreen(); break;
        case DK_HELP:  help = !help; break;
        }
    }

    // Cycle the active engine's selection (mode / shader / preset) with ‹ ›.
    void CyclePick(int dir) {
        if (pmActive) pmHost.NextPreset(dir);
        else if (shaderActive) { int n = shaderHost.ShaderCount(); if (n) shaderIdx = (shaderIdx + dir + n) % n; }
        else CycleMode(dir);
    }

    // Enter the projectM engine as PROJM (bundled presets) or MILK (the user's
    // classic .milk library, if they've pointed one via the folder picker).
    void EnterProjectM(bool library) {
        if (shaderActive) ToggleShader();
        if (!pmActive) ToggleProjectM();
        milkMode = library;
        if (pmActive && pmHost.ready && library && !libraryFolder.empty())
            pmHost.SetPresetFolder(libraryFolder.c_str());
    }

    // ---- multi-monitor: the DISPLAYS dropdown moves the window per screen ----
    std::vector<RECT> DisplayRects() {
        std::vector<RECT> v;
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, (LPARAM)&v);
        return v;
    }
    int DisplayCount() { return (int)DisplayRects().size(); }
    void MoveToMonitor(int idx) {
        auto v = DisplayRects();
        if (idx < 0 || idx >= (int)v.size()) return;
        displayIdx = idx;
        fullscreen = true;
        DWORD style = (DWORD)GetWindowLongPtr(hwnd, GWL_STYLE);
        GetWindowPlacement(hwnd, &savedPlacement);
        SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        RECT r = v[idx];
        SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }

    // ---- generic dropdown, shared by every picker/selector button ----
    // deckMenu holds the DK_ index of the open dropdown (-1 = none). Each
    // anchor answers count/item/current/disabled/select for its own data.
    int MenuCount(int a) {
        switch (a) {
        case DK_PICK:  return pmActive ? pmHost.PresetCount() : shaderActive ? shaderHost.ShaderCount() : MODE_COUNT;
        case DK_TRACE: return PaletteCount();
        case DK_SHUF:  return SHUFFLE_COUNT;
        case DK_RES:   return RES_COUNT;
        case DK_DISP:  return DisplayCount();
        }
        return 0;
    }
    const wchar_t* MenuItem(int a, int i) {
        static wchar_t buf[64];
        switch (a) {
        case DK_PICK:  return pmActive ? pmHost.PresetNameAt(i) : shaderActive ? shaderHost.ShaderName(i) : MODES[i].name;
        case DK_TRACE: return PalNameFor(i);
        case DK_SHUF:  return SHUF_LABELS[i];
        case DK_RES:   return RES_OPTS[i].name;
        case DK_DISP:  swprintf_s(buf, L"DISPLAY %d", i + 1); return buf;
        }
        return L"";
    }
    int MenuCurrent(int a) {
        switch (a) {
        case DK_PICK:  return pmActive ? pmHost.PresetIndex() : shaderActive ? shaderIdx : modeIdx;
        case DK_TRACE: return paletteIdx;
        case DK_SHUF:  return shuffleIdx;
        case DK_RES:   return resIdx;
        case DK_DISP:  return displayIdx;
        }
        return -1;
    }
    bool MenuDisabled(int a, int i) {
        return a == DK_PICK && !pmActive && !shaderActive && calm && MODES[i].flash;
    }
    void MenuSelect(int a, int i) {
        switch (a) {
        case DK_PICK:
            if (pmActive) pmHost.LoadPreset(i, true);
            else if (shaderActive) shaderIdx = i;
            else modeIdx = i;
            break;
        case DK_TRACE: paletteIdx = i; break;
        case DK_SHUF:  SetShuffle(i); break;
        case DK_RES:   resIdx = i; break;
        case DK_DISP:  MoveToMonitor(i); break;
        }
    }
    // Lay the open dropdown out above its anchor button, wrapping into columns
    // so long lists (34 modes, big preset libraries) fit above the deck.
    void MenuGeom(int a, float W, float H, D2D1_RECT_F& bg, std::vector<D2D1_RECT_F>& rows) {
        rows.clear();
        int n = MenuCount(a);
        if (n <= 0) { bg = D2D1::RectF(0, 0, 0, 0); return; }
        D2D1_RECT_F b[DK_N]; DeckLayout(W, H, b);
        const float rw = 168, rh = 24, pad = 5, colGap = 4, topLimit = 8;
        float bottom = b[a].top - 8;
        int perCol = (int)((bottom - topLimit - 2 * pad) / rh);
        if (perCol < 1) perCol = 1;
        if (perCol > n) perCol = n;
        int cols = (n + perCol - 1) / perCol;
        float menuW = cols * rw + (cols - 1) * colGap + 2 * pad;
        float menuH = perCol * rh + 2 * pad;
        float x = b[a].left;
        if (x + menuW > W - 8) x = W - 8 - menuW;
        if (x < 8) x = 8;
        float top = bottom - menuH;
        if (top < topLimit) top = topLimit;
        bg = D2D1::RectF(x, top, x + menuW, top + menuH);
        rows.resize(n);
        for (int i = 0; i < n; i++) {
            int c = i / perCol, r = i % perCol;
            float rx = x + pad + c * (rw + colGap);
            float ry = top + pad + r * rh;
            rows[i] = D2D1::RectF(rx, ry, rx + rw, ry + rh - 2);
        }
    }
    void DrawMenu(UIPainter& p) {
        if (deckMenu < 0) return;
        D2D1_RECT_F bg; std::vector<D2D1_RECT_F> rows;
        MenuGeom(deckMenu, p.W(), p.H(), bg, rows);
        if (rows.empty()) return;
        p.Rect(bg.left, bg.top, bg.right - bg.left, bg.bottom - bg.top, 0.04f, 0.05f, 0.06f, 0.98f, true);
        int cur = MenuCurrent(deckMenu);
        for (int i = 0; i < (int)rows.size(); i++) {
            bool active = (i == cur), off = MenuDisabled(deckMenu, i);
            p.Rect(rows[i].left, rows[i].top, rows[i].right - rows[i].left, rows[i].bottom - rows[i].top,
                   active ? 0.20f : 0.10f, active ? 0.42f : 0.11f, active ? 0.72f : 0.12f, 0.95f, true);
            float t = off ? 0.34f : (active ? 1.0f : 0.86f);
            p.Text(rows[i].left + 10, rows[i].top, rows[i].right - rows[i].left - 14, rows[i].bottom - rows[i].top,
                   MenuItem(deckMenu, i), t, active ? 1.0f : (off ? 0.36f : 0.88f), active ? 1.0f : (off ? 0.38f : 0.90f), 0.96f);
        }
    }
    bool MenuClick(int mx, int my) {
        if (deckMenu < 0) return false;
        D2D1_SIZE_F sz = rt->GetSize();
        D2D1_RECT_F bg; std::vector<D2D1_RECT_F> rows;
        MenuGeom(deckMenu, sz.width, sz.height, bg, rows);
        for (int i = 0; i < (int)rows.size(); i++)
            if (In(rows[i], mx, my)) {
                if (MenuDisabled(deckMenu, i)) return true;   // disabled: swallow, don't select
                MenuSelect(deckMenu, i); deckMenu = -1; return true;
            }
        if (In(bg, mx, my)) return true;   // swallow clicks inside the menu chrome
        deckMenu = -1;                     // click elsewhere closes it
        return false;                      // ...and falls through to the deck
    }

    // Mouse near the bottom edge reveals the deck; hovering keeps it alive.
    void OnMouseMove(int mx, int my) {
        hudActivity = Now();   // any movement wakes the HUD (then it fades on idle)
        if (palPanel) return;  // panel is modal
        RECT rc; GetClientRect(hwnd, &rc);
        int hit = DeckHitTest(mx, my);
        if (my > rc.bottom - 175 || hit >= 0 || deckMenu >= 0) { deckVisible = true; deckActivity = Now(); }
        deckHover = deckVisible ? hit : -1;
    }

    void OnClick(int mx, int my) {
        if (PanelClick(mx, my)) return;
        if (MenuClick(mx, my)) { deckActivity = Now(); return; }
        if (!deckVisible) return;
        int hit = DeckHitTest(mx, my);
        if (hit >= 0) { DeckDispatch(hit); deckActivity = Now(); }
    }

    // HUD opacity: full while the mouse is active, fading to 0 after ~2s idle;
    // 0 when the user has toggled it off with H. No button — the ? overlay
    // documents the shortcut (web parity).
    float HudFade() {
        if (!hud) return 0.0f;
        const float HOLD = 2.0f, FADE = 0.9f;
        float f = 1.0f - (Now() - hudActivity - HOLD) / FADE;
        return f < 0 ? 0 : (f > 1 ? 1 : f);
    }

    // The whole on-screen UI, drawn through the abstract painter so it renders
    // identically over every engine. `l1`/`l2` are the engine-specific HUD
    // status + hint lines built by Render.
    void DrawUI(UIPainter& p, const wchar_t* l1, const wchar_t* /*l2*/) {
        float hf = HudFade();
        if (l1 && hf > 0.01f) {
            // Compact top-left status stack (SRC / MODE / RES / FPS), like the web.
            p.Text(16, 10, 460, 108, l1, 0.90f, 0.93f, 0.92f, 0.92f * hf);
        }
        if (palPanel) { DrawPalettePanel(p); return; }  // modal: hides deck
        if (deckVisible) DrawDeckP(p);
        if (deckVisible) DrawMenu(p);         // open dropdown sits above its anchor
        if (help) DrawHelpP(p);
    }

    void DrawDeckP(UIPainter& p) {
        D2D1_RECT_F b[DK_N];
        DeckLayout(p.W(), p.H(), b);   // no full-width backdrop — let the bar breathe
        for (int i = 0; i < DK_N; i++) {
            bool active = DeckActive(i), hover = (i == deckHover);
            float w = b[i].right - b[i].left, h = b[i].bottom - b[i].top;
            // Each button carries only a faint dark fill for legibility + a thin
            // outline. Active = subtle blue; hover = a touch brighter.
            if (active) {
                p.Rect(b[i].left, b[i].top, w, h, 0.15f, 0.33f, 0.58f, 0.55f, true);
                p.Stroke(b[i].left, b[i].top, w, h, 0.36f, 0.62f, 0.95f, 0.95f, 1.2f, true);
            } else {
                p.Rect(b[i].left, b[i].top, w, h, 0.05f, 0.06f, 0.07f, hover ? 0.55f : 0.40f, true);
                p.Stroke(b[i].left, b[i].top, w, h, 0.36f, 0.42f, 0.49f, hover ? 0.80f : 0.45f, 1.0f, true);
            }
            float tr = active ? 1.0f : 0.78f, tg = active ? 1.0f : 0.83f, tb = active ? 1.0f : 0.85f;
            p.Text(b[i].left, b[i].top, w, h, DeckLabel(i).c_str(), tr, tg, tb, active ? 1.0f : 0.92f, true);
            // Tiny dim descriptor tucked into the reserved slot left of the group.
            const wchar_t* d = DeckDescriptor(i);
            if (*d) {
                float dw = DescWidth(i);
                p.Text(b[i].left - dw, b[i].top, dw - 5, h, d, 0.50f, 0.56f, 0.58f, 0.75f, true, true);
            }
        }
    }

    // Full keymap overlay (F1 / ?), a centered card over any engine.
    void DrawHelpP(UIPainter& p) {
        static const wchar_t* HELP =
            L"WAVESCOPE  \x2014  KEYS\n"
            L"\n"
            L"SPACE / \x2192      next mode (or preset / shader)\n"
            L"\x2190              previous\n"
            L"P              cycle trace palette\n"
            L"N              palettes + shuffle panel (edit / include)\n"
            L"1 / 2 / 3      source: system / mic / demo\n"
            L"4              open an audio file\n"
            L"M              MilkDrop engine (projectM)\n"
            L"G              GPU shader engine\n"
            L"B              Spotify: authorize + start playback (Premium)\n"
            L"S              shuffle timer (off / 15s / 30s / 1m / 5m)\n"
            L"C              calm mode (photosensitive-safe)\n"
            L"R              resolution (AUTO / 1080 / 1440 / 4K / 8K)\n"
            L"L              MilkDrop preset folder\n"
            L"F              fullscreen        H   HUD (auto-fades on idle)\n"
            L"F1 / ?         this help         ESC quit\n"
            L"\n"
            L"Launch: WaveScope.exe [demo | file=PATH | milkdrop | shader | wall | kiosk]\n"
            L"        [install-startup | remove-startup]   \x2014   tray icon: right-click";
        float w = 620, h = 448;
        float x = (p.W() - w) * 0.5f, y = (p.H() - h) * 0.5f;
        if (x < 0) x = 0; if (y < 0) y = 0;
        p.Rect(x, y, w, h, 0.03f, 0.04f, 0.04f, 0.92f, true);
        p.Text(x + 22, y + 18, w - 32, h - 24, HELP, 0.95f, 0.96f, 0.95f, 0.96f);
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

    // Pick an audio file to play + visualize.
    void OpenFile() {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter =
            L"Audio files\0*.mp3;*.wav;*.flac;*.m4a;*.aac;*.wma;*.ogg\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (GetOpenFileNameW(&ofn)) tap.SetFile(file);
    }

    // Point the projectM engine at a folder of .milk presets.
    void OpenPresetFolder() {
        if (!pmActive || !pmHost.ready) return;
        BROWSEINFOW bi = {};
        bi.hwndOwner = hwnd;
        bi.lpszTitle = L"Choose a folder of .milk presets";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, path)) pmHost.SetPresetFolder(path);
            CoTaskMemFree(pidl);
        }
    }

    // Switch between the built-in Direct2D modes and the projectM engine.
    void ToggleProjectM() {
        if (!pmActive) {
            if (shaderActive) { shaderActive = false; shaderHost.Show(false); }
            if (!pmHost.ready && !pmHost.Init(inst, hwnd)) {
                SetWindowTextW(hwnd, L"WaveScope  (projectM failed to start)");
                return;
            }
            RECT rc; GetClientRect(hwnd, &rc);
            pmHost.Resize(rc.right, rc.bottom);
            pmHost.Show(true);
            pmActive = true;
            SetWindowTextW(hwnd, L"WaveScope  projectM (M to exit, SPACE next preset)");
        } else {
            pmActive = false;
            pmHost.Show(false);
            SetWindowTextW(hwnd, L"WaveScope");
        }
    }

    // Switch between the built-in Direct2D modes and the GPU shader engine.
    void ToggleShader() {
        if (!shaderActive) {
            if (pmActive) { pmActive = false; pmHost.Show(false); }
            if (!shaderHost.ready && !shaderHost.Init(inst, hwnd)) {
                SetWindowTextW(hwnd, L"WaveScope  (GPU shaders failed to start)");
                return;
            }
            RECT rc; GetClientRect(hwnd, &rc);
            shaderHost.Resize(rc.right, rc.bottom);
            shaderHost.Show(true);
            shaderActive = true;
            SetWindowTextW(hwnd, L"WaveScope  GPU shader (G to exit, SPACE next shader)");
        } else {
            shaderActive = false;
            shaderHost.Show(false);
            SetWindowTextW(hwnd, L"WaveScope");
        }
    }

    // Advance the mode, skipping beat-burst modes while calm mode is on.
    void CycleMode(int dir) {
        for (int step = 0; step < MODE_COUNT; step++) {
            modeIdx = (modeIdx + dir + MODE_COUNT) % MODE_COUNT;
            if (!calm || !MODES[modeIdx].flash) return;
        }
    }

    // A random mode other than the current one (non-flash while calm).
    int PickRandomMode() {
        for (int tries = 0; tries < 64; tries++) {
            int i = rand() % MODE_COUNT;
            if (i != modeIdx && (!calm || !MODES[i].flash)) return i;
        }
        return modeIdx;
    }

    void SetCalm(bool c) {
        calm = c;
        if (calm && MODES[modeIdx].flash) CycleMode(1);
    }

    void CycleShuffle() { shuffleIdx = (shuffleIdx + 1) % SHUFFLE_COUNT; lastShuffle = Now(); }
    void SetShuffle(int i) { shuffleIdx = Clampi(i, 0, SHUFFLE_COUNT - 1); lastShuffle = Now(); }

    // ----- palettes (built-in + user-created) -----

    int PaletteCount() const { return PAL_COUNT + (int)customPalettes.size(); }
    const wchar_t* PaletteName() const {
        return paletteIdx < PAL_COUNT ? PALETTES[paletteIdx].name
                                      : customPalettes[paletteIdx - PAL_COUNT].name.c_str();
    }
    // Resolve the active palette's color fn, priming the custom-stop globals.
    D2D1_COLOR_F (*PaletteFn())(float, float) {
        if (paletteIdx < PAL_COUNT) return PALETTES[paletteIdx].color;
        const CustomPalette& cp = customPalettes[paletteIdx - PAL_COUNT];
        g_customStopCount = cp.n;
        for (int i = 0; i < cp.n; i++)
            for (int c = 0; c < 3; c++) g_customStops[i][c] = cp.stops[i][c];
        return PalCustom;
    }
    void CyclePalette() { paletteIdx = (paletteIdx + 1) % PaletteCount(); }

    // Create a palette from up to 3 colors via the standard picker (cancel to
    // stop early). A full stop editor arrives with the on-screen deck.
    void NewPalette() {
        static COLORREF customColors[16] = {};
        CustomPalette cp; cp.n = 0;
        COLORREF seed[3] = { RGB(51, 70, 201), RGB(122, 224, 195), RGB(242, 244, 241) };
        for (int s = 0; s < 3; s++) {
            CHOOSECOLORW cc = {};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hwnd;
            cc.lpCustColors = customColors;
            cc.rgbResult = seed[s];
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;
            if (!ChooseColorW(&cc)) break;
            cp.stops[cp.n][0] = GetRValue(cc.rgbResult) / 255.0f;
            cp.stops[cp.n][1] = GetGValue(cc.rgbResult) / 255.0f;
            cp.stops[cp.n][2] = GetBValue(cc.rgbResult) / 255.0f;
            cp.n++;
        }
        if (cp.n < 1) return;
        wchar_t nm[24]; swprintf_s(nm, L"CUSTOM %d", (int)customPalettes.size() + 1);
        cp.name = nm;
        customPalettes.push_back(cp);
        paletteIdx = PAL_COUNT + (int)customPalettes.size() - 1;
        EnsurePalInclude();
        SaveConfig();
    }

    // ----- palettes + shuffle panel (the web "PALETTES + SHUFFLE" window) -----

    void EnsurePalInclude() {
        int n = PaletteCount();
        while ((int)palInclude.size() < n) palInclude.push_back(1);
    }
    const wchar_t* PalNameFor(int i) const {
        return i < PAL_COUNT ? PALETTES[i].name : customPalettes[i - PAL_COUNT].name.c_str();
    }
    D2D1_COLOR_F (*PaletteFnFor(int i))(float, float) {
        if (i < PAL_COUNT) return PALETTES[i].color;
        const CustomPalette& cp = customPalettes[i - PAL_COUNT];
        g_customStopCount = cp.n;
        for (int a = 0; a < cp.n; a++) for (int c = 0; c < 3; c++) g_customStops[a][c] = cp.stops[a][c];
        return PalCustom;
    }
    int PickRandomIncludedPalette() {
        EnsurePalInclude();
        int n = PaletteCount(), pool[64], c = 0;
        for (int i = 0; i < n && c < 64; i++) if (palInclude[i]) pool[c++] = i;
        return c ? pool[rand() % c] : paletteIdx;
    }
    void OpenPalettePanel() { palPanel = !palPanel; nameFocused = false; EnsurePalInclude(); }

    // Open the standard picker for one of the new-palette color stops.
    void PickStop(int k) {
        static COLORREF custom[16] = {};
        CHOOSECOLORW cc = {};
        cc.lStructSize = sizeof(cc); cc.hwndOwner = hwnd; cc.lpCustColors = custom;
        cc.rgbResult = RGB((int)(newStops[k][0] * 255), (int)(newStops[k][1] * 255), (int)(newStops[k][2] * 255));
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColorW(&cc)) {
            newStops[k][0] = GetRValue(cc.rgbResult) / 255.0f;
            newStops[k][1] = GetGValue(cc.rgbResult) / 255.0f;
            newStops[k][2] = GetBValue(cc.rgbResult) / 255.0f;
        }
    }
    void AddNewPalette() {
        CustomPalette cp; cp.n = 3;
        for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) cp.stops[k][c] = newStops[k][c];
        if (!newName.empty()) cp.name = newName;
        else { wchar_t nm[24]; swprintf_s(nm, L"CUSTOM %d", (int)customPalettes.size() + 1); cp.name = nm; }
        customPalettes.push_back(cp);
        palInclude.push_back(1);
        paletteIdx = PAL_COUNT + (int)customPalettes.size() - 1;
        newName.clear(); nameFocused = false;
        SaveConfig();
    }
    // Keystrokes for the name field while the panel is open.
    void PanelChar(wchar_t c) {
        if (!palPanel || !nameFocused) return;
        if (c == 8) { if (!newName.empty()) newName.pop_back(); }
        else if (c == 13) AddNewPalette();
        else if (c >= 32 && newName.size() < 22) newName += c;
    }

    struct PalRects {
        D2D1_RECT_F card, close, master, swatch[3], preview, nameBox, addBtn;
        D2D1_RECT_F row[64];
        int rows = 0;
        float dividerY = 0, newHeadY = 0, newDescY = 0;
    };
    void PalLayout(float W, float H, PalRects& r) {
        EnsurePalInclude();
        int n = PaletteCount(); if (n > 64) n = 64;
        r.rows = n;
        const float pw = 462, pad = 28, rowH = 26;
        float ph = 344 + n * rowH;
        float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
        if (py < 12) py = 12;
        r.card = D2D1::RectF(px, py, px + pw, py + ph);
        float x = px + pad;
        r.close = D2D1::RectF(px + pw - pad - 44, py + 16, px + pw - pad, py + 38);
        r.master = D2D1::RectF(x, py + 64, x + 20, py + 84);
        float y = py + 132;
        for (int i = 0; i < n; i++) { r.row[i] = D2D1::RectF(x, y, px + pw - pad, y + rowH - 4); y += rowH; }
        r.dividerY = y + 12; y += 26;
        r.newHeadY = y; y += 30;
        r.newDescY = y; y += 42;
        for (int k = 0; k < 3; k++) { float sx = x + k * 60; r.swatch[k] = D2D1::RectF(sx, y, sx + 52, y + 40); }
        r.preview = D2D1::RectF(x + 3 * 60 + 12, y, px + pw - pad, y + 40);
        y += 52;
        r.nameBox = D2D1::RectF(x, y, x + 250, y + 34);
        r.addBtn = D2D1::RectF(px + pw - pad - 116, y, px + pw - pad, y + 34);
    }

    static bool In(const D2D1_RECT_F& b, int mx, int my) {
        return mx >= b.left && mx <= b.right && my >= b.top && my <= b.bottom;
    }
    void DrawCheck(UIPainter& p, const D2D1_RECT_F& b, bool on) {
        float w = b.right - b.left, h = b.bottom - b.top;
        p.Rect(b.left, b.top, w, h, 0.16f, 0.18f, 0.21f, 0.95f);
        if (on) p.Rect(b.left + 3, b.top + 3, w - 6, h - 6, 0.30f, 0.60f, 0.95f, 1.0f);
    }
    void DrawGrad(UIPainter& p, float x, float y, float w, float h, D2D1_COLOR_F (*fn)(float, float)) {
        const int N = 48; float sw = w / (float)N;
        for (int i = 0; i < N; i++) {
            float t = (i + 0.5f) / N;
            D2D1_COLOR_F c = fn ? fn(t, 1.0f) : D2D1::ColorF(0);
            p.Rect(x + i * sw, y, sw + 1.0f, h, c.r, c.g, c.b, 1.0f);
        }
    }
    void DrawPalettePanel(UIPainter& p) {
        PalRects r; PalLayout(p.W(), p.H(), r);
        p.Rect(0, 0, p.W(), p.H(), 0, 0, 0, 0.5f);                       // scrim
        p.Rect(r.card.left, r.card.top, r.card.right - r.card.left, r.card.bottom - r.card.top,
               0.04f, 0.05f, 0.06f, 0.98f, true);
        float cx = r.card.left + 28;
        p.Text(cx, r.card.top + 14, 340, 22, L"PALETTES + SHUFFLE", 0.35f, 0.62f, 0.95f, 1.0f);
        p.Text(r.close.left - 6, r.close.top, 60, 22, L"close", 0.72f, 0.74f, 0.76f, 0.9f);
        DrawCheck(p, r.master, shufflePalettes);
        p.Text(r.master.right + 12, r.master.top - 2, 340, 22, L"Shuffle palettes too", 0.92f, 0.93f, 0.94f, 0.96f);
        p.Text(cx, r.master.bottom + 4, 406, 40,
               L"Each shuffle tick also picks a random trace\npalette from the checked list below.",
               0.60f, 0.63f, 0.66f, 0.92f);
        for (int i = 0; i < r.rows; i++) {
            const D2D1_RECT_F& row = r.row[i];
            DrawCheck(p, D2D1::RectF(row.left, row.top + 1, row.left + 18, row.top + 19), palInclude[i] != 0);
            DrawGrad(p, row.left + 30, row.top + 2, 190, 17, PaletteFnFor(i));
            bool active = (i == paletteIdx);
            p.Text(row.left + 232, row.top - 1, 200, 22, PalNameFor(i),
                   active ? 0.45f : 0.86f, active ? 0.70f : 0.88f, active ? 1.0f : 0.90f, 0.95f);
        }
        p.Rect(cx, r.dividerY, r.card.right - 28 - cx, 1, 0.25f, 0.28f, 0.32f, 0.8f);
        p.Text(cx, r.newHeadY, 300, 24, L"New palette", 0.92f, 0.93f, 0.94f, 0.96f);
        p.Text(cx, r.newDescY, 406, 40,
               L"Three color stops, blended dark to bright across\neach visualizer.",
               0.60f, 0.63f, 0.66f, 0.92f);
        for (int k = 0; k < 3; k++) {
            const D2D1_RECT_F& s = r.swatch[k];
            p.Rect(s.left, s.top, s.right - s.left, s.bottom - s.top,
                   newStops[k][0], newStops[k][1], newStops[k][2], 1.0f, true);
        }
        g_customStopCount = 3;
        for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++) g_customStops[k][c] = newStops[k][c];
        DrawGrad(p, r.preview.left, r.preview.top, r.preview.right - r.preview.left,
                 r.preview.bottom - r.preview.top, PalCustom);
        p.Rect(r.nameBox.left, r.nameBox.top, r.nameBox.right - r.nameBox.left,
               r.nameBox.bottom - r.nameBox.top, 0.10f, 0.11f, 0.13f, 0.96f, true);
        bool placeholder = newName.empty() && !nameFocused;
        std::wstring shown = placeholder ? L"Palette name" : (newName + (nameFocused ? L"_" : L""));
        p.Text(r.nameBox.left + 10, r.nameBox.top + 6, 230, 22, shown.c_str(),
               placeholder ? 0.5f : 0.92f, placeholder ? 0.52f : 0.93f, placeholder ? 0.54f : 0.94f, 0.95f);
        p.Rect(r.addBtn.left, r.addBtn.top, r.addBtn.right - r.addBtn.left,
               r.addBtn.bottom - r.addBtn.top, 0.16f, 0.34f, 0.62f, 0.98f, true);
        p.Text(r.addBtn.left, r.addBtn.top, r.addBtn.right - r.addBtn.left, r.addBtn.bottom - r.addBtn.top,
               L"add palette", 1.0f, 1.0f, 1.0f, 0.98f, true);
    }
    // Handle a click while the panel is open. Returns true if consumed.
    bool PanelClick(int mx, int my) {
        if (!palPanel) return false;
        D2D1_SIZE_F sz = rt->GetSize();
        PalRects r; PalLayout(sz.width, sz.height, r);
        if (In(r.close, mx, my)) { palPanel = false; return true; }
        if (In(r.master, mx, my)) { shufflePalettes = !shufflePalettes; SaveConfig(); return true; }
        for (int i = 0; i < r.rows; i++) if (In(r.row[i], mx, my)) {
            if (mx <= r.row[i].left + 24) { palInclude[i] = !palInclude[i]; SaveConfig(); }
            else paletteIdx = i;
            return true;
        }
        for (int k = 0; k < 3; k++) if (In(r.swatch[k], mx, my)) { PickStop(k); return true; }
        if (In(r.nameBox, mx, my)) { nameFocused = true; return true; }
        if (In(r.addBtn, mx, my)) { AddNewPalette(); return true; }
        nameFocused = false;
        if (!In(r.card, mx, my)) palPanel = false; // click outside closes
        return true; // swallow clicks while the panel is up
    }

    // ----- settings persistence (wavescope.ini beside the exe) -----

    std::wstring ConfigPath() const { return PmExeDir() + L"wavescope.ini"; }

    void SaveConfig() {
        std::wstring path = ConfigPath();
        auto wi = [&](const wchar_t* k, int v) {
            wchar_t b[16]; _itow_s(v, b, 10);
            WritePrivateProfileStringW(L"settings", k, b, path.c_str());
        };
        wi(L"mode", modeIdx); wi(L"palette", paletteIdx); wi(L"resolution", resIdx);
        wi(L"shuffle", shuffleIdx); wi(L"calm", calm ? 1 : 0); wi(L"shader", shaderIdx);
        wi(L"shufflePal", shufflePalettes ? 1 : 0);
        EnsurePalInclude();
        std::wstring inc;
        for (char v : palInclude) inc += (v ? L'1' : L'0');
        WritePrivateProfileStringW(L"settings", L"palInclude", inc.c_str(), path.c_str());
        wchar_t cnt[16]; _itow_s((int)customPalettes.size(), cnt, 10);
        WritePrivateProfileStringW(L"palettes", L"count", cnt, path.c_str());
        for (size_t i = 0; i < customPalettes.size(); i++) {
            const CustomPalette& cp = customPalettes[i];
            std::wstring val = cp.name + L"|";
            for (int s = 0; s < cp.n; s++) {
                wchar_t hx[10];
                swprintf_s(hx, L"%02x%02x%02x", (int)(cp.stops[s][0] * 255 + 0.5f),
                           (int)(cp.stops[s][1] * 255 + 0.5f), (int)(cp.stops[s][2] * 255 + 0.5f));
                if (s) val += L",";
                val += hx;
            }
            wchar_t key[16]; swprintf_s(key, L"p%d", (int)i);
            WritePrivateProfileStringW(L"palettes", key, val.c_str(), path.c_str());
        }
    }

    void LoadConfig() {
        std::wstring path = ConfigPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        int cnt = GetPrivateProfileIntW(L"palettes", L"count", 0, path.c_str());
        for (int i = 0; i < cnt && i < 64; i++) {
            wchar_t key[16]; swprintf_s(key, L"p%d", i);
            wchar_t buf[160];
            GetPrivateProfileStringW(L"palettes", key, L"", buf, 160, path.c_str());
            std::wstring v = buf;
            size_t bar = v.find(L'|');
            if (bar == std::wstring::npos) continue;
            CustomPalette cp; cp.name = v.substr(0, bar); cp.n = 0;
            std::wstring hexes = v.substr(bar + 1);
            size_t pos = 0;
            while (cp.n < 3 && pos < hexes.size()) {
                size_t comma = hexes.find(L',', pos);
                std::wstring h = hexes.substr(pos, comma == std::wstring::npos ? comma : comma - pos);
                if (h.size() >= 6) {
                    unsigned int rgb = wcstoul(h.c_str(), nullptr, 16);
                    cp.stops[cp.n][0] = ((rgb >> 16) & 255) / 255.0f;
                    cp.stops[cp.n][1] = ((rgb >> 8) & 255) / 255.0f;
                    cp.stops[cp.n][2] = (rgb & 255) / 255.0f;
                    cp.n++;
                }
                if (comma == std::wstring::npos) break;
                pos = comma + 1;
            }
            if (cp.n >= 1) customPalettes.push_back(cp);
        }
        modeIdx = Clampi(GetPrivateProfileIntW(L"settings", L"mode", 0, path.c_str()), 0, MODE_COUNT - 1);
        paletteIdx = Clampi(GetPrivateProfileIntW(L"settings", L"palette", 0, path.c_str()), 0, PaletteCount() - 1);
        resIdx = Clampi(GetPrivateProfileIntW(L"settings", L"resolution", 0, path.c_str()), 0, RES_COUNT - 1);
        shuffleIdx = Clampi(GetPrivateProfileIntW(L"settings", L"shuffle", 0, path.c_str()), 0, SHUFFLE_COUNT - 1);
        calm = GetPrivateProfileIntW(L"settings", L"calm", 0, path.c_str()) != 0;
        shaderIdx = Clampi(GetPrivateProfileIntW(L"settings", L"shader", 0, path.c_str()), 0, SV_COUNT - 1);
        shufflePalettes = GetPrivateProfileIntW(L"settings", L"shufflePal", 0, path.c_str()) != 0;
        wchar_t inc[128];
        GetPrivateProfileStringW(L"settings", L"palInclude", L"", inc, 128, path.c_str());
        palInclude.clear();
        for (wchar_t* q = inc; *q; ++q) palInclude.push_back(*q == L'0' ? 0 : 1);
        EnsurePalInclude(); // new palettes default to included
    }

    // Release everything this window owns (GL hosts, audio tap, D2D).
    void Shutdown() {
        shaderHost.Destroy();
        pmHost.Destroy();
        tap.Stop();
        ReleaseOffscreen();
        ReleaseOverlay();
        SafeRelease(&wic);
        SafeRelease(&brush);
        SafeRelease(&rt);
        SafeRelease(&deckFont);
        SafeRelease(&tinyFont);
        SafeRelease(&hudFont);
        SafeRelease(&dwrite);
        SafeRelease(&d2d);
    }
};

// Self-test harness (WaveScope.exe selftest): drives every engine on the demo
// signal, dumps PNGs of what rendered, writes wavescope-selftest.log.
#include "selftest.inl"

// One App per display, heap-allocated so the pointers stashed in each window
// (GWLP_USERDATA) stay valid regardless of vector growth.
static std::vector<App*> g_views;

// The App that owns a given window, stashed at creation in GWLP_USERDATA.
static App* AppFromHwnd(HWND h) {
    return (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

// ----- polish: tray icon, kiosk cursor-hide, launch-at-login -----

#define WM_TRAY       (WM_APP + 1)
#define IDM_TRAY_SHOW 1
#define IDM_TRAY_QUIT 2

static NOTIFYICONDATAW g_tray = {};
static bool g_trayAdded = false;
static bool g_borderless = false;   // wall / kiosk: fullscreen-style windows
static bool g_cursorHidden = false;
static POINT g_lastCursor = {};
static float g_lastMouseMove = 0;

static void TrayAdd(HWND hwnd) {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_tray.szTip, L"WaveScope");
    if (Shell_NotifyIconW(NIM_ADD, &g_tray)) g_trayAdded = true;
}

static void TrayRemove() {
    if (g_trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g_tray); g_trayAdded = false; }
}

// Add or remove the HKCU Run entry (invoked only via an explicit command-line
// action the user runs deliberately).
static void SetAutoStart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = std::wstring(L"\"") + exe + L"\"";
        RegSetValueExW(k, L"WaveScope", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        MessageBoxW(nullptr, L"WaveScope will now launch at login.", L"WaveScope",
                    MB_OK | MB_ICONINFORMATION);
    } else {
        RegDeleteValueW(k, L"WaveScope");
        MessageBoxW(nullptr, L"WaveScope login launch removed.", L"WaveScope",
                    MB_OK | MB_ICONINFORMATION);
    }
    RegCloseKey(k);
}

// Hide the cursor after it sits idle over a fullscreen-style window; any
// movement brings it back. Polled (works over the GL child windows too).
static void UpdateCursorHide(float now) {
    POINT cp; GetCursorPos(&cp);
    if (cp.x != g_lastCursor.x || cp.y != g_lastCursor.y) {
        g_lastCursor = cp; g_lastMouseMove = now;
        if (g_cursorHidden) { while (ShowCursor(TRUE) < 0) {} g_cursorHidden = false; }
    } else if (g_borderless && !g_cursorHidden && now - g_lastMouseMove > 3.0f) {
        while (ShowCursor(FALSE) >= 0) {}
        g_cursorHidden = true;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        // Bind the App* passed as CreateWindow's lpParam to this window.
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    App* a = AppFromHwnd(hwnd);
    if (!a) return DefWindowProc(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        if (a->rt) a->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        if (a->pmHost.ready) a->pmHost.Resize(LOWORD(lp), HIWORD(lp));
        if (a->shaderHost.ready) a->shaderHost.Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_SETCURSOR:
        // Force the arrow over the client area. The bottom sizing border flips
        // the cursor to a resize double-arrow when you graze it to reveal the
        // deck, and it can stay stuck; this pins it back. Borders still resize.
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE; }
        break;
    case WM_MOUSEMOVE:
        a->OnMouseMove((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONDOWN:
        a->OnClick((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_SPACE:
        case VK_RIGHT:
            if (a->pmActive) a->pmHost.NextPreset(1);
            else if (a->shaderActive) a->shaderIdx = (a->shaderIdx + 1) % a->shaderHost.ShaderCount();
            else a->CycleMode(1);
            break;
        case VK_LEFT:
            if (a->pmActive) a->pmHost.NextPreset(-1);
            else if (a->shaderActive) a->shaderIdx = (a->shaderIdx + a->shaderHost.ShaderCount() - 1) % a->shaderHost.ShaderCount();
            else a->CycleMode(-1);
            break;
        case 'P':      a->CyclePalette(); break;
        case 'N':      a->OpenPalettePanel(); break;
        case '1':      a->tap.SetSource(AudioTap::SRC_LOOPBACK); break;
        case '2':      a->tap.SetSource(AudioTap::SRC_MIC); break;
        case '3':      a->tap.SetSource(AudioTap::SRC_DEMO); break;
        case '4':      a->OpenFile(); break;
        case 'S':      a->CycleShuffle(); break;
        case 'C':      a->SetCalm(!a->calm); break;
        case 'R':      a->resIdx = (a->resIdx + 1) % RES_COUNT; break;
        case 'L':      a->OpenPresetFolder(); break;
        case 'M':      a->ToggleProjectM(); break;
        case 'G':      a->ToggleShader(); break;
        case 'B':      g_spotify.OneButton(); break;  // Spotify: authorize + play
        case 'F':      a->ToggleFullscreen(); break;
        case 'H':      a->hud = !a->hud; break;
        case VK_F1:    a->help = !a->help; break;
        case VK_OEM_2: if (GetKeyState(VK_SHIFT) & 0x8000) a->help = !a->help; break; // '?'
        case VK_ESCAPE:
            if (a->palPanel) a->palPanel = false;      // ESC closes the panel first
            else PostQuitMessage(0);
            break;
        }
        return 0;
    case WM_CHAR:
        a->PanelChar((wchar_t)wp);
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, IsWindowVisible(hwnd) ? L"Hide" : L"Show");
            AppendMenuW(m, MF_STRING, IDM_TRAY_QUIT, L"Quit");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            if (IsWindowVisible(hwnd)) SetForegroundWindow(hwnd);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_TRAY_SHOW) {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
            if (IsWindowVisible(hwnd)) SetForegroundWindow(hwnd);
        } else if (LOWORD(wp) == IDM_TRAY_QUIT) {
            PostQuitMessage(0);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && g_trayAdded) { ShowWindow(hwnd, SW_HIDE); return 0; }
        break;  // other syscommands fall through to DefWindowProc
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// Create one App-backed window. `rc` is the target rect; borderless => a
// popup filling that monitor (video-wall mode), else a normal 1440x900 window.
static App* CreateView(HINSTANCE inst, const wchar_t* cls, RECT rc, bool borderless, int nCmdShow) {
    App* v = new App();
    v->inst = inst;
    QueryPerformanceFrequency(&v->qpcFreq);
    QueryPerformanceCounter(&v->qpcStart);
    DWORD style = borderless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    v->hwnd = CreateWindowExW(0, cls, L"WaveScope", style,
                              rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, inst, v);
    if (!v->hwnd || !v->CreateGraphics()) { delete v; return nullptr; }
    v->tap.Start();
    v->LoadConfig();
    ShowWindow(v->hwnd, borderless ? SW_SHOW : nCmdShow);
    g_views.push_back(v);
    return v;
}

// Collect each display's bounding rect for the video wall.
static BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* rects = (std::vector<RECT>*)lp;
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (GetMonitorInfoW(mon, &mi)) rects->push_back(mi.rcMonitor);
    return TRUE;
}

// Parsed command line: whole-token flags only, so a path like
// "C:\music\demo tracks\x.mp3" can never trip the demo/milkdrop flags, and
// flags after file=... are not swallowed into the path (quotes work through
// CommandLineToArgvW as usual).
struct LaunchOpts {
    bool demo = false, milkdrop = false, shader = false, wall = false,
         kiosk = false, installStartup = false, removeStartup = false,
         selftest = false;
    std::wstring file;
};

static LaunchOpts ParseCmdLine() {
    LaunchOpts o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;
    for (int i = 1; i < argc; i++) {
        std::wstring t = argv[i];
        if      (t == L"demo")            o.demo = true;
        else if (t == L"milkdrop")        o.milkdrop = true;
        else if (t == L"shader")          o.shader = true;
        else if (t == L"wall")            o.wall = true;
        else if (t == L"kiosk")           o.kiosk = true;
        else if (t == L"install-startup") o.installStartup = true;
        else if (t == L"remove-startup")  o.removeStartup = true;
        else if (t == L"selftest")        o.selftest = true;
        else if (t.rfind(L"file=", 0) == 0) o.file = t.substr(5);
    }
    LocalFree(argv);
    return o;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // UI-thread shell dialogs

    LaunchOpts opts = ParseCmdLine();

    // Explicit launch-at-login toggles (user runs these deliberately).
    if (opts.installStartup) { SetAutoStart(true);  return 0; }
    if (opts.removeStartup)  { SetAutoStart(false); return 0; }

    g_spotify.LoadToken(); // restore the DPAPI-encrypted Spotify refresh token

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WaveScopeWindow";
    RegisterClassW(&wc);

    // "wall" spreads one borderless window across every display (video wall).
    // "kiosk" is a single borderless full-screen window on the primary display
    // with an idle cursor-hide. Otherwise a single 1440x900 window.
    bool wall = opts.wall;
    bool kiosk = opts.kiosk;
    g_borderless = wall || kiosk;
    std::vector<RECT> monitors;
    if (wall) {
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, (LPARAM)&monitors);
    } else if (kiosk) {
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi))
            monitors.push_back(mi.rcMonitor);
    }
    if (monitors.empty()) monitors.push_back(RECT{ CW_USEDEFAULT, CW_USEDEFAULT,
                                                   CW_USEDEFAULT + 1440, CW_USEDEFAULT + 900 });

    for (size_t i = 0; i < monitors.size(); i++) {
        App* v = CreateView(hInstance, wc.lpszClassName, monitors[i], g_borderless, nCmdShow);
        if (!v) { if (i == 0) return 1; else continue; }
        // Video wall: give each extra screen a distinct starting visual.
        if (wall && i > 0) {
            v->modeIdx = (v->modeIdx + (int)i * 5) % MODE_COUNT;
            v->paletteIdx = (v->paletteIdx + (int)i) % v->PaletteCount();
        }
    }
    if (g_views.empty()) return 1;
    TrayAdd(g_views[0]->hwnd); // one tray icon for the app (primary window)

    // Command-line, applied to every window: "demo" synthetic signal,
    // "file=<path>" playback, "milkdrop"/"shader" boot into those engines.
    for (App* v : g_views) {
        if (opts.demo) v->tap.SetSource(AudioTap::SRC_DEMO);
        if (!opts.file.empty()) v->tap.SetFile(opts.file.c_str());
        if (opts.milkdrop) v->ToggleProjectM();
        if (opts.shader) v->ToggleShader();
    }

    if (opts.selftest) {
        int rc = RunSelfTest(g_views[0]);
        TrayRemove();
        for (App* v : g_views) { v->Shutdown(); delete v; }
        g_views.clear();
        return rc;
    }

    // Render loop: each window presents on vsync, so this paces itself.
    MSG msg = {};
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        for (App* v : g_views) {
            v->Render();
            if (v->pmHost.ready) v->pmHost.PollReload(); // hot-reload .milk folder
        }
        UpdateCursorHide(g_views[0]->Now());
    }
done:
    if (!g_views.empty()) g_views[0]->SaveConfig(); // primary window's setup persists
    if (g_cursorHidden) { while (ShowCursor(TRUE) < 0) {} g_cursorHidden = false; }
    TrayRemove();
    for (App* v : g_views) { v->Shutdown(); delete v; }
    g_views.clear();
    return 0;
}
