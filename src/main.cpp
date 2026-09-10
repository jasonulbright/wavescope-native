// WaveScope native: phase-1 shell.
//
// A classic, dependency-free Windows app: Win32 window, Direct2D rendering,
// WASAPI loopback capture (hears whatever the machine plays), own FFT.
// Everything below links against OS DLLs only; /MT static CRT means the
// shipped artifact is one .exe with zero runtime requirements.
//
// Keys: SPACE/RIGHT next (mode or preset), LEFT prev, E cycle engine,
// P palette, M displays, R record, F fullscreen, H HUD, ESC restore/quit —
// F1 / ? shows the full map.
//
// All 34 WaveScope modes are ported (viz_modes.inl) across six palettes, and
// the real MilkDrop engine runs via projectM (projectm_host.inl).

// Shared release version for the GUI, Windows resources and package workflow.
#include "version.h"

#include "resource.h"

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
#include <dwmapi.h>

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
#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------- utilities

template <typename T> static void SafeRelease(T** p) {
    if (*p) { (*p)->Release(); *p = nullptr; }
}

// The pure analysis math (AudioFrame, Hsla, BinLog, FFT, AnalyzeWindow) —
// shared with tests.cpp, which asserts it against the web contract.
#include "analysis.inl"

// ------------------------------------------------------------------- audio

// WASAPI loopback capture into a mono ring buffer + FFT analysis on demand.
class AudioTap {
public:
    bool Start() {
        m_thread = CreateThread(nullptr, 0, ThreadMain, this, 0, nullptr);
        return m_thread != nullptr;
    }

    void Stop() {
        m_quit = true;
        if (m_thread) { WaitForSingleObject(m_thread, INFINITE); CloseHandle(m_thread); m_thread = nullptr; }
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
    // Play + analyze a decoded audio file (Media Foundation). The generation
    // bump makes RunFile restart even when the source is already FILE, so
    // picking a second file switches tracks.
    void SetFile(const wchar_t* path) {
        AcquireSRWLockExclusive(&m_lock);
        wcsncpy_s(m_filePath, path, _TRUNCATE);
        ReleaseSRWLockExclusive(&m_lock);
        m_fileGen++;
        m_source = SRC_FILE;
    }

    // A consumer's read position into the tap. Each consumer (projectM feed,
    // recorder) owns one, including its own demo-synthesis clock.
    struct TapCursor { uint64_t idx = 0; double demoMs = 0; };

    // Copy up to `maxn` samples that arrived after the cursor (oldest first),
    // advancing it — for consumers that must see each sample exactly once,
    // not a re-sent sliding window. On the demo source the signal is
    // synthesized at real-time pace instead.
    int ReadNew(TapCursor& c, float* out, int maxn) {
        if (m_source.load() == SRC_DEMO) {
            double sr = (double)(m_sampleRate.load() ? m_sampleRate.load() : 48000);
            double nowMs = (double)GetTickCount64();
            if (c.demoMs == 0.0) c.demoMs = nowMs;
            int n = (int)((nowMs - c.demoMs) * sr / 1000.0);
            if (n <= 0) return 0;
            if (n > maxn) { c.demoMs = nowMs - (double)maxn * 1000.0 / sr; n = maxn; }
            double t0 = c.demoMs / 1000.0;
            // Wrap the detune LFO clock to keep float trig accurate on long
            // uptimes; one tiny wobble discontinuity every ~8 h is invisible.
            float cents[5]; DemoDetune((float)fmod(t0, 30000.0), cents);
            for (int i = 0; i < n; i++)
                out[i] = DemoSynth(t0 + (double)i / sr, cents);
            c.demoMs += (double)n * 1000.0 / sr;
            return n;
        }
        AcquireSRWLockShared(&m_lock);
        uint64_t w = m_writeIdx;
        if (c.idx > w) c.idx = w;
        uint64_t avail = w - c.idx;
        if (avail > (uint64_t)RING) { c.idx = w - RING; avail = RING; } // overran: skip ahead
        int n = (int)(avail < (uint64_t)maxn ? avail : (uint64_t)maxn);
        for (int i = 0; i < n; i++) out[i] = m_ring[(c.idx + i) % RING];
        ReleaseSRWLockShared(&m_lock);
        c.idx += n;
        return n;
    }

    // Snapshot the newest 2048 samples and analyze them.
    void Analyze(AudioFrame& f, float now) {
        float window[2048];
        if (m_source.load() == SRC_DEMO) {
            // Synthesize a coherent window ending at the current time. The
            // detune is sampled once per window so phase stays clean.
            float sr = (float)(m_sampleRate.load() ? m_sampleRate.load() : 48000);
            float cents[5]; DemoDetune(now, cents);
            for (int i = 0; i < 2048; i++)
                window[i] = DemoSynth((double)now - (double)(2048 - i) / sr, cents);
        } else {
            AcquireSRWLockShared(&m_lock);
            uint64_t w = m_writeIdx;
            for (int i = 0; i < 2048; i++) {
                uint64_t idx = w + RING - 2048 + i;
                window[i] = m_ring[idx % RING];
            }
            ReleaseSRWLockShared(&m_lock);
        }
        // The Web-Audio-analyser-matched math lives in analysis.inl
        // (tests.cpp asserts it against the contract).
        AnalyzeWindow(m_an, window, (float)(m_sampleRate.load() ? m_sampleRate.load() : 48000), now, f);
    }

private:
    static const int RING = 8192;

    static DWORD WINAPI ThreadMain(LPVOID param) {
        return ((AudioTap*)param)->Run();
    }

    // The demo signal, matching the web spec: saw + harmonic sine partials
    // with slow LFO detune, a 120->40 Hz kick every 500 ms, and the chord
    // retuned every 8 kicks (4 s) over roots A2/C3/G2/D3. Analysis-only.
    static const float* DemoRatios() {
        static const float r[5] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.02f };
        return r;
    }
    // Per-partial detune in cents, sampled once per synthesized block so the
    // phase math below (t*freq) stays artifact-free within the block.
    static void DemoDetune(float t, float cents[5]) {
        const float* ratios = DemoRatios();
        for (int k = 0; k < 5; k++) {
            float lfoHz = 0.05f + ratios[k] * 0.07f;
            cents[k] = 2.0f * ratios[k] * sinf(TAU * lfoHz * t);
        }
    }
    static float DemoSynth(double t, const float cents[5]) {
        static const float roots[4] = { 110.0f, 130.81f, 98.0f, 146.83f };
        int kickN = (int)(t / 0.5);                       // one kick per 500 ms
        float root = roots[((kickN / 8) % 4 + 4) % 4];    // retune every 8 kicks
        const float* ratios = DemoRatios();
        float s = 0.0f;
        for (int k = 0; k < 5; k++) {
            float freq = root * ratios[k] * powf(2.0f, cents[k] / 1200.0f);
            float ph = (float)(t * freq - floor(t * freq));
            float osc = ratios[k] < 2.0f ? (2.0f * ph - 1.0f) : sinf(TAU * ph);
            s += osc * (0.18f / ratios[k]);
        }
        // Kick: 120 Hz sweeping exponentially to 40 Hz over 120 ms (phase is
        // the closed-form integral), gain 0.9 decaying to ~0 by 250 ms.
        double kt = t - floor(t / 0.5) * 0.5;
        const double A = 9.1553;                 // ln(3)/0.12: 120->40 in 120 ms
        double phase = kt < 0.12
            ? 120.0 / A * (1.0 - exp(-A * kt))
            : 120.0 / A * (1.0 - exp(-A * 0.12)) + 40.0 * (kt - 0.12);
        s += sinf(TAU * (float)(phase - floor(phase))) * 0.9f * expf(-(float)kt * 27.0f);
        return s * 0.6f;
    }

    // Reopens capture when the user switches the Windows default device (the
    // old endpoint would otherwise keep "working", silently, until unplug).
    struct DeviceNotify : IMMNotificationClient {
        std::atomic<uint32_t>* gen = nullptr;
        ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
                *ppv = this; return S_OK;
            }
            *ppv = nullptr; return E_NOINTERFACE;
        }
        HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override {
            if (role == eConsole && gen) (*gen)++;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    };

    // Capture thread: (re)opens WASAPI for loopback or mic, and reinitializes
    // when the source changes, the default device changes, or the endpoint
    // fails (unplugged). Demo needs no capture (Analyze synthesizes).
    DWORD Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);
        DeviceNotify notify; notify.gen = &m_deviceGen;
        while (!m_quit) {
            int src = m_source.load();
            if (src == SRC_DEMO) { Sleep(30); continue; }
            if (src == SRC_FILE) { RunFile(); continue; }

            IMMDeviceEnumerator* devEnum = nullptr;
            IMMDevice* device = nullptr;
            IAudioClient* client = nullptr;
            IAudioCaptureClient* capture = nullptr;
            WAVEFORMATEX* fmt = nullptr;
            bool notifyOn = false;
            uint32_t devGen = m_deviceGen.load();

            EDataFlow flow = (src == SRC_MIC) ? eCapture : eRender;
            DWORD streamFlags = (src == SRC_MIC) ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK;

            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          __uuidof(IMMDeviceEnumerator), (void**)&devEnum);
            if (SUCCEEDED(hr))
                notifyOn = SUCCEEDED(devEnum->RegisterEndpointNotificationCallback(&notify));
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
                ULONGLONG lastData = GetTickCount64();

                while (!m_quit && m_source.load() == src && m_deviceGen.load() == devGen) {
                    Sleep(5);
                    UINT32 packet = 0;
                    bool got = false;
                    while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                        BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                        got = frames > 0;
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
                    ULONGLONG nowMs = GetTickCount64();
                    if (got) {
                        lastData = nowMs;
                    } else if (nowMs - lastData > 60) {
                        // Loopback stops delivering packets when the machine
                        // goes silent. Feed real-time zeros so the analysis
                        // decays to dark (web behavior) instead of freezing
                        // on the last window — and so projectM's PCM feed
                        // hears the silence too.
                        UINT32 zeros = (UINT32)((nowMs - lastData) * m_sampleRate / 1000);
                        UINT32 cap = m_sampleRate / 5;   // at most 200 ms per catch-up
                        if (zeros > cap) zeros = cap;
                        AcquireSRWLockExclusive(&m_lock);
                        for (UINT32 i = 0; i < zeros; i++) m_ring[(m_writeIdx++) % RING] = 0.0f;
                        ReleaseSRWLockExclusive(&m_lock);
                        lastData = nowMs;
                    }
                }
                client->Stop();
            }

            if (notifyOn) devEnum->UnregisterEndpointNotificationCallback(&notify);
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
        uint32_t gen = m_fileGen.load();   // a new SetFile restarts playback
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
            while (!m_quit && m_source.load() == SRC_FILE && m_fileGen.load() == gen) {
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

    HANDLE m_thread = nullptr;
    std::atomic<bool> m_quit{false};
    std::atomic<int> m_source{ SRC_LOOPBACK };
    std::atomic<uint32_t> m_fileGen{0};
    std::atomic<uint32_t> m_deviceGen{0};
    SRWLOCK m_lock = SRWLOCK_INIT;
    float m_ring[RING] = {};
    uint64_t m_writeIdx = 0;
    std::atomic<UINT32> m_sampleRate{48000};
    AnalysisState m_an;
    wchar_t m_filePath[MAX_PATH] = {};
};

// ------------------------------------------------------------------- modes

// Palettes, the DrawCtx primitives, all 34 modes, and the MODES[] registry.
// (BinLog lives in analysis.inl.)
#include "viz_modes.inl"

// The projectM (MilkDrop) engine host: child GL window + libprojectM.
#include "ui_paint.inl"
#include "projectm_host.inl"
#include "app_paths.inl"
#include "preset_save.inl"
#include "preset_favorites.inl"
#include "shader_host.inl"
#include "spotify.inl"
#include "options.inl"
#include "record.inl"
#include "spout_out.inl"

// --------------------------------------------------------------------- app

// Shuffle timer choices, in seconds; 0 = off. NEGATIVE = auto-DJ: the tick
// hops the ENGINE too (random engine + random visual), web-style. VJ_SEC is
// the audio-driven VJ (no timer): a bass drop hard-cuts, a lull gently morphs.
static const int VJ_SEC = -1;
static const int SHUFFLE_SECS[] = { 0, 15, 30, 60, 300, -30, VJ_SEC };
static const int SHUFFLE_COUNT = (int)(sizeof(SHUFFLE_SECS) / sizeof(SHUFFLE_SECS[0]));
static const wchar_t* SHUF_LABELS[] = { L"OFF", L"15s", L"30s", L"1m", L"5m", L"DJ", L"VJ" };

// Render-resolution targets (backing-store height); 0 = auto (window size).
struct ResOpt { const wchar_t* name; int height; };
static const ResOpt RES_OPTS[] = {
    { L"AUTO",  0 }, { L"1080", 1080 }, { L"1440", 1440 }, { L"4K", 2160 }, { L"8K", 4320 },
};
static const int RES_COUNT = (int)(sizeof(RES_OPTS) / sizeof(RES_OPTS[0]));

// A user-created palette: 1-3 RGB stops (0..1), persisted to the config file.
struct CustomPalette { std::wstring name; int n = 1; float stops[3][3] = {}; };

// Web console design tokens (app/src/styles.css @theme) — the deck, menus,
// panel and HUD chrome must match the web app exactly.
static const float UI_ULTRA_R = 0.200f, UI_ULTRA_G = 0.275f, UI_ULTRA_B = 0.788f; // #3346C9
static const float UI_SOFT_R  = 0.420f, UI_SOFT_G  = 0.478f, UI_SOFT_B  = 0.871f; // #6B7ADE
static const float UI_SCOPE_R = 0.043f, UI_SCOPE_G = 0.051f, UI_SCOPE_B = 0.047f; // #0B0D0C

static int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Appends each display's bounding rect (defined after App, used by DisplayRects).
static BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC dc, LPRECT rc, LPARAM lp);

// ONE audio engine for the whole process: every window (primary + companion
// displays) renders from the same tap — one loopback capture, one FFT, one
// beat state, analyzed once per loop. This is the desktop's structural win
// over the web app's BroadcastChannel + PCM streaming.
static AudioTap g_tap;

// Companion-window bookkeeping (defined near wWinMain).
static bool g_companionsOpen = false;
static void OpenCompanions(HINSTANCE inst);
static void CloseCompanions();

// Selftest drives the app through transient states (engines, Spout on/off);
// none of that may leak into the user's wavescope.ini.
static bool g_noPersist = false;

