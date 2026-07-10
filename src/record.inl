// Clip recording: hotkey R captures the active engine's render target
// (pre-UI) plus the analysed audio into wavescope-<timestamp>.mp4 beside the
// exe. Media Foundation SinkWriter, hardware H.264 where available + AAC —
// zero new dependencies. Video frames arrive bottom-up BGRA from the render
// thread through a small drop-oldest queue; a worker thread owns the writer.
// Audio is pulled on the worker from the same AudioTap ring the analysis
// reads (its own cursor), mono -> 16-bit stereo.

#include <deque>
#include <mutex>

struct Recorder {
    // ---- render-thread interface ----

    bool Active() const { return m_active.load(); }
    int  Width()  const { return m_w; }
    int  Height() const { return m_h; }
    float Seconds() {
        if (!m_active.load()) return 0;
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
        return (float)((double)(t.QuadPart - m_t0.QuadPart) / (double)f.QuadPart);
    }

    // Begin recording at a fixed frame size (evened for H.264). Returns false
    // if the sink writer can't start (no encoder, bad path).
    bool Start(int w, int h, UINT32 sampleRate, AudioTap* tap) {
        if (m_active.load()) return true;
        m_w = w & ~1; m_h = h & ~1;
        if (m_w < 64 || m_h < 64) return false;
        m_rate = (sampleRate == 44100 || sampleRate == 48000) ? sampleRate : 48000;
        m_tap = tap;
        m_quit = false;
        m_videoSamples = 0;
        m_audioSamples = 0;
        m_dropNote = 0;
        {
            std::lock_guard<std::mutex> g(m_qLock);
            m_queue.clear();
        }
        // wavescope-YYYYMMDD-HHMMSS.mp4 beside the exe.
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"wavescope-%04d%02d%02d-%02d%02d%02d.mp4",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        m_path = PmExeDir() + name;
        QueryPerformanceCounter(&m_t0);
        m_active = true;
        m_ok = false;
        m_worker = std::thread([this] { Run(); });
        // Wait briefly for the writer to come up so the caller can report.
        for (int i = 0; i < 100 && m_active.load() && !m_ok.load(); i++) Sleep(10);
        if (!m_ok.load()) { Stop(); return false; }
        return true;
    }

    void Stop() {
        if (!m_active.exchange(false)) return;
        m_quit = true;
        if (m_worker.joinable()) m_worker.join();
    }

    // Push one BGRA frame (bottom-up unless topDown). Frames smaller than the
    // record size stop the recording cleanly (engine/res switched mid-take ->
    // finalize the file); larger ones are cropped.
    void PushVideo(const uint8_t* bgra, int w, int h, bool topDown = false) {
        if (!m_active.load()) return;
        if (w < m_w || h < m_h) { m_sizeBroke = true; return; }
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        VideoFrame f;
        f.qpc = t.QuadPart;
        f.data.resize((size_t)m_w * m_h * 4);
        // Sample rows are bottom-up (row 0 = image bottom).
        for (int y = 0; y < m_h; y++) {
            int src = topDown ? (h - 1 - y) : y;
            memcpy(&f.data[(size_t)y * m_w * 4], bgra + (size_t)src * w * 4, (size_t)m_w * 4);
        }
        std::lock_guard<std::mutex> g(m_qLock);
        if (m_queue.size() >= 8) { m_queue.pop_front(); m_dropNote++; }  // drop oldest
        m_queue.push_back(std::move(f));
    }

    const wchar_t* Path() const { return m_path.c_str(); }

private:
    struct VideoFrame { LONGLONG qpc = 0; std::vector<uint8_t> data; };

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        MFStartup(MF_VERSION, MFSTARTUP_LITE);

        IMFSinkWriter* wr = nullptr;
        DWORD vStream = 0, aStream = 0;
        bool audioOn = false;
        HRESULT hr;

        IMFAttributes* attr = nullptr;
        MFCreateAttributes(&attr, 2);
        if (attr) {
            attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        }
        hr = MFCreateSinkWriterFromURL(m_path.c_str(), nullptr, attr, &wr);
        if (attr) attr->Release();

