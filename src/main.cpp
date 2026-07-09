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
#include <commdlg.h>
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
#include "projectm_host.inl"

// --------------------------------------------------------------------- app

// Shuffle timer choices, in seconds; 0 = off.
static const int SHUFFLE_SECS[] = { 0, 15, 30, 60, 300 };
static const int SHUFFLE_COUNT = (int)(sizeof(SHUFFLE_SECS) / sizeof(SHUFFLE_SECS[0]));

// Render-resolution targets (backing-store height); 0 = auto (window size).
struct ResOpt { const wchar_t* name; int height; };
static const ResOpt RES_OPTS[] = {
    { L"AUTO",  0 }, { L"1080", 1080 }, { L"1440", 1440 }, { L"4K", 2160 }, { L"8K", 4320 },
};
static const int RES_COUNT = (int)(sizeof(RES_OPTS) / sizeof(RES_OPTS[0]));

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
    ID2D1BitmapRenderTarget* offscreen = nullptr;
    ID2D1SolidColorBrush* offBrush = nullptr;
    UINT offW = 0, offH = 0;
    int modeIdx = 0;
    int paletteIdx = 0;
    int resIdx = 0;          // index into RES_OPTS
    bool calm = false;
    int shuffleIdx = 0;      // index into SHUFFLE_SECS
    float lastShuffle = 0;
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

        // Shuffle timer: hop to a random other visual (or preset) on interval.
        if (SHUFFLE_SECS[shuffleIdx] > 0 && now - lastShuffle >= (float)SHUFFLE_SECS[shuffleIdx]) {
            lastShuffle = now;
            if (pmActive) pmHost.NextPreset(1);
            else modeIdx = PickRandomMode();
        }

        AudioFrame frame;
        tap.Analyze(frame, now);
        if (calm) frame.beat *= 0.2f; // photosensitivity: tame the pulse

        // projectM engine: render OpenGL on the child window instead of D2D.
        if (pmActive && pmHost.ready) {
            pmHost.RenderFrame(frame);
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
        DrawCtx g{ offscreen, d2d, offBrush, PALETTES[paletteIdx].color, (float)offW, (float)offH, &frame };
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

        if (hud && hudFont) {
            wchar_t text[360];
            static const wchar_t* shuf[] = { L"off", L"15s", L"30s", L"1m", L"5m" };
            swprintf_s(text, L"SPECTRAVIZ  MODE %02d/%d %s  TRACE %s  SRC %s  RES %s (%ux%u)  SHUF %s%s  FPS %d  LEVEL %.2f\nSPACE/\x2190\x2192 cycle  P palette  1 sys  2 mic  3 demo  4 file  R res  S shuffle  C calm  M milkdrop  F full  H hud  ESC",
                       modeIdx + 1, MODE_COUNT, mode.name, PALETTES[paletteIdx].name, tap.SourceName(),
                       RES_OPTS[resIdx].name, offW, offH, shuf[shuffleIdx], calm ? L"  CALM" : L"", fps, frame.level);
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
            else g_app.CycleMode(1);
            break;
        case VK_LEFT:
            if (g_app.pmActive) g_app.pmHost.NextPreset(-1);
            else g_app.CycleMode(-1);
            break;
        case 'P':      g_app.paletteIdx = (g_app.paletteIdx + 1) % PAL_COUNT; break;
        case '1':      g_app.tap.SetSource(AudioTap::SRC_LOOPBACK); break;
        case '2':      g_app.tap.SetSource(AudioTap::SRC_MIC); break;
        case '3':      g_app.tap.SetSource(AudioTap::SRC_DEMO); break;
        case '4':      g_app.OpenFile(); break;
        case 'S':      g_app.CycleShuffle(); break;
        case 'C':      g_app.SetCalm(!g_app.calm); break;
        case 'R':      g_app.resIdx = (g_app.resIdx + 1) % RES_COUNT; break;
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

    // Command-line: "demo" starts on the synthetic signal, "file=<path>"
    // plays an audio file, "milkdrop" boots straight into the projectM engine.
    if (pCmdLine && wcsstr(pCmdLine, L"demo")) g_app.tap.SetSource(AudioTap::SRC_DEMO);
    if (pCmdLine) { const wchar_t* p = wcsstr(pCmdLine, L"file="); if (p) g_app.tap.SetFile(p + 5); }
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
    g_app.ReleaseOffscreen();
    SafeRelease(&g_app.brush);
    SafeRelease(&g_app.rt);
    SafeRelease(&g_app.hudFont);
    SafeRelease(&g_app.dwrite);
    SafeRelease(&g_app.d2d);
    return 0;
}