struct App {
    HWND hwnd = nullptr;
    ID2D1Factory* d2d = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteTextFormat* hudFont = nullptr;
    IDWriteTextFormat* deckFont = nullptr;   // centered, for deck buttons
    IDWriteTextFormat* tinyFont = nullptr;   // small, right-aligned group descriptors
    bool isCompanion = false;   // extra-display window: closable on its own
    HINSTANCE inst = nullptr;
    ProjectMHost pmHost;
    bool pmActive = false;
    AudioTap::TapCursor pmCursor;   // read position into the tap for the PCM feed
    Recorder rec;
    SpoutOut spout;                          // O: shared-texture output (OBS)
    std::vector<uint8_t> recScratch;         // GL readback for recorder/Spout
    IWICBitmap* recBmp = nullptr;            // built-in engine records through a
    ID2D1RenderTarget* recRT = nullptr;      // WIC (CPU) offscreen: same draws,
    ID2D1SolidColorBrush* recBrush = nullptr;// lockable pixels
    ID2D1Bitmap* recPresent = nullptr;       // cached upload for presenting it
    UINT recW = 0, recH = 0;
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
    VizState viz;           // per-window mode state (pools, history, peaks)
    BlitCache blitCache;    // cached CPU-blit bitmap, tied to `offscreen`
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
    // Paged dropdown state: a picker with more columns than MENU_MAXCOLS (only
    // the 551-preset MILK library today) turns into a fixed-size page with a
    // ‹ N / M › pager and a jump-to-page field. Small menus ignore all of this.
    int palOffset = 0;
    int menuPage = 0;                  // current page of the open dropdown
    bool pageFocused = false;          // jump-to-page field has keyboard focus
    std::wstring pageInput;            // digits typed into the jump-to-page field
    PresetFavorites presetFavorites;
    HWND presetDialog = nullptr;
    std::vector<std::wstring> browserPaths;
    std::wstring browserLibrary;
    unsigned browserRevision = 0;
    void OpenPresetBrowser();
    void RefreshPresetBrowser();
    void PresetBrowserCommand(int id, int code);
    bool RouteDialogMessage(MSG* msg) {
        return presetDialog && IsDialogMessageW(presetDialog, msg);
    }
    std::wstring FavoriteKey(const std::wstring& path) const {
        return WsPresetKey(path, PmExeDir(), WsDataDir());
    }
    bool IsPresetFavorite(const std::wstring& path) const {
        return presetFavorites.keys.count(FavoriteKey(path)) != 0;
    }
    int RandomPresetOther() const {
        std::vector<int> pool;
        for (int i = 0; i < pmHost.PresetCount(); ++i)
            if (i != pmHost.PresetIndex() && (!presetFavorites.shuffleOnly || IsPresetFavorite(pmHost.presetPaths[i])))
                pool.push_back(i);
        return pool.empty() ? -1 : pool[rand() % pool.size()];
    }
    int modeIdx = 0;
    int paletteIdx = 0;
    int resIdx = 0;          // index into RES_OPTS
    bool calm = false;
    int shuffleIdx = 0;      // index into SHUFFLE_SECS
    float lastShuffle = 0;
    // Auto-VJ (SHUFFLE "VJ"): audio-driven switching. Bass feeds a fast and a
    // slow envelope (seeded at arm); a fast spike over the slow floor is a drop
    // (hard cut), a long lull is a gentle morph. State advances on a 150 ms
    // cadence so the web's constants port regardless of frame rate.
    float vjSlow = 0, vjFast = 0;
    bool vjSeeded = false;
    float vjLastSwitch = 0;   // Now() baseline for the held-time checks
    float vjLastTick = 0;     // Now() of the last 150 ms envelope step
    // Morph deck (projectM engine): blend length (persisted) and target. The
    // target is a preset name, empty = shuffle (a random other preset); a name
    // is resolved against the live library each morph, so a gone/renamed target
    // just falls back to shuffle. Length seeds from the classic 2.7 s blend and
    // quantizes to 0.5 s as the slider is dragged.
    float morphSec = 2.7f;
    std::wstring morphTargetName;   // empty = shuffle
    bool morphDragging = false;     // slider thumb is being dragged
    // Preset lab (projectM engine): a modal editor over the active preset's
    // full .milk source. Apply hard-cuts the edit; a rejected edit keeps the
    // running preset up and shows the engine's message. Save As writes the
    // text into presets\user\ and points the picker at it.
    bool labPanel = false;
    bool labIgnoreOpenChar = false;
    std::wstring labText;           // editor buffer (\n-separated source)
    size_t labCaret = 0;            // caret position (index into labText)
    int labScroll = 0;              // first visible editor line
    std::wstring labSeedName;       // preset name the editor was seeded from
    std::wstring labStatus;         // one-line status under the editor
    bool labStatusErr = false;      // status is an error (drawn red)
    std::wstring labSaveName;       // Save As filename field
    enum { LF_NONE, LF_EDITOR, LF_NAME };
    int labFocus = LF_NONE;         // which lab field holds keyboard focus
    bool hud = true;         // master enable (H); when on, auto-fades on idle
    float hudActivity = -3;  // last mouse-move time; launch visibility is separate
    float startupUiUntil = -1; // -1: awaiting first frame; 0: dismissed/expired
    bool help = false;       // F1 / ? full keymap overlay
    bool deckVisible = false;// auto-hiding bottom control bar
    int deckHover = -1;      // button under the cursor, or -1
    float deckActivity = 0;  // last time the deck was touched (for auto-hide)
    bool fullscreen = false;
    bool alwaysOnTop = false;
    WINDOWPLACEMENT savedPlacement = { sizeof(WINDOWPLACEMENT) };
    int savedEngine = 0;         // persisted engine (0 wave, 1 gpu, 2 projm, 3 milk)
    bool savedSpout = false;     // persisted Spout-output state
    RECT restoreRect = {};       // persisted window placement
    bool haveRestore = false, restoreMax = false;
    LARGE_INTEGER qpcFreq = {}, qpcStart = {};
    int fpsFrames = 0;
    float fpsWindowStart = 0;
    int fps = 0;