        // Video out: H.264 at a bitrate scaled to the pixel rate.
        if (SUCCEEDED(hr)) {
            IMFMediaType* mt = nullptr; MFCreateMediaType(&mt);
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            UINT64 pixrate = (UINT64)m_w * m_h * FPS;
            UINT32 kbps = (UINT32)(pixrate / 10);            // ~0.1 bpp
            if (kbps < 2000000) kbps = 2000000;
            if (kbps > 40000000) kbps = 40000000;
            mt->SetUINT32(MF_MT_AVG_BITRATE, kbps);
            MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, m_w, m_h);
            MFSetAttributeRatio(mt, MF_MT_FRAME_RATE, FPS, 1);
            MFSetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            hr = wr->AddStream(mt, &vStream);
            mt->Release();
        }
        // Video in: bottom-up BGRA (RGB32), the natural GL/GDI orientation.
        if (SUCCEEDED(hr)) {
            IMFMediaType* mt = nullptr; MFCreateMediaType(&mt);
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            MFSetAttributeSize(mt, MF_MT_FRAME_SIZE, m_w, m_h);
            MFSetAttributeRatio(mt, MF_MT_FRAME_RATE, FPS, 1);
            MFSetAttributeRatio(mt, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            hr = wr->SetInputMediaType(vStream, mt, nullptr);
            mt->Release();
        }
        // Audio out/in: AAC 128 kbps from 16-bit stereo PCM. Optional — a
        // failure here records video-only rather than nothing.
        if (SUCCEEDED(hr)) {
            IMFMediaType* mt = nullptr; MFCreateMediaType(&mt);
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            mt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_rate);
            mt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            mt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
            HRESULT ah = wr->AddStream(mt, &aStream);
            mt->Release();
            if (SUCCEEDED(ah)) {
                IMFMediaType* in = nullptr; MFCreateMediaType(&in);
                in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_rate);
                in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_rate * 4);
                audioOn = SUCCEEDED(wr->SetInputMediaType(aStream, in, nullptr));
                in->Release();
            }
        }
        if (SUCCEEDED(hr)) hr = wr->BeginWriting();
        if (FAILED(hr)) {
            if (wr) { wr->Release(); wr = nullptr; }
            m_active = false;
            MFShutdown();
            CoUninitialize();
            return;
        }
        m_ok = true;

        LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
        AudioTap::TapCursor audioCur;
        float pcm[4800];
        std::vector<int16_t> st16;

        while (!m_quit.load() || QueueDepth() > 0) {
            // Video: drain the queue.
            VideoFrame f;
            bool have = false;
            {
                std::lock_guard<std::mutex> g(m_qLock);
                if (!m_queue.empty()) { f = std::move(m_queue.front()); m_queue.pop_front(); have = true; }
            }
            if (have) {
                IMFSample* s = nullptr; IMFMediaBuffer* b = nullptr;
                DWORD cb = (DWORD)f.data.size();
                if (SUCCEEDED(MFCreateMemoryBuffer(cb, &b))) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(b->Lock(&dst, nullptr, nullptr))) {
                        memcpy(dst, f.data.data(), cb);
                        b->Unlock();
                        b->SetCurrentLength(cb);
                        if (SUCCEEDED(MFCreateSample(&s))) {
                            s->AddBuffer(b);
                            LONGLONG rt = (LONGLONG)((double)(f.qpc - m_t0.QuadPart) * 10000000.0 / (double)qpf.QuadPart);
                            s->SetSampleTime(rt);
                            s->SetSampleDuration(10000000 / FPS);
                            wr->WriteSample(vStream, s);
                            m_videoSamples++;
                            s->Release();
                        }
                    }
                    b->Release();
                }
            }
            // Audio: pull whatever new analysed samples exist (mono -> stereo16).
            if (audioOn && m_tap) {
                int n = m_tap->ReadNew(audioCur, pcm, 4800);
                if (n > 0) {
                    st16.resize((size_t)n * 2);
                    for (int i = 0; i < n; i++) {
                        float v = pcm[i];
                        if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
                        int16_t q = (int16_t)(v * 32000.0f);
                        st16[(size_t)i * 2] = q; st16[(size_t)i * 2 + 1] = q;
                    }
                    DWORD cb = (DWORD)(st16.size() * sizeof(int16_t));
                    IMFSample* s = nullptr; IMFMediaBuffer* b = nullptr;
                    if (SUCCEEDED(MFCreateMemoryBuffer(cb, &b))) {
                        BYTE* dst = nullptr;
                        if (SUCCEEDED(b->Lock(&dst, nullptr, nullptr))) {
                            memcpy(dst, st16.data(), cb);
                            b->Unlock();
                            b->SetCurrentLength(cb);
                            if (SUCCEEDED(MFCreateSample(&s))) {
                                s->AddBuffer(b);
                                LONGLONG rt = (LONGLONG)(m_audioSamples * 10000000ull / m_rate);
                                s->SetSampleTime(rt);
                                s->SetSampleDuration((LONGLONG)n * 10000000ll / m_rate);
                                wr->WriteSample(aStream, s);
                                m_audioSamples += n;
                                s->Release();
                            }
                        }
                        b->Release();
                    }
                }
            }
            if (m_sizeBroke.load()) break;   // engine/res changed mid-take
            if (!have) Sleep(4);
        }

        wr->Finalize();
        wr->Release();
        m_sizeBroke = false;
        m_active = false;
        MFShutdown();
        CoUninitialize();
    }

    size_t QueueDepth() {
        std::lock_guard<std::mutex> g(m_qLock);
        return m_queue.size();
    }

    static const UINT32 FPS = 60;
    std::atomic<bool> m_active{ false };
    std::atomic<bool> m_quit{ false };
    std::atomic<bool> m_ok{ false };
    std::atomic<bool> m_sizeBroke{ false };
    std::thread m_worker;
    std::mutex m_qLock;
    std::deque<VideoFrame> m_queue;
    AudioTap* m_tap = nullptr;
    std::wstring m_path;
    int m_w = 0, m_h = 0;
    UINT32 m_rate = 48000;
    LARGE_INTEGER m_t0 = {};
    uint64_t m_videoSamples = 0, m_audioSamples = 0;
    int m_dropNote = 0;
};
