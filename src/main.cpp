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

// Render-resolution targets (backing-store height); 0 = auto (window size).
struct ResOpt { const wchar_t* name; int height; };
static const ResOpt RES_OPTS[] = {
    { L"AUTO",  0 }, { L"1080", 1080 }, { L"1440", 1440 }, { L"4K", 2160 }, { L"8K", 4320 },
};
static const int RES_COUNT = (int)(sizeof(RES_OPTS) / sizeof(RES_OPTS[0]));

// A user-created palette: 1-3 RGB stops (0..1), persisted to the config file.
struct CustomPalette { std::wstring name; int n = 1; float stops[3][3] = {}; };

static int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct App {
    HWND hwnd = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* hudFont = nullptr;
    IDWriteTextFormat* deckFont = nullptr;   // centered, for deck buttons
    AudioTap tap;
    HINSTANCE inst = nullptr;
    ProjectMHost pmHost;
    bool pmActive = false;
    ShaderHost shaderHost;
    bool shaderActive = false;
    int shaderIdx = 0;       // index into the GPU shader modes
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
    int modeIdx = 0;
    int paletteIdx = 0;
    int resIdx = 0;          // index into RES_OPTS
    bool calm = false;
    int shuffleIdx = 0;      // index into SHUFFLE_SECS
    float lastShuffle = 0;
    bool hud = true;
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
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 13.0f, L"en-us", &deckFont);
        if (deckFont) {
            deckFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            deckFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
    // into. Capped at 1440 tall to bound the per-change texture upload; the
    // aspect is preserved so deck hit-testing (in window pixels) stays aligned.
    bool EnsureOverlayRT(int clientW, int clientH) {
        if (!wic) return false;
        UINT w, h;
        if (clientH <= 1440) { w = (UINT)(clientW > 0 ? clientW : 1); h = (UINT)(clientH > 0 ? clientH : 1); }
        else {
            h = 1440;
            w = (UINT)((float)h * (float)clientW / (float)clientH + 0.5f);
            if (w < 1) w = 1;
        }
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
        wchar_t sig[900];
        swprintf_s(sig, L"%u|%u|%d|%d|%d|%s|%s", olW, olH, deckVisible ? 1 : 0,
                   deckHover, help ? 1 : 0, l1 ? l1 : L"", l2 ? l2 : L"");
        if (olSig == sig) return false; // unchanged — reuse the cached texture
        olSig = sig;

        olRT->BeginDraw();
        olRT->Clear(D2D1::ColorF(0, 0.0f));   // transparent
        D2DPainter p(olRT, olBrush, hudFont, deckFont);
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

        // Shuffle timer: hop to a random other visual (or preset) on interval.
        if (SHUFFLE_SECS[shuffleIdx] > 0 && now - lastShuffle >= (float)SHUFFLE_SECS[shuffleIdx]) {
            lastShuffle = now;
            if (pmActive) pmHost.NextPreset(1);
            else if (shaderActive) shaderIdx = (shaderIdx + 1) % shaderHost.ShaderCount();
            else modeIdx = PickRandomMode();
        }

        AudioFrame frame;
        tap.Analyze(frame, now);
        if (calm) frame.beat *= 0.2f; // photosensitivity: tame the pulse

        // Auto-hide the deck once idle (shared by every engine).
        if (deckVisible && now - deckActivity > 2.5f) { deckVisible = false; deckHover = -1; }
        static const wchar_t* SHUF[] = { L"off", L"15s", L"30s", L"1m", L"5m" };

        RECT crc; GetClientRect(hwnd, &crc);

        // projectM engine: render OpenGL on the child window, then composite
        // the D2D-rendered UI overlay (crisp, identical to the built-in view)
        // as a texture on top.
        if (pmActive && pmHost.ready) {
            wchar_t l1[300], l2[280];
            swprintf_s(l1, L"SPECTRAVIZ  projectM  %s  (%d presets)  SRC %s  SHUF %s  FPS %d",
                       pmHost.PresetName(), pmHost.PresetCount(), tap.SourceName(), SHUF[shuffleIdx], fps);
            swprintf_s(l2, L"SPACE/\x2190\x2192 preset  L folder  1-4 src  S shuffle  M exit  G gpu  B spotify  F full  H hud  F1 help  ESC");
            bool dirty = BuildOverlay(crc.right, crc.bottom, l1, l2);
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
            wchar_t l1[300], l2[280];
            swprintf_s(l1, L"SPECTRAVIZ  GPU shader  %s  (%d/%d)  SRC %s  SHUF %s  FPS %d",
                       shaderHost.ShaderName(shaderIdx), (shaderIdx % shaderHost.ShaderCount()) + 1,
                       shaderHost.ShaderCount(), tap.SourceName(), SHUF[shuffleIdx], fps);
            swprintf_s(l2, L"SPACE/\x2190\x2192 shader  1-4 src  S shuffle  G exit  M milk  B spotify  F full  H hud  F1 help  ESC");
            bool dirty = BuildOverlay(crc.right, crc.bottom, l1, l2);
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

        wchar_t l1[300], l2[280];
        swprintf_s(l1, L"SPECTRAVIZ  MODE %02d/%d %s  TRACE %s  SRC %s  RES %s (%ux%u)  SHUF %s%s  FPS %d",
                   modeIdx + 1, MODE_COUNT, mode.name, PaletteName(), tap.SourceName(),
                   RES_OPTS[resIdx].name, offW, offH, SHUF[shuffleIdx], calm ? L"  CALM" : L"", fps);
        swprintf_s(l2, L"SPACE/\x2190\x2192 cycle  P palette  N new  1-4 src  R res  S shuffle  C calm  M milk  G gpu  B spotify  F full  H hud  F1 help  ESC");
        D2DPainter dp(rt, brush, hudFont, deckFont);
        DrawUI(dp, l1, l2);

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
    enum { DK_SYS, DK_MIC, DK_DEMO, DK_FILE, DK_MODES, DK_MILK, DK_GPU,
           DK_PREV, DK_NEXT, DK_PAL, DK_RES, DK_SHUF, DK_CALM, DK_HUD, DK_SPOT, DK_N };

    static const wchar_t* DeckLabel(int i) {
        static const wchar_t* L_[DK_N] = {
            L"SYS", L"MIC", L"DEMO", L"FILE", L"MODES", L"MILK", L"GPU",
            L"\x2039", L"\x203A", L"PAL", L"RES", L"SHUF", L"CALM", L"HUD", L"SPOT" };
        return L_[i];
    }

    // Fill `out` with each button's rect and return the bar background rect.
    D2D1_RECT_F DeckLayout(float W, float H, D2D1_RECT_F out[DK_N]) {
        const float margin = 20, barH = 50, gap = 5, bottom = 14;
        float x0 = margin, x1 = W - margin;
        float y0 = H - barH - bottom, y1 = y0 + barH;
        float bw = (x1 - x0 - gap * (int)(DK_N - 1)) / (float)DK_N;
        for (int i = 0; i < DK_N; i++) {
            float bx = x0 + i * (bw + gap);
            out[i] = D2D1::RectF(bx, y0, bx + bw, y1);
        }
        return D2D1::RectF(x0 - 8, y0 - 8, x1 + 8, y1 + 8);
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
        case DK_MODES: return !pmActive && !shaderActive;
        case DK_MILK:  return pmActive;
        case DK_GPU:   return shaderActive;
        case DK_SHUF:  return shuffleIdx > 0;
        case DK_CALM:  return calm;
        case DK_HUD:   return hud;
        case DK_SPOT:  return g_spotify.busy.load();
        default:       return false;
        }
    }

    void DeckDispatch(int i) {
        switch (i) {
        case DK_SYS:   tap.SetSource(AudioTap::SRC_LOOPBACK); break;
        case DK_MIC:   tap.SetSource(AudioTap::SRC_MIC); break;
        case DK_DEMO:  tap.SetSource(AudioTap::SRC_DEMO); break;
        case DK_FILE:  OpenFile(); break;
        case DK_MODES: if (pmActive) ToggleProjectM(); else if (shaderActive) ToggleShader(); break;
        case DK_MILK:  if (!pmActive) ToggleProjectM(); break;
        case DK_GPU:   if (!shaderActive) ToggleShader(); break;
        case DK_PREV:  CycleMode(-1); break;
        case DK_NEXT:  CycleMode(1); break;
        case DK_PAL:   CyclePalette(); break;
        case DK_RES:   resIdx = (resIdx + 1) % RES_COUNT; break;
        case DK_SHUF:  CycleShuffle(); break;
        case DK_CALM:  SetCalm(!calm); break;
        case DK_HUD:   hud = !hud; break;
        case DK_SPOT:  g_spotify.OneButton(); break;
        }
    }

    // Mouse near the bottom edge reveals the deck; hovering keeps it alive.
    void OnMouseMove(int mx, int my) {
        RECT rc; GetClientRect(hwnd, &rc);
        int hit = DeckHitTest(mx, my);
        if (my > rc.bottom - 110 || hit >= 0) { deckVisible = true; deckActivity = Now(); }
        deckHover = deckVisible ? hit : -1;
    }

    void OnClick(int mx, int my) {
        if (!deckVisible) return;
        int hit = DeckHitTest(mx, my);
        if (hit >= 0) { DeckDispatch(hit); deckActivity = Now(); }
    }

    // The whole on-screen UI, drawn through the abstract painter so it renders
    // identically over every engine. `l1`/`l2` are the engine-specific HUD
    // status + hint lines built by Render.
    void DrawUI(UIPainter& p, const wchar_t* l1, const wchar_t* l2) {
        float W = p.W(), H = p.H();
        if (hud) {
            p.Rect(0, 0, W, 46, 0.043f, 0.051f, 0.047f, 0.42f);
            if (l1) p.Text(16, 4, W - 32, 20, l1, 0.95f, 0.96f, 0.95f, 0.92f);
            if (l2) p.Text(16, 24, W - 32, 20, l2, 0.80f, 0.83f, 0.82f, 0.80f);
        }
        if (deckVisible) DrawDeckP(p);
        if (help) DrawHelpP(p);
    }

    void DrawDeckP(UIPainter& p) {
        D2D1_RECT_F b[DK_N];
        D2D1_RECT_F bar = DeckLayout(p.W(), p.H(), b);
        p.Rect(bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top,
               0.03f, 0.04f, 0.04f, 0.86f, true);
        for (int i = 0; i < DK_N; i++) {
            bool active = DeckActive(i), hover = (i == deckHover);
            float br_ = active ? 0.20f : (hover ? 0.16f : 0.09f);
            float bg_ = active ? 0.42f : (hover ? 0.19f : 0.11f);
            float bb_ = active ? 0.72f : (hover ? 0.22f : 0.12f);
            p.Rect(b[i].left, b[i].top, b[i].right - b[i].left, b[i].bottom - b[i].top,
                   br_, bg_, bb_, 0.93f, true);
            float tr = active ? 1.0f : 0.86f, tg = active ? 1.0f : 0.88f, tb = active ? 1.0f : 0.88f;
            p.Text(b[i].left, b[i].top, b[i].right - b[i].left, b[i].bottom - b[i].top,
                   DeckLabel(i), tr, tg, tb, 0.96f, true);
        }
    }

    // Full keymap overlay (F1 / ?), a centered card over any engine.
    void DrawHelpP(UIPainter& p) {
        static const wchar_t* HELP =
            L"SPECTRAVIZ  \x2014  KEYS\n"
            L"\n"
            L"SPACE / \x2192      next mode (or preset / shader)\n"
            L"\x2190              previous\n"
            L"P              cycle trace palette\n"
            L"N              new custom palette (color picker)\n"
            L"1 / 2 / 3      source: system / mic / demo\n"
            L"4              open an audio file\n"
            L"M              MilkDrop engine (projectM)\n"
            L"G              GPU shader engine\n"
            L"B              Spotify: authorize + start playback (Premium)\n"
            L"S              shuffle timer (off / 15s / 30s / 1m / 5m)\n"
            L"C              calm mode (photosensitive-safe)\n"
            L"R              resolution (AUTO / 1080 / 1440 / 4K / 8K)\n"
            L"L              MilkDrop preset folder\n"
            L"F              fullscreen        H   HUD on/off\n"
            L"F1 / ?         this help         ESC quit\n"
            L"\n"
            L"Launch: SpectraViz.exe [demo | file=PATH | milkdrop | shader | wall | kiosk]\n"
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

    // Switch between the built-in Direct2D modes and the GPU shader engine.
    void ToggleShader() {
        if (!shaderActive) {
            if (pmActive) { pmActive = false; pmHost.Show(false); }
            if (!shaderHost.ready && !shaderHost.Init(inst, hwnd)) {
                SetWindowTextW(hwnd, L"SpectraViz  (GPU shaders failed to start)");
                return;
            }
            RECT rc; GetClientRect(hwnd, &rc);
            shaderHost.Resize(rc.right, rc.bottom);
            shaderHost.Show(true);
            shaderActive = true;
            SetWindowTextW(hwnd, L"SpectraViz  GPU shader (G to exit, SPACE next shader)");
        } else {
            shaderActive = false;
            shaderHost.Show(false);
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
        SaveConfig();
    }

    // ----- settings persistence (spectraviz.ini beside the exe) -----

    std::wstring ConfigPath() const { return PmExeDir() + L"spectraviz.ini"; }

    void SaveConfig() {
        std::wstring path = ConfigPath();
        auto wi = [&](const wchar_t* k, int v) {
            wchar_t b[16]; _itow_s(v, b, 10);
            WritePrivateProfileStringW(L"settings", k, b, path.c_str());
        };
        wi(L"mode", modeIdx); wi(L"palette", paletteIdx); wi(L"resolution", resIdx);
        wi(L"shuffle", shuffleIdx); wi(L"calm", calm ? 1 : 0); wi(L"shader", shaderIdx);
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
        SafeRelease(&hudFont);
        SafeRelease(&dwrite);
        SafeRelease(&d2d);
    }
};

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
    wcscpy_s(g_tray.szTip, L"SpectraViz");
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
        RegSetValueExW(k, L"SpectraViz", 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        MessageBoxW(nullptr, L"SpectraViz will now launch at login.", L"SpectraViz",
                    MB_OK | MB_ICONINFORMATION);
    } else {
        RegDeleteValueW(k, L"SpectraViz");
        MessageBoxW(nullptr, L"SpectraViz login launch removed.", L"SpectraViz",
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
        case 'N':      a->NewPalette(); break;
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
        case VK_ESCAPE: PostQuitMessage(0); break;  // ESC on any window quits all
        }
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
    v->hwnd = CreateWindowExW(0, cls, L"SpectraViz", style,
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // UI-thread shell dialogs

    // Explicit launch-at-login toggles (user runs these deliberately).
    if (pCmdLine && wcsstr(pCmdLine, L"install-startup")) { SetAutoStart(true);  return 0; }
    if (pCmdLine && wcsstr(pCmdLine, L"remove-startup"))  { SetAutoStart(false); return 0; }

    g_spotify.LoadToken(); // restore the DPAPI-encrypted Spotify refresh token

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SpectraVizWindow";
    RegisterClassW(&wc);

    // "wall" spreads one borderless window across every display (video wall).
    // "kiosk" is a single borderless full-screen window on the primary display
    // with an idle cursor-hide. Otherwise a single 1440x900 window.
    bool wall = pCmdLine && wcsstr(pCmdLine, L"wall");
    bool kiosk = pCmdLine && wcsstr(pCmdLine, L"kiosk");
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
    const wchar_t* filePath = pCmdLine ? wcsstr(pCmdLine, L"file=") : nullptr;
    for (App* v : g_views) {
        if (pCmdLine && wcsstr(pCmdLine, L"demo")) v->tap.SetSource(AudioTap::SRC_DEMO);
        if (filePath) v->tap.SetFile(filePath + 5);
        if (pCmdLine && wcsstr(pCmdLine, L"milkdrop")) v->ToggleProjectM();
        if (pCmdLine && wcsstr(pCmdLine, L"shader")) v->ToggleShader();
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