    float Now() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return (float)((double)(t.QuadPart - qpcStart.QuadPart) / (double)qpcFreq.QuadPart);
    }

    bool StartupUiVisible() const { return startupUiUntil > 0; }
    bool DeckShown() const { return deckVisible || deckMenu >= 0 || StartupUiVisible(); }

    void UpdateUiVisibility(float now) {
        // Start on the first rendered frame, so engine loading doesn't consume
        // the discovery window. Keyboard dismissal also works before that frame.
        if (startupUiUntil < 0) startupUiUntil = now + 15.0f;
        if (now >= startupUiUntil) startupUiUntil = 0;
        // Keep ordinary mouse activity independent of the launch override.
        // An open dropdown remains usable after the discovery window ends.
        if (deckVisible && deckMenu < 0 && now - deckActivity > 3.0f) {
            deckVisible = false; deckHover = -1;
        }
    }

    bool CreateGraphics() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       (IUnknown**)&dwrite))) return false;
        // Sizes mirror the web console: 12px meter text for HUD + buttons,
        // 10px tracked-uppercase readout labels for the group descriptors.
        dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 12.0f, L"en-us", &hudFont);
        dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 12.0f, L"en-us", &deckFont);
        if (deckFont) {
            deckFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            deckFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            deckFont->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);  // clip, never wrap
        }
        // Tiny dim group descriptors (SRC / MODE / TRACE / SHUFFLE / RES),
        // right-aligned so they tuck against the group they label.
        dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                 10.0f, L"en-us", &tinyFont);
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
        float sc = UiScale();
        std::wstring sig = std::to_wstring((int)(sc * 100)) + L"|" +
            std::to_wstring(olW) + L"|" + std::to_wstring(olH) + L"|" +
            std::to_wstring(DeckShown()) + L"|" + std::to_wstring(deckHover) + L"|" +
            std::to_wstring(StartupUiVisible()) + L"|" +
            std::to_wstring(help) + L"|" + std::to_wstring(deckMenu) + L"|" +
            std::to_wstring((int)(HudFade() * 12)) + L"|" +   // fade steps → re-render as HUD fades
            std::to_wstring(DeckActive(DK_SYS)) + std::to_wstring(DeckActive(DK_MIC)) +
            std::to_wstring(DeckActive(DK_DEMO)) + std::to_wstring(DeckActive(DK_FILE)) +
            // Selector labels + rec state also live on deck buttons: cycling
            // them by key (P/S/R) must dirty the overlay even when l1 is quiet.
            L"|" + std::to_wstring(paletteIdx) + L"." + std::to_wstring(shuffleIdx) +
            L"." + std::to_wstring(resIdx) + L"." + std::to_wstring(rec.Active()) +
            // Morph deck: length + target ride on deck buttons, so dragging the
            // slider or repicking the target must re-render the overlay.
            L"|M" + std::to_wstring((int)(morphSec * 10)) + morphTargetName +
            L"|" + (l1 ? l1 : L"") + L"|" + (l2 ? l2 : L"");
        if (palPanel) {   // capture everything the panel draws so it stays live
            sig += L"|P" + std::to_wstring(shufflePalettes) + std::to_wstring(nameFocused) +
                   std::to_wstring(paletteIdx) + L"|" + newName + L"|";
            for (char v : palInclude) sig += (v ? L'1' : L'0');
            for (int k = 0; k < 3; k++) for (int c = 0; c < 3; c++)
                sig += L"." + std::to_wstring((int)(newStops[k][c] * 255));
        }
        if (deckMenu >= 0)   // paged dropdown: page + jump-field state
            sig += L"|M" + std::to_wstring(menuPage) + std::to_wstring(pageFocused) + pageInput;
        sig += L"|L" + std::to_wstring(labPanel);
        if (labPanel) sig += labText + L"|" + std::to_wstring(labCaret) + L"|" +
            std::to_wstring(labScroll) + L"|" + std::to_wstring(labFocus) + L"|" +
            labSaveName + L"|" + labStatus + std::to_wstring(labStatusErr);
        sig += L"|SP" + std::to_wstring(g_spotify.StatusVisible()) + g_spotify.Status() + std::to_wstring(g_spotify.busy.load()) + L"|" + std::to_wstring(palOffset);
        if (olSig == sig) return false; // unchanged — reuse the cached texture
        olSig = sig;

        olRT->BeginDraw();
        olRT->Clear(D2D1::ColorF(0, 0.0f));   // transparent
        olRT->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc));
        D2DPainter p(olRT, olBrush, hudFont, deckFont, tinyFont, sc);
        DrawUI(p, l1, l2);
        olRT->SetTransform(D2D1::Matrix3x2F::Identity());
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
        blitCache.Release();   // its bitmap was created on `offscreen`
        SafeRelease(&offBrush);
        SafeRelease(&offscreen);
        offW = offH = 0;
    }

    void ReleaseRecOffscreen() {
        SafeRelease(&recPresent);
        SafeRelease(&recBrush);
        SafeRelease(&recRT);
        SafeRelease(&recBmp);
        recW = recH = 0;
    }

    // Recording renders at the offscreen size clamped to 4K: H.264 encoders
    // top out near 4096 px a side, so an 8K target would only ever fail (and
    // queue ~1 GB of staging frames). On-screen look is unchanged — the
    // recording surface scales to the window exactly like the offscreen.
    void RecTargetSize(UINT& w, UINT& h) const {
        w = offW; h = offH;
        if (h > 2160) { w = (UINT)((UINT64)w * 2160 / h); h = 2160; }
        if (w > 3840) { h = (UINT)((UINT64)h * 3840 / w); w = 3840; }
        if (w < 2) w = 2; if (h < 2) h = 2;
    }

    // The CPU-side offscreen the built-in engine renders through while
    // recording, sized like the normal offscreen (clamped for the encoder).
    bool EnsureRecOffscreen() {
        if (!wic || !offscreen) return false;
        UINT tw, th; RecTargetSize(tw, th);
        if (recRT && recW == tw && recH == th) return true;
        ReleaseRecOffscreen();
        if (FAILED(wic->CreateBitmap(tw, th, GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapCacheOnLoad, &recBmp))) return false;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(d2d->CreateWicBitmapRenderTarget(recBmp, props, &recRT))) {
            ReleaseRecOffscreen(); return false;
        }
        recRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &recBrush);
        recW = tw; recH = th;
        return true;
    }

    // Cached window-target bitmap the recorded frame presents through.
    bool EnsureRecPresent() {
        if (recPresent) {
            D2D1_SIZE_U s = recPresent->GetPixelSize();
            if (s.width == recW && s.height == recH) return true;
            SafeRelease(&recPresent);
        }
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        return SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(recW, recH), props, &recPresent));
    }

    // R: start/stop the MP4 clip. Records the active engine's render target
    // at its current size (offscreen resolution for built-ins, client pixels
    // for the GL engines) plus the analysed audio.
    void ToggleRecord() {
        if (rec.Active()) {
            rec.Stop();
            if (!spout.on) ReleaseRecOffscreen();   // Spout may still need it
            return;
        }
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        if (!pmActive && !shaderActive) {
            EnsureOffscreen(rc.right, rc.bottom);
            UINT tw, th; RecTargetSize(tw, th);
            w = (int)tw; h = (int)th;
        }
        if (!rec.Start(w, h, g_tap.SampleRate(), &g_tap))
            SetWindowTextW(hwnd, L"WaveScope  (recording failed to start)");
    }

    // O: publish the visual as a Spout shared texture (OBS ingests it as a
    // "Spout2 Capture" source). One sender per process, owned by the
    // primary window.
    void ToggleSpout() {
        if (isCompanion) return;
        if (spout.on) {
            spout.Stop();
            if (!rec.Active()) ReleaseRecOffscreen();
            SaveConfig();
            return;
        }
        if (!spout.Start())
            SetWindowTextW(hwnd, L"WaveScope  (Spout output failed to start)");
        else
            SaveConfig();
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
        SafeRelease(&recPresent); // created on the old rt
        SafeRelease(&rt);
        ReleaseOffscreen(); // tied to the old rt via CreateCompatibleRenderTarget
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
        // Pin the target at 96 DPI: the process is per-monitor-v2 aware and
        // does all layout in raw pixels; 1 DIP must always equal 1 pixel.
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), 96.0f, 96.0f);
        HRESULT hr = d2d->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &rt);
        if (FAILED(hr)) return false;
        rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        return true;
    }

    // Monitor scale factor: the UI is laid out in 96-DPI logical units and
    // drawn under a scale transform (text scales as vectors, stays crisp).
    float UiScale() const {
        UINT dpi = GetDpiForWindow(hwnd);
        return dpi ? (float)dpi / 96.0f : 1.0f;
    }

    // Single-window path (selftest): analyze here, then render.
    void Render() {
        AudioFrame frame;
        g_tap.Analyze(frame, Now());
        RenderFrame(frame);
    }

    // Render one frame from the shared analysis (by value: calm mode tames
    // the beat per-window without touching the other windows' copy).
    void RenderFrame(AudioFrame frame) {
        if (!rt) return;
        D2D1_SIZE_F size = rt->GetSize();
        float now = Now();
        UpdateUiVisibility(now);

        // Shuffle timer: hop to a random other visual (or preset) on interval.
        // DJ (negative interval) hops the engine as well.
        int shufSecs = SHUFFLE_SECS[shuffleIdx];
        if (shufSecs == VJ_SEC) {
            VjTick(frame, now);   // audio-driven; no timer
        } else {
            int interval = shufSecs < 0 ? -shufSecs : shufSecs;
            if (interval > 0 && now - lastShuffle >= (float)interval) {
                lastShuffle = now;
                if (shufSecs < 0) DjHop();                    // DJ: hard engine hops
                else if (pmActive) MorphShuffle();            // soft cut at the slider length
                else if (shaderActive) shaderIdx = (shaderIdx + 1) % shaderHost.ShaderCount();
                else modeIdx = PickRandomMode();
                if (shufflePalettes) paletteIdx = PickRandomIncludedPalette();
            }
        }

        if (calm) frame.beat *= 0.2f; // photosensitivity: tame the pulse

        RECT crc; GetClientRect(hwnd, &crc);

        // projectM engine: render OpenGL on the child window, then composite
        // the D2D-rendered UI overlay (crisp, identical to the built-in view)
        // as a texture on top.
        // Extra HUD lines shared by every engine (contract readout: SRC/MODE/
        // RES/FPS/SHUFFLE/REC/now-playing).
        // Truncation returns -1; naive `off +=` would wrap the offset and the
        // next append would write through a wild pointer. Append defensively.
        wchar_t extra[220] = L"";
        {
            size_t off = 0;
            auto append = [&](const wchar_t* fmt, auto... args) {
                if (off + 1 >= 220) return;
                int r = _snwprintf_s(extra + off, 220 - off, _TRUNCATE, fmt, args...);
                if (r > 0) off += (size_t)r; else off = 219; // truncated: buffer is full
            };
            if (shuffleIdx > 0) append(L"\nSHUF %s", SHUF_LABELS[shuffleIdx]);
            std::wstring np = g_spotify.NowPlaying();
            if (!np.empty()) append(L"\n\x266A %s", np.c_str());
            if (rec.Active()) {
                int s = (int)rec.Seconds();
                append(L"\n\x25CF REC %02d:%02d", s / 60, s % 60);
            }
            if (spout.on) append(L"\nSPOUT OUT");
        }

        if (pmActive && pmHost.ready) {
            // _TRUNCATE everywhere a preset/file name flows into a HUD buffer:
            // swprintf_s would abort the process if a long name didn't fit.
            // RES shows the real backing pixels (web parity); the GL engines
            // render at window size, whatever the deck's RES target says.
            wchar_t l1[512];
            _snwprintf_s(l1, _TRUNCATE, L"SRC %s\n%s %s\nRES %ldX%ld\nFPS %d%s",
                       g_tap.SourceName(), milkMode ? L"MILK" : L"PROJM", pmHost.PresetName(),
                       crc.right, crc.bottom, fps, extra);
            bool dirty = BuildOverlay(crc.right, crc.bottom, l1, nullptr);
            float pcm[4800];   // up to 100 ms of new samples per frame
            int newN = g_tap.ReadNew(pmCursor, pcm, 4800);
            pmHost.RenderEngine(pcm, newN);
            if (pmHost.lost == 0) {
                if (rec.Active() || spout.on) {   // pre-UI frame, read once
                    int rw = 0, rh = 0;
                    if (pmHost.ReadBack(recScratch, rw, rh)) {
                        if (rec.Active()) rec.PushVideo(recScratch.data(), rw, rh);
                        spout.Send(recScratch.data(), rw, rh, true);
                    }
                }
                pmHost.BeginOverlay();
                pmHost.DrawOverlayTex(olPixels.empty() ? nullptr : olPixels.data(), (int)olW, (int)olH, dirty);
                pmHost.Present();
            } else if (pmHost.lost > 120) {
                RebuildEngineHosts();   // GPU reset: contexts are gone for good
            }
            fpsFrames++;
            if (now - fpsWindowStart >= 1.0f) { fps = fpsFrames; fpsFrames = 0; fpsWindowStart = now; }
            return;
        }

        // GPU shader engine: a fullscreen fragment shader, same UI over it.
        if (shaderActive && shaderHost.ready) {
            wchar_t l1[512];
            _snwprintf_s(l1, _TRUNCATE, L"SRC %s\nGPU %s\nRES %ldX%ld\nFPS %d%s",
                       g_tap.SourceName(), shaderHost.ShaderName(shaderIdx),
                       crc.right, crc.bottom, fps, extra);
            bool dirty = BuildOverlay(crc.right, crc.bottom, l1, nullptr);
            shaderHost.RenderEngine(shaderIdx, frame);
            if (shaderHost.lost == 0) {
                if (rec.Active() || spout.on) {   // pre-UI frame, read once
                    int rw = 0, rh = 0;
                    if (shaderHost.ReadBack(recScratch, rw, rh)) {
                        if (rec.Active()) rec.PushVideo(recScratch.data(), rw, rh);
                        spout.Send(recScratch.data(), rw, rh, true);
                    }
                }
                shaderHost.BeginOverlay();
                shaderHost.DrawOverlayTex(olPixels.empty() ? nullptr : olPixels.data(), (int)olW, (int)olH, dirty);
                shaderHost.Present();
            } else if (shaderHost.lost > 120) {
                RebuildEngineHosts();
            }
            fpsFrames++;
            if (now - fpsWindowStart >= 1.0f) { fps = fpsFrames; fpsFrames = 0; fpsWindowStart = now; }
            return;
        }

        const Mode& mode = MODES[modeIdx];

        RECT rc; GetClientRect(hwnd, &rc);
        EnsureOffscreen(rc.right, rc.bottom);

        bool recording = (rec.Active() || spout.on) && EnsureRecOffscreen();
        if (recording) {
            // While recording (or streaming to Spout), the modes draw into a
            // WIC (CPU) offscreen of the same size: identical calls, retained
            // trails, lockable pixels. One draw per frame — mode state
            // advances exactly once.
            recRT->BeginDraw();
            recBrush->SetColor(D2D1::ColorF(0.043f, 0.051f, 0.047f, mode.fade));
            recRT->FillRectangle(D2D1::RectF(0, 0, (float)recW, (float)recH), recBrush);
            DrawCtx g{ recRT, d2d, recBrush, PaletteFn(), (float)recW, (float)recH,
                       &frame, &viz, nullptr };
            mode.draw(g);
            recRT->EndDraw();
            WICRect wrc = { 0, 0, (INT)recW, (INT)recH };
            IWICBitmapLock* lock = nullptr;
            if (SUCCEEDED(recBmp->Lock(&wrc, WICBitmapLockRead, &lock))) {
                UINT cb = 0; BYTE* data = nullptr;
                lock->GetDataPointer(&cb, &data);
                if (rec.Active()) rec.PushVideo(data, (int)recW, (int)recH, true); // top-down
                spout.Send(data, (int)recW, (int)recH, false);
                lock->Release();
            }
        } else {
            // Draw the visualizer into the offscreen target at the chosen
            // resolution. It retains its content between frames, so the fade
            // wash produces phosphor trails.
            offscreen->BeginDraw();
            offBrush->SetColor(D2D1::ColorF(0.043f, 0.051f, 0.047f, mode.fade));
            offscreen->FillRectangle(D2D1::RectF(0, 0, (float)offW, (float)offH), offBrush);
            DrawCtx g{ offscreen, d2d, offBrush, PaletteFn(), (float)offW, (float)offH,
                       &frame, &viz, &blitCache };
            mode.draw(g);
            offscreen->EndDraw();
        }

        // Present: blit the frame scaled to the window, then the HUD.
        rt->BeginDraw();
        if (recording) {
            if (EnsureRecPresent()) {
                WICRect wrc = { 0, 0, (INT)recW, (INT)recH };
                IWICBitmapLock* lock = nullptr;
                if (SUCCEEDED(recBmp->Lock(&wrc, WICBitmapLockRead, &lock))) {
                    UINT cb = 0; BYTE* data = nullptr;
                    lock->GetDataPointer(&cb, &data);
                    D2D1_RECT_U all = { 0, 0, recW, recH };
                    recPresent->CopyFromMemory(&all, data, recW * 4);
                    lock->Release();
                }
                rt->DrawBitmap(recPresent, D2D1::RectF(0, 0, size.width, size.height), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        } else {
            ID2D1Bitmap* bmp = nullptr;
            offscreen->GetBitmap(&bmp);
            if (bmp) {
                rt->DrawBitmap(bmp, D2D1::RectF(0, 0, size.width, size.height), 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                bmp->Release();
            }
        }

        // RES = the pixels the modes actually draw (offscreen target, or the
        // clamped recording surface while recording) — web parity.
        UINT rw = recording ? recW : offW, rh = recording ? recH : offH;
        wchar_t l1[512];
        _snwprintf_s(l1, _TRUNCATE, L"SRC %s\nMODE %s  (%02d/%d)\nRES %uX%u\nFPS %d%s%s",
                   g_tap.SourceName(), mode.name, modeIdx + 1, MODE_COUNT,
                   rw, rh, fps, calm ? L"   CALM" : L"", extra);
        float sc = UiScale();
        rt->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc));
        D2DPainter dp(rt, brush, hudFont, deckFont, tinyFont, sc);
        DrawUI(dp, l1, nullptr);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());

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
           DK_LAB,                                        // preset lab button (projectM only)
           DK_MORPHT, DK_MORPH,                          // morph deck (projectM engine only)
           DK_CALM, DK_PALS, DK_DISP, DK_REC, DK_FULL, DK_OPTIONS, DK_HELP, // actions (right cluster)
           DK_N };
    // The morph group carries a slider between its two buttons (target ▾ and
    // the morph button); this is the width reserved for the track + value
    // label in that gap. Only the morph group widens its inter-button gap.
    static constexpr float MORPH_TRACK_W = 84, MORPH_VALUE_W = 34, MORPH_EXTRA = MORPH_TRACK_W + 6 + MORPH_VALUE_W;
    // The HUD has no button (web parity): H toggles it and it auto-fades when the
    // mouse is idle. The ? overlay documents the H shortcut.

    // Each logical group ends here — used for row-break points AND (when the
    // next group carries one) the tiny descriptor label before it.
    static bool DeckGroupEnd(int i) {
        return i == DK_SPOT || i == DK_NEXT || i == DK_MILK ||
               i == DK_TRACE || i == DK_SHUF || i == DK_RES ||
               i == DK_LAB || i == DK_MORPH || i == DK_HELP;
    }
    // Buttons that don't apply to the active engine (web parity: trace
    // palettes, calm and the palette panel are built-in-engine concerns; the
    // C/P/N keys still work everywhere).
    bool DeckHidden(int i) {
        bool alt = pmActive || shaderActive;
        if (i == DK_MORPHT || i == DK_MORPH || i == DK_LAB) return !pmActive;   // projectM-only
        return alt && (i == DK_TRACE || i == DK_CALM || i == DK_PALS);
    }
    // Tiny dim descriptor drawn before a group's first button (web-style).
    // Empty groups (engines, actions, HUD) carry no label.
    const wchar_t* DeckDescriptor(int firstBtn) {
        switch (firstBtn) {
        case DK_SYS:   return L"SRC";
        case DK_PREV:  return pmActive ? L"PRESET" : L"MODE";
        case DK_TRACE: return L"TRACE";
        case DK_SHUF:  return L"SHUFFLE";
        case DK_RES:   return L"RES";
        case DK_MORPHT: return L"MORPH";
        default:       return L"";
        }
    }
    // Width reserved for a descriptor slot (0 when the group has none).
    float DescWidth(int firstBtn) {
        const wchar_t* d = DeckDescriptor(firstBtn);
        return *d ? (float)wcslen(d) * 6.0f + 12.0f : 0.0f;
    }

    // The morph target for the deck button: "shuffle" (a random other preset)
    // or the target preset name, truncated so a long classic name doesn't blow
    // out the deck button width (the dropdown shows the full list).
    std::wstring MorphTargetLabel() const {
        if (morphTargetName.empty()) return L"shuffle";
        std::wstring s = morphTargetName;
        const size_t maxc = 18;
        if (s.size() > maxc) { s.resize(maxc - 1); s += L"\x2026"; }
        return s;
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
        case DK_LAB:    return L"lab (L)";
        case DK_MORPHT: return MorphTargetLabel() + CH;
        case DK_MORPH:  return L"morph";
        case DK_CALM:  return L"calm (C)"; case DK_PALS: return L"palettes + shuffle";
        case DK_DISP:  return std::wstring(L"displays") + CH;
        case DK_REC:   return rec.Active() ? L"stop rec (R)" : L"rec (R)";
        case DK_FULL:  return L"fullscreen (F)";
        case DK_OPTIONS: return L"options (F2)";
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
        // Web deck metrics: 6px gaps inside a group, 20px between groups,
        // 12px between rows, 24px side margins, 12px bar padding, ~10px
        // button side padding at 12px monospace.
        const float margin = 24, rowH = 28, gap = 6, groupGap = 20, rowGap = 12, bottom = 12;
        const float charW = 6.6f, padX = 20, minBtn = 24;
        float x0 = margin, x1 = W - margin, avail = x1 - x0;

        // Hidden buttons (engine-dependent, web parity) take no space at all;
        // they land on a dead rect no hit-test can reach.
        const D2D1_RECT_F dead = D2D1::RectF(-9, -9, -9, -9);
        float nat[DK_N];
        for (int i = 0; i < DK_N; i++) {
            float w = (float)DeckLabel(i).length() * charW + padX;
            // Bound long preset names so the picker cannot push controls offscreen.
            if (i == DK_PICK && w > avail - DescWidth(DK_PREV) - 2 * (minBtn + gap))
                w = avail - DescWidth(DK_PREV) - 2 * (minBtn + gap);
            nat[i] = DeckHidden(i) ? 0.0f : (w < minBtn ? minBtn : w);
        }

        // Left-cluster groups (SRC…MORPH); the right cluster is DK_CALM..DK_HELP.
        int gStart[10], gEnd[10], gCount = 0; float gNat[10];
        for (int i = 0, s = 0; i <= DK_MORPH; i++) {
            if (DeckGroupEnd(i)) {
                gStart[gCount] = s; gEnd[gCount] = i;
                float sum = 0; int vis = 0;
                for (int k = s; k <= i; k++) if (!DeckHidden(k)) { sum += nat[k]; vis++; }
                if (vis) sum += DescWidth(s) + gap * (vis - 1);
                if (s == DK_MORPHT && vis) sum += MORPH_EXTRA;   // slider + value-label gap
                gNat[gCount++] = sum; s = i + 1;
            }
        }
        float rNat = 0; int rVis = 0;
        for (int i = DK_CALM; i <= DK_HELP; i++)
            if (!DeckHidden(i)) { rNat += nat[i]; rVis++; }
        if (rVis > 1) rNat += gap * (rVis - 1);

        // Wrap the left cluster into rows, left-justified.
        int rowOfG[10]; float rowEnd[10]; for (float& e : rowEnd) e = x0;
        int r = 0; float used = 0;
        for (int g = 0; g < gCount; g++) {
            if (gNat[g] <= 0) { rowOfG[g] = r; continue; }   // fully hidden group
            float add = (used > 0 ? groupGap : 0) + gNat[g];
            if (used > 0 && used + add > avail) { r++; used = 0; add = gNat[g]; }
            rowOfG[g] = r; used += add; rowEnd[r] = x0 + used;
        }
        int leftRows = r + 1;

        // Right cluster: same row as the last left row if it fits, else a new row.
        int lastRow = leftRows - 1;
        bool rightNewRow = (rowEnd[lastRow] + groupGap + rNat > x1);
        int rightRows = 1; float rightUsed = 0;
        for (int i = DK_CALM; i <= DK_HELP; i++) if (!DeckHidden(i)) {
            float add = nat[i] + (rightUsed > 0 ? gap : 0);
            if (rightUsed > 0 && rightUsed + add > avail) { rightRows++; rightUsed = 0; add = nat[i]; }
            rightUsed += add;
        }
        int totalRows = leftRows + (rightNewRow ? rightRows : 0);
        float totalH = totalRows * rowH + (totalRows - 1) * rowGap;
        float yTop = H - bottom - totalH;

        // Place the left cluster.
        float bxRow[10]; bool firstInRow[10];
        for (int i = 0; i < 10; i++) { bxRow[i] = x0; firstInRow[i] = true; }
        for (int g = 0; g < gCount; g++) {
            if (gNat[g] <= 0) {
                for (int i = gStart[g]; i <= gEnd[g]; i++) out[i] = dead;
                continue;
            }
            int rr = rowOfG[g]; float y = yTop + rr * (rowH + rowGap);
            float bx = bxRow[rr];
            if (!firstInRow[rr]) bx += groupGap;
            firstInRow[rr] = false;
            bx += DescWidth(gStart[g]);   // reserve the descriptor slot
            for (int i = gStart[g]; i <= gEnd[g]; i++) {
                if (DeckHidden(i)) { out[i] = dead; continue; }
                out[i] = D2D1::RectF(bx, y, bx + nat[i], y + rowH);
                bx += nat[i] + (i < gEnd[g] ? gap : 0);
                if (i == DK_MORPHT) bx += MORPH_EXTRA;   // reserve the slider + value-label gap
            }
            bxRow[rr] = bx;
        }
        // Flow action buttons across additional rows when the window is narrow.
        int rr = rightNewRow ? leftRows : lastRow;
        float bx = rightNewRow && rightRows > 1 ? x0 : x1 - rNat;
        for (int i = DK_CALM; i <= DK_HELP; i++) {
            if (DeckHidden(i)) { out[i] = dead; continue; }
            if (bx > x0 && bx + nat[i] > x1 + 0.01f) { rr++; bx = x0; }
            float y = yTop + rr * (rowH + rowGap);
            out[i] = D2D1::RectF(bx, y, bx + nat[i], y + rowH);
            bx += nat[i] + gap;
        }
        // The full-width bar rect (web: bg-scope/85 with a white/10 top
        // hairline, hugging the bottom edge).
        return D2D1::RectF(0, yTop - bottom, W, H);
    }

    // The morph slider's track + value-label rects live in the reserved gap
    // between the two morph buttons (see DeckLayout / MORPH_EXTRA). Valid only
    // while the morph group is visible (projectM engine); callers guard first.
    void MorphSliderRects(const D2D1_RECT_F b[DK_N], D2D1_RECT_F& track, D2D1_RECT_F& label) const {
        const float gap = 6;
        float tl = b[DK_MORPHT].right + gap, tr = tl + MORPH_TRACK_W;
        track = D2D1::RectF(tl, b[DK_MORPHT].top, tr, b[DK_MORPHT].bottom);
        label = D2D1::RectF(tr + 6, b[DK_MORPHT].top, tr + 6 + MORPH_VALUE_W, b[DK_MORPHT].bottom);
    }
    // Map an x within the track to a 0.5–10 s length, quantized to 0.5 s (web
    // slider step). Out-of-track x clamps to the ends.
    float MorphSecFromX(float mx, const D2D1_RECT_F& track) const {
        float w = track.right - track.left;
        float frac = w > 0 ? (mx - track.left) / w : 0.0f;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        float sec = 0.5f + frac * (10.0f - 0.5f);
        sec = (float)((int)(sec / 0.5f + 0.5f)) * 0.5f;   // snap to 0.5 s
        if (sec < 0.5f) sec = 0.5f; if (sec > 10.0f) sec = 10.0f;
        return sec;
    }

    // Hit tests take LOGICAL coords (WndProc divides mouse pixels by UiScale).
    int DeckHitTest(int mx, int my) {
        if (!rt) return -1;
        float sc = UiScale();
        D2D1_SIZE_F size = rt->GetSize();
        D2D1_RECT_F b[DK_N];
        DeckLayout(size.width / sc, size.height / sc, b);
        for (int i = 0; i < DK_N; i++)
            if (mx >= b[i].left && mx <= b[i].right && my >= b[i].top && my <= b[i].bottom)
                return i;
        return -1;
    }

    // Is a button currently the active selection (for highlight)?
    bool DeckActive(int i) {
        switch (i) {
        case DK_SYS:   return g_tap.Source() == AudioTap::SRC_LOOPBACK;
        case DK_MIC:   return g_tap.Source() == AudioTap::SRC_MIC;
        case DK_DEMO:  return g_tap.Source() == AudioTap::SRC_DEMO;
        case DK_FILE:  return g_tap.Source() == AudioTap::SRC_FILE;
        case DK_SPOT:  return g_spotify.busy.load();
        case DK_WAVE:  return !pmActive && !shaderActive;
        case DK_GPU:   return shaderActive;
        case DK_PROJM: return pmActive && !milkMode;
        case DK_MILK:  return pmActive && milkMode;
        case DK_LAB:   return labPanel;
        case DK_CALM:  return calm;
        case DK_PALS:  return palPanel;
        case DK_REC:   return rec.Active();
        case DK_FULL:  return fullscreen;
        case DK_HELP:  return help;
        case DK_PICK: case DK_TRACE: case DK_SHUF: case DK_RES: case DK_DISP: case DK_MORPHT:
            return deckMenu == i;   // highlight whichever dropdown is open
        default:       return false;
        }
    }

    void DeckDispatch(int i) {
        int prev = deckMenu; deckMenu = -1;   // any click closes an open dropdown by default
        switch (i) {
        case DK_SYS:   g_tap.SetSource(AudioTap::SRC_LOOPBACK); break;
        case DK_MIC:   g_tap.SetSource(AudioTap::SRC_MIC); break;
        case DK_DEMO:  g_tap.SetSource(AudioTap::SRC_DEMO); break;
        case DK_FILE:  OpenFile(); break;
        case DK_SPOT:  SpotifyButton(); break;
        case DK_PREV:  CyclePick(-1); break;
        case DK_NEXT:  CyclePick(1); break;
        case DK_WAVE:  if (pmActive) ToggleProjectM(); if (shaderActive) ToggleShader(); OpenMenu(DK_PICK); break;
        case DK_GPU:   if (pmActive) ToggleProjectM(); if (!shaderActive) ToggleShader(); OpenMenu(DK_PICK); break;
        case DK_PROJM: EnterProjectM(false); OpenMenu(DK_PICK); break;
        case DK_MILK:
            // Enter MILK on first click; re-clicking while already in MILK opens
            // the folder picker to point the library at your own .milk archive
            // (this was the old L shortcut, now the lab).
            if (pmActive && milkMode) OpenPresetFolder();
            else { EnterProjectM(true); OpenMenu(DK_PICK); }
            break;
        case DK_PICK: case DK_TRACE: case DK_SHUF: case DK_RES: case DK_DISP: case DK_MORPHT:
                       if (prev == i) deckMenu = -1; else OpenMenu(i); break;   // toggle this dropdown
        case DK_LAB:   OpenLab(); break;
        case DK_MORPH: MorphNow(); break;
        case DK_CALM:  SetCalm(!calm); break;
        case DK_PALS:  OpenPalettePanel(); break;
        case DK_REC:   ToggleRecord(); break;
        case DK_FULL:  ToggleFullscreen(); break;
        case DK_OPTIONS: OpenOptions(); break;
        case DK_HELP:  help = !help; break;
        }
    }

    void OpenOptions() {
        DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_OPTIONS), hwnd, OptionsProc, 0);
        hudActivity = Now(); olSig.clear();
    }
    void SpotifyButton() {
        if (!SpValidClientId(g_spotify.clientId)) OpenOptions();
        else { g_spotify.OneButton(); hudActivity = Now(); }
    }

    // Auto-DJ hops between available engines and visuals.
    void DjHop() {
        switch (rand() % 3) {
        case 1:   // GPU shaders
            if (pmActive) ToggleProjectM();
            if (!shaderActive) ToggleShader();
            if (shaderActive && shaderHost.ShaderCount() > 0) {
                shaderIdx = rand() % shaderHost.ShaderCount();
                return;
            }
            break;
        case 2:   // projectM
            if (shaderActive) ToggleShader();
            if (!pmActive) ToggleProjectM();
            if (pmActive && pmHost.PresetCount() > 0) {
                int pick = RandomPresetOther();
                if (pick >= 0) pmHost.LoadPreset(pick, false);
                return;
            }
            break;
        }
        if (pmActive) ToggleProjectM();
        if (shaderActive) ToggleShader();
        modeIdx = PickRandomMode();
    }

    // Cycle the whole engine (WaveScope -> GPU -> projectM), E key.
    void CycleEngine() {
        if (!pmActive && !shaderActive) ToggleShader();
        else if (shaderActive) { ToggleShader(); ToggleProjectM(); }
        else ToggleProjectM();   // back to built-in
    }

    // Cycle the active engine's selection (mode / shader / preset) with ‹ ›.
    void CyclePick(int dir) {
        if (pmActive) pmHost.NextPreset(dir);
        else if (shaderActive) { int n = shaderHost.ShaderCount(); if (n) shaderIdx = (shaderIdx + dir + n) % n; }
        else CycleMode(dir);
    }

    // Morph to a random OTHER preset over the slider length. This is the
    // shuffle target's action, shared by the morph button and the timed
    // shuffle so both blend identically instead of hard-cutting.
    void MorphShuffle() {
        if (!pmActive || !pmHost.ready) return;
        int t = RandomPresetOther();
        if (t < 0) return;
        pmHost.MorphToPreset(t, morphSec);
    }

    // The morph button: cross-fade to the target over the slider length — or a
    // random other preset while the target is shuffle. A named target that is
    // current or no longer present is a no-op (web parity).
    void MorphNow() {
        if (!pmActive || !pmHost.ready) return;
        if (morphTargetName.empty()) { MorphShuffle(); return; }
        int t = pmHost.PresetIndexByName(morphTargetName.c_str());
        if (t < 0 || t == pmHost.PresetIndex()) return;
        pmHost.MorphToPreset(t, morphSec);
    }

    // MILK's library folder: the user's picked archive, else the bundled
    // classic MilkDrop pack (presets-classic\, 551 originals) when present.
    std::wstring MilkLibrary() const {
        if (!libraryFolder.empty()) return libraryFolder;
        std::wstring def = PmExeDir() + L"presets-classic";
        return GetFileAttributesW(def.c_str()) != INVALID_FILE_ATTRIBUTES ? def : L"";
    }

    // Enter the projectM engine as PROJM (the bundled WaveScope presets) or
    // MILK (the classic library / the user's own folder). Switching flavors
    // re-points the scan both ways.
    void EnterProjectM(bool library) {
        if (shaderActive) ToggleShader();
        if (!pmActive) ToggleProjectM();
        if (!pmActive || !pmHost.ready) return;
        milkMode = library;
        std::wstring want = library ? MilkLibrary() : PmExeDir() + L"presets";
        if (!want.empty() && pmHost.scanDir != want)
            pmHost.SetPresetFolder(want.c_str());
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
        case DK_DISP:  return DisplayCount() + 1;   // + ALL DISPLAYS toggle
        case DK_MORPHT: return 1 + (pmActive ? pmHost.PresetCount() : 0);   // shuffle + presets
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
        case DK_MORPHT: return i == 0 ? L"shuffle" : pmHost.PresetNameAt(i - 1);
        case DK_DISP:
            if (i >= DisplayCount())
                return g_companionsOpen ? L"CLOSE EXTRA DISPLAYS" : L"ALL DISPLAYS";
            swprintf_s(buf, L"DISPLAY %d", i + 1); return buf;
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
        case DK_MORPHT: {
            if (morphTargetName.empty()) return 0;   // shuffle
            int t = pmActive ? pmHost.PresetIndexByName(morphTargetName.c_str()) : -1;
            return t >= 0 ? t + 1 : 0;   // a gone target reads as shuffle
        }
        }
        return -1;
    }
    bool MenuDisabled(int a, int i) {
        return a == DK_PICK && !pmActive && !shaderActive && calm && MODES[i].flash;
    }
    void MenuSelect(int a, int i) {
        switch (a) {
        case DK_PICK:
            // Hard cut: the soft cross-fade white-flashes (projectm_host.inl).
            if (pmActive) pmHost.LoadPreset(i, false);
            else if (shaderActive) shaderIdx = i;
            else modeIdx = i;
            break;
        case DK_TRACE: paletteIdx = i; break;
        case DK_SHUF:  SetShuffle(i); break;
        case DK_RES:   resIdx = i; break;
        case DK_MORPHT:
            if (i == 0) morphTargetName.clear();               // shuffle
            else morphTargetName = pmHost.PresetNameAt(i - 1);
            break;
        case DK_DISP:
            if (i >= DisplayCount()) {
                if (g_companionsOpen) CloseCompanions(); else OpenCompanions(inst);
            } else {
                MoveToMonitor(i);
            }
            break;
        }
    }
    // The paged dropdown never grows past this many columns; beyond it, the
    // list pages instead of running off the screen. Rows cap too, so a page
    // holds at most 4 x 15 = 60 entries — denser reads as a wall of text.
    static const int MENU_MAXCOLS = 4;
    static const int MENU_MAXROWS = 15;
    // Footer controls for a paged dropdown (empty rects when not paged).
    struct MenuChrome { bool paged = false; int pages = 1; D2D1_RECT_F prev{}, next{}, field{}; };

    // Column/row grid for anchor `a`'s dropdown. perCol is derived from the
    // height above the deck (as before); cols is capped at MENU_MAXCOLS, so a
    // list longer than one page (pageSize = cols*perCol) pages rather than
    // adding columns sideways. Small lists come out identical to before.
    void MenuMetrics(int a, float W, float H, int& cols, int& perCol, int& pageSize, int& pages) {
        int n = MenuCount(a);
        D2D1_RECT_F b[DK_N]; DeckLayout(W, H, b);
        const float rh = 24, pad = 5, topLimit = 8;
        float bottom = b[a].top - 8;
        perCol = (int)((bottom - topLimit - 2 * pad) / rh);
        if (perCol > MENU_MAXROWS) perCol = MENU_MAXROWS;
        if (perCol < 1) perCol = 1;
        if (perCol > n && n > 0) perCol = n;
        int natural = (n + perCol - 1) / perCol;
        cols = natural < MENU_MAXCOLS ? natural : MENU_MAXCOLS;
        if (cols < 1) cols = 1;
        pageSize = cols * perCol;
        pages = pageSize > 0 ? (n + pageSize - 1) / pageSize : 1;
        if (pages < 1) pages = 1;
    }

    // Open dropdown `a`, starting on the page that holds its current selection
    // and clearing any prior jump-field state.
    void OpenMenu(int a) {
        if (a == DK_PICK && pmActive) { OpenPresetBrowser(); return; }
        deckMenu = a; pageFocused = false; pageInput.clear(); menuPage = 0;
        if (!rt) return;
        float sc = UiScale(); D2D1_SIZE_F sz = rt->GetSize();
        int cols, perCol, pageSize, pages;
        MenuMetrics(a, sz.width / sc, sz.height / sc, cols, perCol, pageSize, pages);
        int cur = MenuCurrent(a);
        if (cur > 0 && pageSize > 0) menuPage = cur / pageSize;
        if (menuPage >= pages) menuPage = pages - 1;
        if (menuPage < 0) menuPage = 0;
    }

    // Lay the open dropdown out above its anchor button. Rows is sized to the
    // full list; entries not on the current page get a dead rect (off-screen)
    // so draw/click can key off the real index without any offset arithmetic.
    void MenuGeom(int a, float W, float H, D2D1_RECT_F& bg,
                  std::vector<D2D1_RECT_F>& rows, MenuChrome& ch) {
        rows.clear(); ch = MenuChrome{};
        int n = MenuCount(a);
        if (n <= 0 || DeckHidden(a)) { bg = D2D1::RectF(0, 0, 0, 0); return; }
        int cols, perCol, pageSize, pages;
        MenuMetrics(a, W, H, cols, perCol, pageSize, pages);
        ch.paged = pages > 1; ch.pages = pages;
        if (menuPage >= pages) menuPage = pages - 1;   // list may have shrunk
        if (menuPage < 0) menuPage = 0;
        D2D1_RECT_F b[DK_N]; DeckLayout(W, H, b);
        const float rw = 192, rh = 24, pad = 5, colGap = 4, topLimit = 8; // web select max-w-48
        const float footerH = ch.paged ? 30.0f : 0.0f;
        float bottom = b[a].top - 8;
        float menuW = cols * rw + (cols - 1) * colGap + 2 * pad;
        float menuH = perCol * rh + 2 * pad + footerH;
        float x = b[a].left;
        if (x + menuW > W - 8) x = W - 8 - menuW;
        if (x < 8) x = 8;
        float top = bottom - menuH;
        if (top < topLimit) top = topLimit;
        bg = D2D1::RectF(x, top, x + menuW, top + menuH);
        rows.assign(n, D2D1::RectF(-9, -9, -9, -9));   // off-page = dead rect
        int start = menuPage * pageSize, end = start + pageSize;
        if (end > n) end = n;
        for (int i = start; i < end; i++) {
            int k = i - start, c = k / perCol, r = k % perCol;
            float rx = x + pad + c * (rw + colGap);
            float ry = top + pad + r * rh;
            rows[i] = D2D1::RectF(rx, ry, rx + rw, ry + rh - 2);
        }
        if (ch.paged) {
            float fy = top + perCol * rh + 2 * pad + 4, fh = 22;
            ch.prev  = D2D1::RectF(x + pad, fy, x + pad + 26, fy + fh);
            ch.field = D2D1::RectF(x + pad + 34, fy, x + pad + 34 + 92, fy + fh);
            ch.next  = D2D1::RectF(x + menuW - pad - 26, fy, x + menuW - pad, fy + fh);
        }
    }
    // Columns are a fixed 192px; the row font is monospace (Consolas 12), so a
    // name past ~26 chars is clipped to one line with an ellipsis rather than
    // wrapping into the row below.
    std::wstring MenuFit(const wchar_t* s) {
        std::wstring w = s;
        const size_t maxc = 26;
        if (w.size() > maxc) { w.resize(maxc - 1); w += L"\x2026"; }
        return w;
    }
    void DrawMenu(UIPainter& p) {
        if (deckMenu < 0) return;
        D2D1_RECT_F bg; std::vector<D2D1_RECT_F> rows; MenuChrome ch;
        MenuGeom(deckMenu, p.W(), p.H(), bg, rows, ch);
        if (rows.empty()) return;
        // Dropdown card: scope ground with a white/15 hairline (web chrome).
        p.Rect(bg.left, bg.top, bg.right - bg.left, bg.bottom - bg.top,
               UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.97f);
        p.Stroke(bg.left, bg.top, bg.right - bg.left, bg.bottom - bg.top, 1, 1, 1, 0.15f, 1.0f);
        int cur = MenuCurrent(deckMenu);
        for (int i = 0; i < (int)rows.size(); i++) {
            if (rows[i].left < 0) continue;   // not on the current page
            bool active = (i == cur), off = MenuDisabled(deckMenu, i);
            float rw2 = rows[i].right - rows[i].left, rh2 = rows[i].bottom - rows[i].top;
            std::wstring label = MenuFit(MenuItem(deckMenu, i));
            if (active) {
                p.Rect(rows[i].left, rows[i].top, rw2, rh2, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 0.22f);
                p.Stroke(rows[i].left, rows[i].top, rw2, rh2, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 0.9f, 1.0f);
                p.Text(rows[i].left + 10, rows[i].top + 3, rw2 - 14, rh2 - 3,
                       label.c_str(), UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 1.0f);
            } else {
                p.Text(rows[i].left + 10, rows[i].top + 3, rw2 - 14, rh2 - 3,
                       label.c_str(), 1, 1, 1, off ? 0.35f : 0.80f);
            }
        }
        if (ch.paged) DrawPager(p, ch);
    }
    // The ‹ N / M › footer under a paged dropdown: prev/next buttons reuse the
    // deck's chevron glyphs; the box mirrors the palette name-field styling and
    // shows the current page (with a caret while it's being typed into).
    void DrawPager(UIPainter& p, const MenuChrome& ch) {
        auto btn = [&](const D2D1_RECT_F& r, const wchar_t* glyph) {
            float w = r.right - r.left, h = r.bottom - r.top;
            p.Stroke(r.left, r.top, w, h, 1, 1, 1, 0.30f, 1.0f);
            p.Text(r.left, r.top, w, h, glyph, 1, 1, 1, 0.85f, true);
        };
        btn(ch.prev, L"\x2039");
        btn(ch.next, L"\x203A");
        float fw = ch.field.right - ch.field.left, fh = ch.field.bottom - ch.field.top;
        p.Rect(ch.field.left, ch.field.top, fw, fh, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.96f);
        p.Stroke(ch.field.left, ch.field.top, fw, fh, 1, 1, 1, pageFocused ? 0.40f : 0.15f, 1.0f);
        std::wstring cur = pageFocused ? (pageInput + L"_") : std::to_wstring(menuPage + 1);
        wchar_t buf[40]; swprintf_s(buf, L"%s / %d", cur.c_str(), ch.pages);
        p.Text(ch.field.left + 8, ch.field.top + 3, fw - 12, fh - 3, buf, 1, 1, 1, 0.85f);
    }
    bool MenuClick(int mx, int my) {
        if (deckMenu < 0) return false;
        float sc = UiScale();
        D2D1_SIZE_F sz = rt->GetSize();
        D2D1_RECT_F bg; std::vector<D2D1_RECT_F> rows; MenuChrome ch;
        MenuGeom(deckMenu, sz.width / sc, sz.height / sc, bg, rows, ch);
        if (ch.paged) {   // pager controls take clicks before the rows
            if (In(ch.prev, mx, my))  { if (menuPage > 0) menuPage--; pageFocused = false; return true; }
            if (In(ch.next, mx, my))  { if (menuPage < ch.pages - 1) menuPage++; pageFocused = false; return true; }
            if (In(ch.field, mx, my)) { pageFocused = true; pageInput.clear(); return true; }
        }
        for (int i = 0; i < (int)rows.size(); i++) {
            if (rows[i].left < 0) continue;   // not on the current page
            if (In(rows[i], mx, my)) {
                if (MenuDisabled(deckMenu, i)) return true;   // disabled: swallow, don't select
                MenuSelect(deckMenu, i); deckMenu = -1; pageFocused = false; return true;
            }
        }
        if (In(bg, mx, my)) return true;   // swallow clicks inside the menu chrome
        deckMenu = -1; pageFocused = false;   // click elsewhere closes it
        return false;                         // ...and falls through to the deck
    }

    // Mouse near the bottom edge reveals the deck; hovering keeps it alive.
    // px/py arrive in physical pixels; the UI lives in logical units.
    void OnMouseMove(int pxx, int pxy) {
        float sc = UiScale();
        int mx = (int)(pxx / sc), my = (int)(pxy / sc);
        hudActivity = Now();   // any movement wakes the HUD (then it fades on idle)
        // Dragging the morph slider tracks the thumb; self-heal a drag whose
        // button was released off-window (no WM_LBUTTONUP would have arrived).
        if (morphDragging) {
            if (!(GetKeyState(VK_LBUTTON) & 0x8000)) { morphDragging = false; SaveConfig(); }
            else {
                if (pmActive && rt && !DeckHidden(DK_MORPHT)) {
                    D2D1_SIZE_F sz = rt->GetSize();
                    D2D1_RECT_F b[DK_N]; DeckLayout(sz.width / sc, sz.height / sc, b);
                    D2D1_RECT_F track, label; MorphSliderRects(b, track, label);
                    morphSec = MorphSecFromX((float)mx, track);
                }
                deckVisible = true; deckActivity = Now();
                return;
            }
        }
        if (palPanel || labPanel || help) return;  // panels are modal
        RECT rc; GetClientRect(hwnd, &rc);
        int hit = DeckHitTest(mx, my);
        if (my > (int)(rc.bottom / sc) - 175 || hit >= 0 || deckMenu >= 0) { deckVisible = true; deckActivity = Now(); }
        deckHover = DeckShown() ? hit : -1;
    }

    void OnClick(int pxx, int pxy) {
        if (help) { help = false; return; }
        float sc = UiScale();
        int mx = (int)(pxx / sc), my = (int)(pxy / sc);
        if (LabClick(mx, my)) return;
        if (PanelClick(mx, my)) return;
        if (MenuClick(mx, my)) { deckActivity = Now(); return; }
        if (!DeckShown()) return;
        // Morph slider: press-and-drag to set the blend length. It sits in the
        // gap between the morph buttons, which DeckHitTest reports as no button.
        if (pmActive && rt && !DeckHidden(DK_MORPHT)) {
            D2D1_SIZE_F sz = rt->GetSize();
            D2D1_RECT_F b[DK_N]; DeckLayout(sz.width / sc, sz.height / sc, b);
            D2D1_RECT_F track, label; MorphSliderRects(b, track, label);
            if (mx >= track.left - 4 && mx <= track.right + 4 &&
                my >= track.top && my <= track.bottom) {
                morphDragging = true;
                SetCapture(hwnd);
                morphSec = MorphSecFromX((float)mx, track);
                deckActivity = Now();
                return;
            }
        }
        int hit = DeckHitTest(mx, my);
        if (hit >= 0) { DeckDispatch(hit); deckActivity = Now(); }
    }

    // HUD opacity: full during launch or mouse activity, fading after ~2s idle;
    // 0 when the user has toggled it off with H. No button — the ? overlay
    // documents the shortcut (web parity).
    float HudFade() {
        if (!hud) return 0.0f;
        if (StartupUiVisible()) return 1.0f;
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
            // Top-left readout stack, one scope/70 chip per line with
            // white/70 text — the web HUD's exact recipe.
            std::wstring all = l1;
            float y = 16;
            size_t pos = 0;
            while (pos <= all.size()) {
                size_t nl = all.find(L'\n', pos);
                std::wstring line = all.substr(pos, nl == std::wstring::npos ? nl : nl - pos);
                if (!line.empty()) {
                    float chipW = (float)line.length() * 6.6f + 16.0f;
                    p.Rect(16, y, chipW, 20, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.70f * hf);
                    p.Text(24, y + 3, chipW, 16, line.c_str(), 1, 1, 1, 0.70f * hf);
                    y += 24;
                }
                if (nl == std::wstring::npos) break;
                pos = nl + 1;
            }
        }
        std::wstring spStatus = g_spotify.Status();
        if (!spStatus.empty() && g_spotify.StatusVisible()) {
            p.Rect(16, p.H() - 240, p.W() - 32, 48, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.95f);
            p.Text(24, p.H() - 234, p.W() - 48, 40, spStatus.c_str(), 1, 1, 1, 0.9f);
        }
        if (labPanel) { DrawLabPanel(p); return; }       // modal: hides deck
        if (palPanel) { DrawPalettePanel(p); return; }  // modal: hides deck
        if (StartupUiVisible() && !help && deckMenu < 0) {
            D2D1_RECT_F buttons[DK_N];
            D2D1_RECT_F bar = DeckLayout(p.W(), p.H(), buttons);
            float w = std::min(640.0f, p.W() - 32), x = (p.W() - w) * 0.5f;
            float y = bar.top - 78;
            p.Rect(x, y, w, 66, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.95f);
            p.Text(x + 14, y + 9, w - 28, 50,
                L"Move the mouse to the bottom edge to bring back the controls.\n"
                L"F1  Help     F2  Options     H  Toggle status display\n"
                L"Auto-hides after 15 seconds or your first keypress.",
                1, 1, 1, 0.95f);
        }
        if (DeckShown()) DrawDeckP(p);
        if (DeckShown()) DrawMenu(p);         // open dropdown sits above its anchor
        if (help) DrawHelpP(p);
    }

    void DrawDeckP(UIPainter& p) {
        D2D1_RECT_F b[DK_N];
        D2D1_RECT_F bar = DeckLayout(p.W(), p.H(), b);
        // Full-width bar, web-style: bg-scope/85 under a white/10 hairline.
        p.Rect(bar.left, bar.top, bar.right - bar.left, bar.bottom - bar.top,
               UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.85f);
        p.Rect(bar.left, bar.top, bar.right - bar.left, 1.0f, 1, 1, 1, 0.10f);
        for (int i = 0; i < DK_N; i++) {
            if (DeckHidden(i)) continue;
            bool active = DeckActive(i), hover = (i == deckHover);
            float w = b[i].right - b[i].left, h = b[i].bottom - b[i].top;
            // Web button recipe: hairline border only (white/15, white/40 on
            // hover), text white/60 -> white; active = ultra border, ultra/20
            // fill, ultra-soft text. No fills otherwise, no rounding.
            if (active) {
                p.Rect(b[i].left, b[i].top, w, h, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 0.20f);
                p.Stroke(b[i].left, b[i].top, w, h, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 1.0f, 1.0f);
            } else {
                p.Stroke(b[i].left, b[i].top, w, h, 1, 1, 1, hover ? 0.40f : 0.15f, 1.0f);
            }
            if (active)
                p.Text(b[i].left, b[i].top, w, h, DeckLabel(i).c_str(),
                       UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 1.0f, true);
            else
                p.Text(b[i].left, b[i].top, w, h, DeckLabel(i).c_str(),
                       1, 1, 1, hover ? 1.0f : 0.60f, true);
            // Tiny readout descriptor in the reserved slot (web: white/40).
            const wchar_t* d = DeckDescriptor(i);
            if (*d) {
                float dw = DescWidth(i);
                p.Text(b[i].left - dw, b[i].top, dw - 6, h, d, 1, 1, 1, 0.40f, true, true);
            }
        }
        // Morph slider: rail + thumb in the reserved gap between the two morph
        // buttons, value label beside it — the deck's one slider, styled to sit
        // with the button chrome (soft-blue thumb, white/25 rail).
        if (pmActive && !DeckHidden(DK_MORPHT)) {
            D2D1_RECT_F track, label; MorphSliderRects(b, track, label);
            float tw = track.right - track.left, th = track.bottom - track.top;
            float cy = track.top + th * 0.5f;
            p.Rect(track.left, cy - 1.5f, tw, 3, 1, 1, 1, 0.25f);                    // rail
            float frac = (morphSec - 0.5f) / (10.0f - 0.5f);
            if (frac < 0) frac = 0; if (frac > 1) frac = 1;
            float tx = track.left + frac * tw;
            p.Rect(tx - 3, track.top + 4, 6, th - 8, UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 0.95f);  // thumb
            wchar_t vb[16]; swprintf_s(vb, L"%.1fs", morphSec);
            p.Text(label.left, label.top, label.right - label.left, th, vb, 1, 1, 1, 0.55f, true);
        }
    }

    // Full keymap overlay (F1 / ?), a centered card over any engine.
    void DrawHelpP(UIPainter& p) {
        static const wchar_t* HELP =
            L"WAVESCOPE " WAVESCOPE_VERSION L"  \x2014  KEYS\n"
            L"\n"
            L"SPACE / \x2192      next mode (or preset / shader)\n"
            L"\x2190              previous\n"
            L"E              cycle engine (WaveScope / GPU / projectM)\n"
            L"G              GPU shader engine\n"
            L"P              cycle trace palette\n"
            L"N              palettes + shuffle panel (edit / include)\n"
            L"1 / 2 / 3      source: system / mic / demo\n"
            L"4              open an audio file\n"
            L"M              displays           F3   preset search / favorites\n"
            L"R              record clip -> wavescope-<time>.mp4 (H.264+AAC)\n"
            L"O              Spout output (OBS ingests \"WaveScope\")\n"
            L"B              Spotify: authorize + start playback (Premium)\n"
            L"S              shuffle timer (off / 15s / 30s / 1m / 5m / DJ / VJ)\n"
            L"C              calm mode (reduced motion and beat intensity)\n"
            L"L              preset lab (edit the active .milk)   RES \x25BE on deck\n"
            L"F              fullscreen        H   HUD (auto-fades on idle)\n"
            L"T              always on top     F1 / ?   help    F2   options\n"
            L"ESC            close panel / menu / fullscreen, then quit\n"
            L"Mouse          bottom edge reveals controls; movement wakes HUD\n"
            L"\n"
            L"Launch: WaveScope.exe [demo | file=PATH | milkdrop | shader | wall | kiosk]\n"
            L"        [install-startup | remove-startup]   \x2014   tray icon: right-click";
        float w = 620, h = 523;
        float x = (p.W() - w) * 0.5f, y = (p.H() - h) * 0.5f;
        if (x < 0) x = 0; if (y < 0) y = 0;
        p.Rect(x, y, w, h, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.95f);
        p.Stroke(x, y, w, h, 1, 1, 1, 0.15f, 1.0f);
        p.Text(x + 22, y + 18, w - 32, h - 24, HELP, 1, 1, 1, 0.85f);
    }

    // T / tray menu: keep the visualizer over every other window (the
    // desktop's answer to the web app's ?embed=1 overlay uses).
    void SetAlwaysOnTop(bool on) {
        alwaysOnTop = on;
        SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
        if (GetOpenFileNameW(&ofn)) g_tap.SetFile(file);
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
            if (SHGetPathFromIDListW(pidl, path)) {
                libraryFolder = path;   // persisted; MILK mode uses it
                milkMode = true;
                pmHost.SetPresetFolder(path);
                SaveConfig();
            }
            CoTaskMemFree(pidl);
        }
    }

    // After a GPU reset both GL contexts are permanently dead: tear the hosts
    // down and rebuild whichever engine is active (best-effort, once per
    // ~2 s of sustained failure at 60 fps).
    void RebuildEngineHosts() {
        bool wantPm = pmActive, wantShader = shaderActive;
        pmHost.Destroy();
        shaderHost.Destroy();
        RECT rc; GetClientRect(hwnd, &rc);
        if (wantPm && pmHost.Init(inst, hwnd)) {
            if (isCompanion) pmHost.SetVsync(false);
            pmHost.Resize(rc.right, rc.bottom);
            if (milkMode && !MilkLibrary().empty()) pmHost.SetPresetFolder(MilkLibrary().c_str());
            pmHost.Show(true);
        } else if (wantPm) {
            pmActive = false;
        }
        if (wantShader && shaderHost.Init(inst, hwnd)) {
            if (isCompanion) shaderHost.SetVsync(false);
            shaderHost.Resize(rc.right, rc.bottom);
            shaderHost.Show(true);
        } else if (wantShader) {
            shaderActive = false;
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
            if (isCompanion) pmHost.SetVsync(false);
            RECT rc; GetClientRect(hwnd, &rc);
            pmHost.Resize(rc.right, rc.bottom);
            pmHost.Show(true);
            pmActive = true;
            SetWindowTextW(hwnd, L"WaveScope  projectM (SPACE next preset, E cycles engine)");
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
            if (isCompanion) shaderHost.SetVsync(false);
            RECT rc; GetClientRect(hwnd, &rc);
            shaderHost.Resize(rc.right, rc.bottom);
            shaderHost.Show(true);
            shaderActive = true;
            SetWindowTextW(hwnd, L"WaveScope  GPU shader (SPACE next shader, E cycles engine)");
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

    void CycleShuffle() { shuffleIdx = (shuffleIdx + 1) % SHUFFLE_COUNT; lastShuffle = Now(); ArmVj(); }
    void SetShuffle(int i) { shuffleIdx = Clampi(i, 0, SHUFFLE_COUNT - 1); lastShuffle = Now(); ArmVj(); }

    // (Re)arm the VJ detector: re-seed the envelopes on the next tick and reset
    // the cadence. Cheap to call whenever the shuffle option changes.
    void ArmVj() { vjSeeded = false; vjLastTick = 0; }

    // Auto-VJ tick (web parity). Bass feeds a fast (~0.5 s) and a slow (~10 s)
    // envelope; a fast spike over the slow floor after the 8 s hold is a drop
    // (hard cut); 45 s with no drop is a gentle 5 s morph. Runs on a 150 ms
    // cadence, so the ported constants match the web at any frame rate.
    void VjTick(const AudioFrame& frame, float now) {
        if (now - vjLastTick < 0.15f) return;
        vjLastTick = now;
        float bass = frame.bass;
        if (!vjSeeded) { vjSlow = vjFast = bass; vjSeeded = true; vjLastSwitch = now; return; }
        vjSlow = vjSlow * 0.985f + bass * 0.015f;
        vjFast = vjFast * 0.6f + bass * 0.4f;
        float held = now - vjLastSwitch;
        if (held < 8.0f) return;                           // 8 s minimum hold
        if (vjSlow > 0.04f && vjFast > vjSlow * 1.5f && vjFast > 0.18f) {
            VjHop(0.0f); vjLastSwitch = now; return;       // drop: hard cut
        }
        if (held > 45.0f) { VjHop(5.0f); vjLastSwitch = now; }   // lull: gentle morph
    }
    // Switch the active engine's visual for a VJ hop. blendSec 0 = hard cut
    // (drop); 5 = a gentle morph (only projectM blends; others just switch).
    void VjHop(float blendSec) {
        if (pmActive && pmHost.ready) {
            int t = RandomPresetOther();
            if (t < 0) return;
            if (blendSec > 0) pmHost.MorphToPreset(t, (double)blendSec);
            else pmHost.LoadPreset(t, false);
        } else if (shaderActive) {
            int n = shaderHost.ShaderCount();
            if (n > 1) { int c = shaderIdx, t; do { t = rand() % n; } while (t == c); shaderIdx = t; }
        } else {
            modeIdx = PickRandomMode();
        }
    }

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
    void OpenPalettePanel() { palPanel = !palPanel; nameFocused = false; palOffset = 0; deckMenu = -1; EnsurePalInclude(); }

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
        if (customPalettes.size() >= 64) return;
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
        else if (c >= 32 && c != L'|' && newName.size() < 22) newName += c;
    }
    // Jump-to-page field: digits only, Enter jumps to that page (1-based,
    // clamped), Backspace edits, and both Enter and a committed jump un-focus.
    void PageChar(wchar_t c) {
        if (deckMenu < 0 || !pageFocused) return;
        if (c == 8) { if (!pageInput.empty()) pageInput.pop_back(); return; }
        if (c == 13) {
            if (!pageInput.empty() && rt) {
                float sc = UiScale(); D2D1_SIZE_F sz = rt->GetSize();
                int cols, perCol, pageSize, pages;
                MenuMetrics(deckMenu, sz.width / sc, sz.height / sc, cols, perCol, pageSize, pages);
                int pg = _wtoi(pageInput.c_str()) - 1;
                if (pg < 0) pg = 0;
                if (pg >= pages) pg = pages - 1;
                menuPage = pg;
            }
            pageFocused = false; pageInput.clear();
            return;
        }
        if (c >= L'0' && c <= L'9' && pageInput.size() < 3) pageInput += c;
    }

    struct PalRects {
        D2D1_RECT_F card, close, master, swatch[3], preview, nameBox, addBtn, prev, next;
        D2D1_RECT_F row[64];
        int rows = 0;
        float dividerY = 0, newHeadY = 0, newDescY = 0;
    };
    void PalLayout(float W, float H, PalRects& r) {
        EnsurePalInclude();
        int n = PaletteCount();
        const float pw = 462, pad = 28, rowH = 26;
        int visible = Clampi((int)((H - 24 - 374) / rowH), 1, 64);
        if (visible > n) visible = n;
        palOffset = Clampi(palOffset, 0, n - visible);
        r.rows = visible;
        float ph = 374 + visible * rowH;
        float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
        if (py < 12) py = 12;
        r.card = D2D1::RectF(px, py, px + pw, py + ph);
        float x = px + pad;
        r.close = D2D1::RectF(px + pw - pad - 44, py + 16, px + pw - pad, py + 38);
        r.master = D2D1::RectF(x, py + 64, x + 20, py + 84);
        float y = py + 132;
        for (int i = 0; i < visible; i++) { r.row[i] = D2D1::RectF(x, y, px + pw - pad, y + rowH - 4); y += rowH; }
        r.prev = D2D1::RectF(x, y + 2, x + 60, y + 26);
        r.next = D2D1::RectF(px + pw - pad - 60, y + 2, px + pw - pad, y + 26);
        y += 30;
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
        p.Rect(b.left, b.top, w, h, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.95f);
        p.Stroke(b.left, b.top, w, h, 1, 1, 1, 0.25f, 1.0f);
        if (on) p.Rect(b.left + 3, b.top + 3, w - 6, h - 6, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 1.0f);
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
               UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.97f);
        p.Stroke(r.card.left, r.card.top, r.card.right - r.card.left, r.card.bottom - r.card.top,
                 1, 1, 1, 0.15f, 1.0f);
        float cx = r.card.left + 28;
        p.Text(cx, r.card.top + 14, 340, 22, L"PALETTES + SHUFFLE",
               UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 1.0f);
        p.Text(r.close.left - 6, r.close.top, 60, 22, L"close", 0.72f, 0.74f, 0.76f, 0.9f);
        DrawCheck(p, r.master, shufflePalettes);
        p.Text(r.master.right + 12, r.master.top - 2, 340, 22, L"Shuffle palettes too", 0.92f, 0.93f, 0.94f, 0.96f);
        p.Text(cx, r.master.bottom + 4, 406, 40,
               L"Each shuffle tick also picks a random trace\npalette from the checked list below.",
               0.60f, 0.63f, 0.66f, 0.92f);
        for (int i = 0; i < r.rows; i++) {
            const D2D1_RECT_F& row = r.row[i];
            int idx = palOffset + i;
            DrawCheck(p, D2D1::RectF(row.left, row.top + 1, row.left + 18, row.top + 19), palInclude[idx] != 0);
            DrawGrad(p, row.left + 30, row.top + 2, 170, 17, PaletteFnFor(idx));
            bool active = (idx == paletteIdx);
            p.Text(row.left + 212, row.top - 1, 194, 22, PalNameFor(idx),
                   active ? UI_SOFT_R : 1.0f, active ? UI_SOFT_G : 1.0f,
                   active ? UI_SOFT_B : 1.0f, active ? 1.0f : 0.70f);
        }
        std::wstring range = std::to_wstring(palOffset + 1) + L" - " + std::to_wstring(palOffset + r.rows) + L" / " + std::to_wstring(PaletteCount());
        p.Text(r.prev.left, r.prev.top, 60, 24, L"< prev", 1, 1, 1, 0.8f, true);
        p.Text(r.next.left, r.next.top, 60, 24, L"next >", 1, 1, 1, 0.8f, true);
        p.Text(r.prev.right, r.prev.top, r.next.left - r.prev.right, 24, range.c_str(), 1, 1, 1, 0.6f, true);
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
               r.nameBox.bottom - r.nameBox.top, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.96f);
        p.Stroke(r.nameBox.left, r.nameBox.top, r.nameBox.right - r.nameBox.left,
                 r.nameBox.bottom - r.nameBox.top, 1, 1, 1, nameFocused ? 0.40f : 0.15f, 1.0f);
        bool placeholder = newName.empty() && !nameFocused;
        std::wstring shown = placeholder ? L"Palette name" : (newName + (nameFocused ? L"_" : L""));
        p.Text(r.nameBox.left + 10, r.nameBox.top + 6, 230, 22, shown.c_str(),
               1, 1, 1, placeholder ? 0.30f : 0.85f);
        // Web's add button: border-ultra, bg ultra/20, ultra-soft text.
        p.Rect(r.addBtn.left, r.addBtn.top, r.addBtn.right - r.addBtn.left,
               r.addBtn.bottom - r.addBtn.top, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 0.20f);
        p.Stroke(r.addBtn.left, r.addBtn.top, r.addBtn.right - r.addBtn.left,
                 r.addBtn.bottom - r.addBtn.top, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 1.0f, 1.0f);
        p.Text(r.addBtn.left, r.addBtn.top, r.addBtn.right - r.addBtn.left, r.addBtn.bottom - r.addBtn.top,
               L"add palette", UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 1.0f, true);
    }
    // Handle a click while the panel is open. Returns true if consumed.
    bool PanelClick(int mx, int my) {
        if (!palPanel) return false;
        float sc = UiScale();
        D2D1_SIZE_F sz = rt->GetSize();
        PalRects r; PalLayout(sz.width / sc, sz.height / sc, r);
        if (In(r.close, mx, my)) { palPanel = false; return true; }
        if (In(r.master, mx, my)) { shufflePalettes = !shufflePalettes; SaveConfig(); return true; }
        if (In(r.prev, mx, my)) { palOffset = Clampi(palOffset - r.rows, 0, PaletteCount() - 1); return true; }
        if (In(r.next, mx, my)) { palOffset = Clampi(palOffset + r.rows, 0, PaletteCount() - 1); return true; }
        for (int i = 0; i < r.rows; i++) if (In(r.row[i], mx, my)) {
            int idx = palOffset + i;
            if (mx <= r.row[i].left + 24) { palInclude[idx] = !palInclude[idx]; SaveConfig(); }
            else paletteIdx = idx;
            return true;
        }
        for (int k = 0; k < 3; k++) if (In(r.swatch[k], mx, my)) { PickStop(k); return true; }
        if (In(r.nameBox, mx, my)) { nameFocused = true; return true; }
        if (In(r.addBtn, mx, my)) { AddNewPalette(); return true; }
        nameFocused = false;
        if (!In(r.card, mx, my)) palPanel = false; // click outside closes
        return true; // swallow clicks while the panel is up
    }

    // ----- preset lab (modal editor over the active preset's .milk source) -----

    static constexpr float LAB_LINE_H = 16.0f;   // editor line height (Consolas 12)
    static constexpr float LAB_CHAR_W = 6.6f;    // monospace advance at that size

    static std::wstring Utf8ToWide(const std::string& s) {
        if (s.empty()) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
        return w;
    }
    static std::string WideToUtf8(const std::wstring& s) {
        if (s.empty()) return "";
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &u[0], n, nullptr, nullptr);
        return u;
    }

    struct LabRects { D2D1_RECT_F card, close, editor, status, apply, nameBox, save; };
    void LabLayout(float W, float H, LabRects& r) const {
        float pw = 680; if (pw > W - 16) pw = W - 16;
        float ph = H - 24; if (ph > 760) ph = 760;
        float px = (W - pw) * 0.5f, py = (H - ph) * 0.5f;
        if (px < 8) px = 8; if (py < 12) py = 12;
        r.card = D2D1::RectF(px, py, px + pw, py + ph);
        const float pad = 24;
        float x = px + pad, right = px + pw - pad, bottom = py + ph - pad;
        r.close   = D2D1::RectF(right - 52, py + 16, right, py + 40);
        float saveTop = bottom - 30;
        r.save    = D2D1::RectF(right - 90, saveTop, right, saveTop + 30);
        r.nameBox = D2D1::RectF(x, saveTop, r.save.left - 10, saveTop + 30);
        float applyTop = saveTop - 8 - 30;
        r.apply   = D2D1::RectF(x, applyTop, x + 90, applyTop + 30);
        float statusTop = applyTop - 6 - 22;
        r.status  = D2D1::RectF(x, statusTop, right, statusTop + 22);
        r.editor  = D2D1::RectF(x, py + 78, right, statusTop - 8);
    }

    // L / deck button: toggle the lab. Seeds from the active preset on open.
    void OpenLab() {
        if (!pmActive || !pmHost.ready) return;   // lab is a projectM feature
        deckMenu = -1; palPanel = false; help = false;
        labPanel = !labPanel;
        if (labPanel) SeedLab();
        else labFocus = LF_NONE;
    }
    void SeedLab() {
        labText = pmHost.CurrentPresetText();
        labSeedName = pmHost.PresetName();
        labCaret = 0; labScroll = 0;
        labStatus.clear(); labStatusErr = false;
        if (labText.empty()) {
            labStatus = L"The active preset file is unavailable. Choose another preset to edit.";
            labStatusErr = true;
        }
        labFocus = LF_EDITOR;
        std::wstring base = labSeedName;
        size_t dot = base.rfind(L".milk");
        if (dot != std::wstring::npos) base.resize(dot);
        labSaveName = base + L" edit";
    }

    int LabLineCount() const {
        int n = 1; for (wchar_t c : labText) if (c == L'\n') n++; return n;
    }
    void LabCaretLineCol(int& line, int& col) const {
        line = 0; col = 0;
        for (size_t i = 0; i < labCaret && i < labText.size(); i++) {
            if (labText[i] == L'\n') { line++; col = 0; } else col++;
        }
    }
    size_t LabLineStartIndex(int ln) const {
        if (ln <= 0) return 0;
        int cur = 0;
        for (size_t i = 0; i < labText.size(); i++)
            if (labText[i] == L'\n' && ++cur == ln) return i + 1;
        return labText.size();
    }
    int LabVisibleLines() const {
        if (!rt) return 20;
        float sc = UiScale(); D2D1_SIZE_F sz = rt->GetSize();
        LabRects r; LabLayout(sz.width / sc, sz.height / sc, r);
        int v = (int)((r.editor.bottom - r.editor.top - 8) / LAB_LINE_H);
        return v > 1 ? v : 1;
    }
    void LabEnsureVisible() {
        int line, col; LabCaretLineCol(line, col);
        int vis = LabVisibleLines();
        if (line < labScroll) labScroll = line;
        else if (line >= labScroll + vis) labScroll = line - vis + 1;
        if (labScroll < 0) labScroll = 0;
    }
    void LabInsert(const wchar_t* s) {
        labText.insert(labCaret, s); labCaret += wcslen(s);
        labStatus.clear(); labStatusErr = false;
        LabEnsureVisible();
    }
    void LabInsertChar(wchar_t c) { wchar_t s[2] = { c, 0 }; LabInsert(s); }
    void LabBackspace() { if (labCaret > 0) { labText.erase(labCaret - 1, 1); labCaret--; LabEnsureVisible(); } }
    void LabDelete()    { if (labCaret < labText.size()) labText.erase(labCaret, 1); }
    void LabMoveHoriz(int d) {
        if (d < 0 && labCaret > 0) labCaret--;
        else if (d > 0 && labCaret < labText.size()) labCaret++;
        LabEnsureVisible();
    }
    void LabMoveVert(int dir) {
        int line, col; LabCaretLineCol(line, col);
        int target = line + dir;
        if (target < 0 || target >= LabLineCount()) return;
        size_t s = LabLineStartIndex(target), e = s;
        while (e < labText.size() && labText[e] != L'\n') e++;
        size_t len = e - s;
        labCaret = s + ((size_t)col < len ? (size_t)col : len);
        LabEnsureVisible();
    }
    void LabMoveHome() { int l, c; LabCaretLineCol(l, c); labCaret = LabLineStartIndex(l); }
    void LabMoveEnd()  {
        int l, c; LabCaretLineCol(l, c);
        size_t e = LabLineStartIndex(l);
        while (e < labText.size() && labText[e] != L'\n') e++;
        labCaret = e;
    }

    // Apply the edited source as a hard cut. A rejected edit keeps the running
    // preset up and shows the engine's message (often with a line/col).
    void LabApply() {
        if (!pmActive || !pmHost.ready) { labStatus = L"the lab needs the projectM engine"; labStatusErr = true; return; }
        std::string u8 = WideToUtf8(labText);
        bool ok = pmHost.ApplyData(u8.c_str(), false);
        if (ok) { labStatus = L"applied"; labStatusErr = false; labSeedName = pmHost.PresetName(); }
        else {
            std::wstring msg = Utf8ToWide(pmHost.loadFailMsg);
            labStatus = msg.empty() ? L"load failed" : (L"load failed: " + msg);
            labStatusErr = true;
        }
    }
    // Save the edit into presets\user\ (its own library page) and point the
    // picker at it, so it shows immediately and survives restarts.
    void LabSaveAs() {
        if (labSaveName.empty()) { labStatus = L"name the preset first"; labStatusErr = true; return; }
        std::wstring userDir = WsDataDir() + L"presets\\user";
        std::wstring name;
        if (!WsPresetFileName(labSaveName, name)) {
            labStatus = L"use a valid Windows filename (not a reserved name or path)"; labStatusErr = true; return;
        }
        std::string u8 = WideToUtf8(labText);
        DWORD error = WsSaveNewPreset(userDir, name, u8);
        if (error) {
            labStatus = (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                ? L"a preset with that name already exists — choose a new name"
                : L"save failed — check the folder and available disk space";
            labStatusErr = true; return;
        }
        milkMode = true; libraryFolder = userDir;   // the user folder is now the MILK library
        pmHost.SetPresetFolder(userDir.c_str());
        int idx = pmHost.PresetIndexByName(name.c_str());
        if (idx >= 0) pmHost.LoadPreset(idx, false);
        labSeedName = name;
        labStatus = L"saved: " + userDir + L"\\" + name; labStatusErr = false;
        SaveConfig();
    }

    // Keyboard while the lab is open. Every key is consumed (the plan's rule:
    // shortcuts must not fire mid-edit). ESC steps name->editor->closed.
    void LabKeyDown(WPARAM vk) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (vk == VK_ESCAPE) {
            if (labFocus == LF_NAME) labFocus = LF_EDITOR;
            else { labPanel = false; labFocus = LF_NONE; }
            return;
        }
        if (labFocus == LF_NAME) {
            if (vk == VK_RETURN) LabSaveAs();
            else if (vk == VK_BACK) { if (!labSaveName.empty()) labSaveName.pop_back(); }
            return;   // printable name keys arrive via WM_CHAR
        }
        if (labFocus != LF_EDITOR) return;   // modal: swallow, do nothing
        switch (vk) {
        case VK_RETURN: if (ctrl) LabApply(); else LabInsertChar(L'\n'); break;
        case VK_BACK:   LabBackspace(); break;
        case VK_DELETE: LabDelete(); break;
        case VK_TAB:    LabInsert(L"  "); break;   // two-space soft tab
        case VK_LEFT:   LabMoveHoriz(-1); break;
        case VK_RIGHT:  LabMoveHoriz(1); break;
        case VK_UP:     LabMoveVert(-1); break;
        case VK_DOWN:   LabMoveVert(1); break;
        case VK_HOME:   LabMoveHome(); break;
        case VK_END:    LabMoveEnd(); break;
        case VK_PRIOR:  labScroll -= LabVisibleLines(); if (labScroll < 0) labScroll = 0; break;
        case VK_NEXT: { labScroll += LabVisibleLines(); int m = LabLineCount() - 1;
                        if (labScroll > m) labScroll = m; if (labScroll < 0) labScroll = 0; } break;
        }
    }
    void LabChar(wchar_t c) {
        if (labIgnoreOpenChar) { labIgnoreOpenChar = false; if (c == L'l' || c == L'L') return; }
        if (labFocus == LF_NAME) {
            if (c >= 32 && c != 127 && labSaveName.size() < 80) labSaveName += c;
        } else if (labFocus == LF_EDITOR) {
            if (c >= 32 && c != 127) LabInsertChar(c);   // control chars handled in keydown
        }
    }
    void LabScrollBy(int lines) {
        labScroll += lines;
        int m = LabLineCount() - 1;
        if (labScroll > m) labScroll = m;
        if (labScroll < 0) labScroll = 0;
    }
    void LabPlaceCaret(int mx, int my, const LabRects& r) {
        int line = labScroll + (int)((my - (r.editor.top + 4)) / LAB_LINE_H);
        if (line < 0) line = 0;
        int lc = LabLineCount(); if (line >= lc) line = lc - 1;
        int col = (int)((mx - (r.editor.left + 6)) / LAB_CHAR_W + 0.5f);
        if (col < 0) col = 0;
        size_t s = LabLineStartIndex(line), e = s;
        while (e < labText.size() && labText[e] != L'\n') e++;
        size_t len = e - s;
        labCaret = s + ((size_t)col < len ? (size_t)col : len);
    }
    bool LabClick(int mx, int my) {
        if (!labPanel || !rt) return false;
        float sc = UiScale(); D2D1_SIZE_F sz = rt->GetSize();
        LabRects r; LabLayout(sz.width / sc, sz.height / sc, r);
        if (In(r.close, mx, my))   { labPanel = false; labFocus = LF_NONE; return true; }
        if (In(r.apply, mx, my))   { LabApply(); labFocus = LF_EDITOR; return true; }
        if (In(r.save, mx, my))    { LabSaveAs(); return true; }
        if (In(r.nameBox, mx, my)) { labFocus = LF_NAME; return true; }
        if (In(r.editor, mx, my))  { labFocus = LF_EDITOR; LabPlaceCaret(mx, my, r); return true; }
        if (!In(r.card, mx, my))   { labPanel = false; labFocus = LF_NONE; }
        return true;   // modal: swallow every click
    }
    void DrawLabEditor(UIPainter& p, const LabRects& r) {
        float ex = r.editor.left + 6, ey = r.editor.top + 4;
        float ew = r.editor.right - r.editor.left - 12;
        int vis = (int)((r.editor.bottom - r.editor.top - 8) / LAB_LINE_H);
        int maxCols = (int)((ew - 4) / LAB_CHAR_W); if (maxCols < 1) maxCols = 1;
        int caretLine, caretCol; LabCaretLineCol(caretLine, caretCol);
        int line = 0; size_t lineStart = 0;
        for (size_t i = 0; i <= labText.size(); i++) {
            if (i == labText.size() || labText[i] == L'\n') {
                if (line >= labScroll && line < labScroll + vis) {
                    std::wstring text = labText.substr(lineStart, i - lineStart);
                    if ((int)text.size() > maxCols) text.resize(maxCols);   // clip, never wrap
                    if (!text.empty())
                        p.Text(ex, ey + (line - labScroll) * LAB_LINE_H, ew, LAB_LINE_H,
                               text.c_str(), 0.85f, 0.87f, 0.88f, 0.95f);
                }
                line++; lineStart = i + 1;
                if (i == labText.size()) break;
            }
        }
        if (labFocus == LF_EDITOR && caretLine >= labScroll && caretLine < labScroll + vis) {
            int cc = caretCol < maxCols ? caretCol : maxCols;
            p.Rect(ex + cc * LAB_CHAR_W, ey + (caretLine - labScroll) * LAB_LINE_H,
                   1.5f, LAB_LINE_H - 2, UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 0.95f);
        }
    }
    void DrawLabPanel(UIPainter& p) {
        LabRects r; LabLayout(p.W(), p.H(), r);
        p.Rect(0, 0, p.W(), p.H(), 0, 0, 0, 0.5f);   // scrim
        float cw = r.card.right - r.card.left, ch = r.card.bottom - r.card.top;
        p.Rect(r.card.left, r.card.top, cw, ch, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.97f);
        p.Stroke(r.card.left, r.card.top, cw, ch, 1, 1, 1, 0.15f, 1.0f);
        float cx = r.card.left + 24;
        p.Text(cx, r.card.top + 14, 300, 22, L"PRESET LAB", UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 1.0f);
        p.Text(cx, r.card.top + 40, cw - 120, 20, labSeedName.c_str(), 1, 1, 1, 0.40f);
        float clw = r.close.right - r.close.left, clh = r.close.bottom - r.close.top;
        p.Stroke(r.close.left, r.close.top, clw, clh, 1, 1, 1, 0.25f, 1.0f);
        p.Text(r.close.left, r.close.top, clw, clh, L"close", 1, 1, 1, 0.7f, true);
        float ew = r.editor.right - r.editor.left, eh = r.editor.bottom - r.editor.top;
        p.Rect(r.editor.left, r.editor.top, ew, eh, 0.02f, 0.03f, 0.03f, 0.96f);
        p.Stroke(r.editor.left, r.editor.top, ew, eh, 1, 1, 1, labFocus == LF_EDITOR ? 0.35f : 0.15f, 1.0f);
        DrawLabEditor(p, r);
        float sw = r.status.right - r.status.left;
        if (!labStatus.empty())
            p.Text(r.status.left, r.status.top, sw, 22, labStatus.c_str(),
                   labStatusErr ? 0.90f : UI_SOFT_R, labStatusErr ? 0.40f : UI_SOFT_G,
                   labStatusErr ? 0.42f : UI_SOFT_B, 1.0f);
        else
            p.Text(r.status.left, r.status.top, sw, 22, L"Ctrl+Enter applies \x2022 Esc closes",
                   1, 1, 1, 0.35f);
        float aw = r.apply.right - r.apply.left, ah = r.apply.bottom - r.apply.top;
        p.Rect(r.apply.left, r.apply.top, aw, ah, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 0.20f);
        p.Stroke(r.apply.left, r.apply.top, aw, ah, UI_ULTRA_R, UI_ULTRA_G, UI_ULTRA_B, 1.0f, 1.0f);
        p.Text(r.apply.left, r.apply.top, aw, ah, L"apply", UI_SOFT_R, UI_SOFT_G, UI_SOFT_B, 1.0f, true);
        float nw = r.nameBox.right - r.nameBox.left, nh = r.nameBox.bottom - r.nameBox.top;
        p.Rect(r.nameBox.left, r.nameBox.top, nw, nh, UI_SCOPE_R, UI_SCOPE_G, UI_SCOPE_B, 0.96f);
        p.Stroke(r.nameBox.left, r.nameBox.top, nw, nh, 1, 1, 1, labFocus == LF_NAME ? 0.40f : 0.15f, 1.0f);
        std::wstring shown = labSaveName + (labFocus == LF_NAME ? L"_" : L"");
        p.Text(r.nameBox.left + 8, r.nameBox.top + 5, nw - 16, 22, shown.c_str(), 1, 1, 1, 0.85f);
        float vw = r.save.right - r.save.left, vh = r.save.bottom - r.save.top;
        p.Stroke(r.save.left, r.save.top, vw, vh, 1, 1, 1, 0.25f, 1.0f);
        p.Text(r.save.left, r.save.top, vw, vh, L"save", 1, 1, 1, 0.7f, true);
    }

    // ----- settings persistence (installed data folder or portable exe folder) -----

    std::wstring ConfigPath() const { return WsDataDir() + L"wavescope.ini"; }

    void SaveConfig() {
        if (isCompanion || g_noPersist) return; // primary window only, never selftest
        std::wstring path = ConfigPath();
        AcquireSRWLockExclusive(&g_iniLock);   // vs. the Spotify worker's SaveToken
        auto wi = [&](const wchar_t* k, int v) {
            wchar_t b[16]; _itow_s(v, b, 10);
            WritePrivateProfileStringW(L"settings", k, b, path.c_str());
        };
        wi(L"mode", modeIdx); wi(L"palette", paletteIdx); wi(L"resolution", resIdx);
        wi(L"shuffle", shuffleIdx); wi(L"calm", calm ? 1 : 0); wi(L"shader", shaderIdx);
        wi(L"morphSec", (int)(morphSec * 10 + 0.5f));   // blend length in tenths of a second
        wi(L"shufflePal", shufflePalettes ? 1 : 0);
        wi(L"engine", pmActive ? (milkMode ? 3 : 2) : (shaderActive ? 1 : 0));
        wi(L"spout", spout.on ? 1 : 0);
        WritePrivateProfileStringW(L"settings", L"milkFolder", libraryFolder.c_str(), path.c_str());
        presetFavorites.Save(path);
        // Window placement: the pre-fullscreen placement when fullscreen.
        // Skipped once the window is destroyed (the WM_CLOSE-time save wrote
        // the real values; the exit-path save must not clobber them).
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        if (fullscreen) wp = savedPlacement;
        else if (!IsWindow(hwnd) || !GetWindowPlacement(hwnd, &wp)) wp = {};
        if (wp.rcNormalPosition.right > wp.rcNormalPosition.left) {
            wchar_t buf[96];
            _snwprintf_s(buf, _TRUNCATE, L"%ld,%ld,%ld,%ld,%d",
                         wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                         wp.rcNormalPosition.right, wp.rcNormalPosition.bottom,
                         wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
            WritePrivateProfileStringW(L"settings", L"window", buf, path.c_str());
        }
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
        ReleaseSRWLockExclusive(&g_iniLock);
    }

    void LoadConfig() {
        if (g_noPersist) return;
        std::wstring path = ConfigPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        presetFavorites.Load(path);
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
        morphSec = Clampi(GetPrivateProfileIntW(L"settings", L"morphSec", 27, path.c_str()), 5, 100) / 10.0f;
        calm = GetPrivateProfileIntW(L"settings", L"calm", 0, path.c_str()) != 0;
        shaderIdx = Clampi(GetPrivateProfileIntW(L"settings", L"shader", 0, path.c_str()), 0, SV_COUNT - 1);
        shufflePalettes = GetPrivateProfileIntW(L"settings", L"shufflePal", 0, path.c_str()) != 0;
        savedEngine = Clampi(GetPrivateProfileIntW(L"settings", L"engine", 0, path.c_str()), 0, 3);
        savedSpout = GetPrivateProfileIntW(L"settings", L"spout", 0, path.c_str()) != 0;
        wchar_t mf[MAX_PATH];
        GetPrivateProfileStringW(L"settings", L"milkFolder", L"", mf, MAX_PATH, path.c_str());
        libraryFolder = mf;
        wchar_t wb[96];
        GetPrivateProfileStringW(L"settings", L"window", L"", wb, 96, path.c_str());
        long l = 0, t = 0, r = 0, b = 0; int mx = 0;
        if (swscanf_s(wb, L"%ld,%ld,%ld,%ld,%d", &l, &t, &r, &b, &mx) == 5 && r > l && b > t) {
            restoreRect = { l, t, r, b };
            restoreMax = mx != 0;
            haveRestore = true;
        }
        wchar_t inc[128];
        GetPrivateProfileStringW(L"settings", L"palInclude", L"", inc, 128, path.c_str());
        palInclude.clear();
        for (wchar_t* q = inc; *q; ++q) palInclude.push_back(*q == L'0' ? 0 : 1);
        EnsurePalInclude(); // new palettes default to included
    }

    // Release everything this window owns (GL hosts, D2D). The audio tap is
    // global and outlives the windows.
    void Shutdown() {
        if (presetDialog) DestroyWindow(presetDialog);
        rec.Stop();
        spout.Stop();
        ReleaseRecOffscreen();
        shaderHost.Destroy();
        pmHost.Destroy();
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
#include "preset_browser.inl"
#include "selftest.inl"

// One App per display, heap-allocated so the pointers stashed in each window
// (GWLP_USERDATA) stay valid regardless of vector growth.
static std::vector<App*> g_views;

// The App that owns a given window, stashed at creation in GWLP_USERDATA.
static App* AppFromHwnd(HWND h) {
    return (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
}

// ----- polish: tray icon, kiosk cursor-hide, launch-at-login -----

#define WM_TRAY        (WM_APP + 1)
#define IDM_TRAY_SHOW  1
#define IDM_TRAY_QUIT  2
#define IDM_TRAY_TOP   3

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
    g_tray.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
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
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd); if (!dpi) dpi = 96;
        RECT bounds = {0, 0, MulDiv(760, dpi, 96), MulDiv(560, dpi, 96)};
        AdjustWindowRectExForDpi(&bounds, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, 0, dpi);
        auto mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top};
        return 0;
    }
    case WM_SIZE:
        if (a->rt) a->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        if (a->pmHost.ready) a->pmHost.Resize(LOWORD(lp), HIWORD(lp));
        if (a->shaderHost.ready) a->shaderHost.Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_DPICHANGED: {
        // Per-monitor-v2: adopt the suggested rect; WM_SIZE resizes the
        // targets and UiScale() picks up the new factor on the next frame.
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
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
    case WM_LBUTTONUP:
        // End a morph-slider drag and persist the length once (not per move).
        if (a->morphDragging) { a->morphDragging = false; ReleaseCapture(); a->SaveConfig(); }
        return 0;
    case WM_CAPTURECHANGED:
        if (a->morphDragging) { a->morphDragging = false; a->SaveConfig(); }
        return 0;
    case WM_SYSKEYDOWN:
        a->startupUiUntil = 0;
        break;  // preserve Windows shortcuts such as Alt+F4
    case WM_KEYDOWN:
        a->startupUiUntil = 0; // dismiss the launch preview, then handle this key normally
        // The preset lab is modal: while it's open every key belongs to the
        // editor (or its name field) — no app shortcut may fire mid-edit.
        if (a->labPanel) { a->LabKeyDown(wp); return 0; }
        // While the palette-name field has focus, letters belong to the name
        // (WM_CHAR), not to shortcuts — typing "Sunset" must not cycle the
        // shuffle timer or launch the Spotify flow. ESC still closes.
        if (a->palPanel) {
            if (wp == VK_ESCAPE) {
                if (a->nameFocused) a->nameFocused = false; else a->palPanel = false;
            } else if (!a->nameFocused && (wp == VK_UP || wp == VK_DOWN)) {
                a->palOffset = Clampi(a->palOffset + (wp == VK_UP ? -1 : 1), 0, a->PaletteCount() - 1);
            }
            return 0;
        }
        if (a->help) {
            if (wp == VK_ESCAPE || wp == VK_F1 || wp == VK_OEM_2) a->help = false;
            return 0;
        }
        // While the jump-to-page field has focus, keys drive that field (the
        // digits/Backspace/Enter arrive as WM_CHAR); swallow everything else so
        // it can't leak into shortcuts. ESC un-focuses but leaves the menu open.
        if (a->deckMenu >= 0 && a->pageFocused) {
            if (wp == VK_ESCAPE) a->pageFocused = false;
            return 0;
        }
        // Every dropdown is modal, including a single-page menu. Navigation
        // must not change the visual, source or recorder behind the menu.
        if (a->deckMenu >= 0) {
            if (wp == VK_ESCAPE) a->deckMenu = -1;
            else if ((wp == VK_LEFT || wp == VK_RIGHT) && a->rt) {
                int cols, perCol, pageSize, pages;
                float sc = a->UiScale(); D2D1_SIZE_F sz = a->rt->GetSize();
                a->MenuMetrics(a->deckMenu, sz.width / sc, sz.height / sc, cols, perCol, pageSize, pages);
                a->menuPage = Clampi(a->menuPage + (wp == VK_LEFT ? -1 : 1), 0, pages - 1);
            }
            return 0;
        }
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
        case '1':      g_tap.SetSource(AudioTap::SRC_LOOPBACK); break;
        case '2':      g_tap.SetSource(AudioTap::SRC_MIC); break;
        case '3':      g_tap.SetSource(AudioTap::SRC_DEMO); break;
        case '4':      a->OpenFile(); break;
        case 'S':      a->CycleShuffle(); break;
        case 'C':      a->SetCalm(!a->calm); break;
        case 'L':      a->OpenLab(); a->labIgnoreOpenChar = a->labPanel; break;
        case 'M':      // web parity: M = displays
            a->deckVisible = true; a->deckActivity = a->Now();
            if (a->deckMenu == App::DK_DISP) a->deckMenu = -1; else a->OpenMenu(App::DK_DISP);
            break;
        case 'E':      a->CycleEngine(); break;       // WaveScope -> GPU -> projectM
        case 'G':      a->ToggleShader(); break;      // jump straight to GPU
        case 'R':      a->ToggleRecord(); break;      // web parity: R = record
        case 'T':      a->SetAlwaysOnTop(!a->alwaysOnTop); break;
        case 'O':      a->ToggleSpout(); break;       // Spout output (OBS)
        case 'B':      a->SpotifyButton(); break;  // Spotify: authorize + play
        case 'F':      a->ToggleFullscreen(); break;
        case 'H':
            a->hud = !a->hud;
            if (a->hud) a->hudActivity = a->Now();
            break;
        case VK_F2:    a->OpenOptions(); break;
        case VK_F3:    a->OpenPresetBrowser(); break;
        case VK_F1:    a->help = !a->help; break;
        case VK_OEM_2: if (GetKeyState(VK_SHIFT) & 0x8000) a->help = !a->help; break; // '?'
        case VK_ESCAPE:
            // Web-style restore chain: panel -> open dropdown -> help ->
            // fullscreen -> (companion: close it) -> quit.
            if (a->palPanel) a->palPanel = false;
            else if (a->deckMenu >= 0) a->deckMenu = -1;
            else if (a->help) a->help = false;
            else if (a->fullscreen && !a->isCompanion) a->ToggleFullscreen();
            else if (a->isCompanion) DestroyWindow(hwnd);
            else PostQuitMessage(0);
            break;
        }
        return 0;
    case WM_CHAR:
        if (a->labPanel) { a->LabChar((wchar_t)wp); return 0; }
        if (a->deckMenu >= 0 && a->pageFocused) a->PageChar((wchar_t)wp);
        else a->PanelChar((wchar_t)wp);
        return 0;
    case WM_MOUSEWHEEL:
        if (a->labPanel) a->LabScrollBy((short)HIWORD(wp) > 0 ? -3 : 3);
        else if (a->palPanel) a->palOffset = Clampi(a->palOffset + ((short)HIWORD(wp) > 0 ? -3 : 3), 0, a->PaletteCount() - 1);
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, IDM_TRAY_SHOW, IsWindowVisible(hwnd) ? L"Hide" : L"Show");
            AppendMenuW(m, MF_STRING | (a->alwaysOnTop ? MF_CHECKED : 0),
                        IDM_TRAY_TOP, L"Always on top");
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
        } else if (LOWORD(wp) == IDM_TRAY_TOP) {
            a->SetAlwaysOnTop(!a->alwaysOnTop);
        } else if (LOWORD(wp) == IDM_TRAY_QUIT) {
            PostQuitMessage(0);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && g_trayAdded) { ShowWindow(hwnd, SW_HIDE); return 0; }
        break;  // other syscommands fall through to DefWindowProc
    case WM_CLOSE:
        if (!a->isCompanion) a->SaveConfig(); // capture placement while alive
        break;             // DefWindowProc destroys the window
    case WM_DESTROY:
        if (!a->isCompanion) {
            PostQuitMessage(0);
            return 0;
        }
        // A companion closes alone: unhook, free, and update the toggle.
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        for (size_t i = 0; i < g_views.size(); i++)
            if (g_views[i] == a) { g_views.erase(g_views.begin() + i); break; }
        a->Shutdown();
        delete a;
        g_companionsOpen = false;
        for (App* v : g_views) g_companionsOpen |= v->isCompanion;
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
    if (!v->hwnd) { delete v; return nullptr; }
    if (!v->CreateGraphics()) {
        // Unbind before destroying: late messages must not see a freed App,
        // and the null binding keeps WM_DESTROY from quitting the app.
        SetWindowLongPtrW(v->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(v->hwnd);
        delete v;
        return nullptr;
    }
    v->LoadConfig();
    // Restore the saved placement (normal windows only, and only when the
    // saved rect still lands on a live monitor).
    if (!borderless && v->haveRestore) {
        RECT r = v->restoreRect;
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) &&
            r.right - r.left >= 320 && r.bottom - r.top >= 240)
            SetWindowPos(v->hwnd, nullptr, r.left, r.top,
                         r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(v->hwnd, borderless ? SW_SHOW
                        : (v->restoreMax ? SW_SHOWMAXIMIZED : nCmdShow));
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

// Open one borderless companion window on every monitor the primary window
// is NOT on — all fed by the one global tap, each with its own visual
// (distinct starting mode/palette, focus a window to control it, Esc closes
// just that companion).
static void OpenCompanions(HINSTANCE inst) {
    if (g_views.empty()) return;
    App* prim = g_views[0];
    MONITORINFO pmi = { sizeof(MONITORINFO) };
    GetMonitorInfoW(MonitorFromWindow(prim->hwnd, MONITOR_DEFAULTTONEAREST), &pmi);
    std::vector<RECT> rects;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, (LPARAM)&rects);
    int made = 0;
    for (const RECT& r : rects) {
        if (EqualRect(&r, &pmi.rcMonitor)) continue;
        App* v = CreateView(inst, L"WaveScopeWindow", r, true, SW_SHOW);
        if (!v) continue;
        v->isCompanion = true;
        made++;
        v->modeIdx = (prim->modeIdx + 5 * made) % MODE_COUNT;
        v->paletteIdx = (prim->paletteIdx + made) % v->PaletteCount();
    }
    if (made > 0) g_companionsOpen = true;
    SetForegroundWindow(prim->hwnd);
}

static void CloseCompanions() {
    // DestroyWindow -> WM_DESTROY erases each from g_views and frees it.
    for (int i = (int)g_views.size() - 1; i >= 0; i--)
        if (g_views[i]->isCompanion) DestroyWindow(g_views[i]->hwnd);
    g_companionsOpen = false;
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
    // Per-monitor-v2 DPI awareness (Win10 1703+, our floor): real pixels on
    // every monitor, no bitmap stretching, WM_DPICHANGED on monitor moves.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // UI-thread shell dialogs

    LaunchOpts opts = ParseCmdLine();
    g_noPersist = opts.selftest;       // a test run must not rewrite the config
    if (!WsInitPaths(opts.selftest)) {
        MessageBoxW(nullptr, L"WaveScope could not open its settings folder.", L"WaveScope", MB_OK | MB_ICONERROR);
        CoUninitialize(); return 1;
    }
    srand((unsigned)GetTickCount64()); // shuffle must differ across launches

    // Explicit launch-at-login toggles (user runs these deliberately).
    if (opts.installStartup) { SetAutoStart(true);  return 0; }
    if (opts.removeStartup)  { SetAutoStart(false); return 0; }

    if (!opts.selftest) g_spotify.LoadToken(); // restore the stored Spotify sign-in

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"WaveScopeWindow";
    RegisterClassExW(&wc);

    // "wall" fills every display: a borderless primary on the first monitor
    // plus companion windows on the rest, all driven by the one tap.
    // "kiosk" is a single borderless full-screen window on the primary display
    // with an idle cursor-hide. Otherwise a single 1440x900 window.
    bool wall = opts.wall;
    bool kiosk = opts.kiosk;
    g_borderless = wall || kiosk;
    g_tap.Start();
    RECT primaryRect = { CW_USEDEFAULT, CW_USEDEFAULT,
                         CW_USEDEFAULT + 1440, CW_USEDEFAULT + 900 };
    if (wall || kiosk) {
        MONITORINFO mi = { sizeof(MONITORINFO) };
        if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi))
            primaryRect = mi.rcMonitor;
    }
    if (!CreateView(hInstance, wc.lpszClassName, primaryRect, g_borderless, nCmdShow))
        return 1;
    if (wall) OpenCompanions(hInstance);
    TrayAdd(g_views[0]->hwnd); // one tray icon for the app (primary window)

    // Command-line, applied to every window: "demo" synthetic signal,
    // "file=<path>" playback, "milkdrop"/"shader" boot into those engines.
    for (App* v : g_views) {
        if (opts.demo) g_tap.SetSource(AudioTap::SRC_DEMO);
        if (!opts.file.empty()) g_tap.SetFile(opts.file.c_str());
        if (opts.milkdrop) v->ToggleProjectM();
        if (opts.shader) v->ToggleShader();
    }
    // No engine flag given: restore the persisted engine on the primary view.
    if (!opts.milkdrop && !opts.shader && !opts.selftest && !g_views.empty()) {
        App* v = g_views[0];
        switch (v->savedEngine) {
        case 1: v->ToggleShader(); break;
        case 2: v->EnterProjectM(false); break;
        case 3: v->EnterProjectM(true); break;
        }
        if (v->savedSpout) v->ToggleSpout();
    }

    if (opts.selftest) {
        int rc = RunSelfTest(g_views[0]);
        TrayRemove();
        for (App* v : g_views) { v->Shutdown(); delete v; }
        g_views.clear();
        g_tap.Stop();
        CoUninitialize();
        return rc;
    }

    // Render loop. Analysis runs ONCE per iteration on the shared tap; every
    // visible window renders from the same frame. Pacing: the primary GL
    // engine vsyncs via wglSwapInterval; companions run with vsync off so N
    // windows never stack N vblank waits; if no vsynced GL present happened,
    // pace with DwmFlush (the D2D path does not block under DWM). Hidden/
    // minimized windows are skipped — a tray-hidden visualizer costs
    // ~nothing — and with no visible window the loop parks in
    // MsgWaitForMultipleObjects instead of spinning.
    MSG msg = {};
    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            bool handled = false;
            for (App* v : g_views) if (v->RouteDialogMessage(&msg)) { handled = true; break; }
            if (handled) continue;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        AudioFrame frame;
        g_tap.Analyze(frame, g_views[0]->Now());
        bool rendered = false, vsyncedGl = false;
        for (App* v : g_views) {
            if (!IsWindowVisible(v->hwnd) || IsIconic(v->hwnd)) continue;
            v->RenderFrame(frame);
            if (v->pmHost.ready) v->pmHost.PollReload(); // hot-reload .milk folder
            rendered = true;
            bool gl = (v->pmActive && v->pmHost.ready) ||
                      (v->shaderActive && v->shaderHost.ready);
            vsyncedGl |= gl && !v->isCompanion;
        }
        if (!rendered)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        else if (!vsyncedGl)
            DwmFlush(); // pace the frame to the compositor clock
        UpdateCursorHide(g_views[0]->Now());
    }
done:
    g_spotify.Shutdown(); // join the Spotify worker before statics tear down
    if (!g_views.empty()) g_views[0]->SaveConfig(); // primary window's setup persists
    if (g_cursorHidden) { while (ShowCursor(TRUE) < 0) {} g_cursorHidden = false; }
    TrayRemove();
    for (App* v : g_views) { v->Shutdown(); delete v; }
    g_views.clear();
    g_tap.Stop();
    CoUninitialize();
    return 0;
}
