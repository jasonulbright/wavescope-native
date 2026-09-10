// Media Foundation decoding and WASAPI output live on the audio worker.
// Transport requests are atomics; UI code never calls either audio API.
void AudioTap::RunFile() {
    AcquireSRWLockShared(&m_lock);
    auto f = m_file; uint32_t gen = m_fileGen.load();
    ReleaseSRWLockShared(&m_lock);
    auto current = [&] { return !m_quit && Source() == SRC_FILE && m_fileGen.load() == gen; };
    if (!f) { Sleep(30); return; }
    // Leave a visible error until the user picks another file/source. Avoid
    // reopening a broken file in a busy loop or silently switching to capture.
    if (FAILED(f->error.load())) { Sleep(30); return; }

    const UINT32 RATE = 48000, CH = 2;
    const LONGLONG SECOND = 10000000;
    IMFSourceReader* reader = nullptr; IMFMediaType* type = nullptr;
    IMMDeviceEnumerator* devices = nullptr; IMMDevice* device = nullptr;
    IAudioClient* client = nullptr; IAudioRenderClient* render = nullptr;
    UINT32 bufferFrames = 0;
    HRESULT hr = MFCreateSourceReaderFromURL(f->path.c_str(), nullptr, &reader);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(hr)) hr = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) {
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, CH);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, CH * 4);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RATE * CH * 4);
        hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
    }
    if (SUCCEEDED(hr)) {
        PROPVARIANT value; PropVariantInit(&value);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &value)) &&
            value.vt == VT_UI8 && value.uhVal.QuadPart <= (ULONGLONG)LLONG_MAX)
            f->duration = (LONGLONG)value.uhVal.QuadPart;
        PropVariantClear(&value);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
            MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &value)) && value.vt == VT_UI4)
            f->seekable = f->duration > 0 && (value.ulVal & MFMEDIASOURCE_CAN_SEEK) != 0;
        PropVariantClear(&value);
        f->loop = f->seekable.load();
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&devices);
    }
    if (SUCCEEDED(hr)) hr = devices->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT; wf.nChannels = CH;
    wf.nSamplesPerSec = RATE; wf.wBitsPerSample = 32;
    wf.nBlockAlign = CH * 4; wf.nAvgBytesPerSec = RATE * CH * 4;
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        1000000, 0, &wf, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);

    bool running = false, eof = false;
    std::vector<float> pending; size_t offset = 0;
    LONGLONG anchor = 0, discardBefore = 0; uint64_t submitted = 0;
    auto clearRing = [&] {
        AcquireSRWLockExclusive(&m_lock);
        memset(m_ring, 0, sizeof(m_ring));
        ReleaseSRWLockExclusive(&m_lock);
    };
    auto seek = [&](LONGLONG target) {
        HRESULT result = client->Stop(); running = false;
        if (SUCCEEDED(result)) result = client->Reset();
        PROPVARIANT value; PropVariantInit(&value); value.vt = VT_I8; value.hVal.QuadPart = target;
        bool atEnd = f->duration > 0 && target >= f->duration;
        if (SUCCEEDED(result) && !atEnd) result = reader->SetCurrentPosition(GUID_NULL, value);
        PropVariantClear(&value);
        if (SUCCEEDED(result)) {
            pending.clear(); offset = 0; submitted = 0; eof = atEnd;
            anchor = discardBefore = target; f->position = target; f->ended = atEnd; clearRing();
        }
        return result;
    };
    if (SUCCEEDED(hr) && current()) { m_sampleRate = RATE; clearRing(); f->ready = true; }
    while (SUCCEEDED(hr) && current()) {
        if (f->seek >= 0) {
            // Keep the UI from restoring the old thumb position between
            // accepting a request and publishing its completed position.
            f->seeking = true;
            LONGLONG requested = f->seek.exchange(-1);
            hr = seek(requested); f->seeking = false;
            if (FAILED(hr)) break;
        }
        if (f->paused) {
            if (running) { hr = client->Stop(); running = false; }
            Sleep(5); continue;
        }
        if (!running) { hr = client->Start(); running = SUCCEEDED(hr); if (FAILED(hr)) break; }
        if (!eof && pending.size() - offset < (size_t)bufferFrames * CH * 4) {
            DWORD flags = 0; IMFSample* sample = nullptr; LONGLONG timestamp = 0;
            hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &timestamp, &sample);
            if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_ERROR)) hr = E_FAIL;
            eof = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (SUCCEEDED(hr) && sample) {
                IMFMediaBuffer* buffer = nullptr;
                hr = sample->ConvertToContiguousBuffer(&buffer);
                if (SUCCEEDED(hr)) {
                    BYTE* data = nullptr; DWORD length = 0;
                    hr = buffer->Lock(&data, nullptr, &length);
                    if (SUCCEEDED(hr)) {
                        const float* samples = (const float*)data;
                        size_t frames = length / (CH * sizeof(float));
                        // Compressed formats can seek to an earlier packet.
                        // Drop preroll so the displayed position and sound agree.
                        long double trim = ((long double)discardBefore - timestamp) * RATE / SECOND;
                        size_t skip = trim > 0 ? (size_t)(std::min)(trim, (long double)frames) : 0;
                        pending.insert(pending.end(), samples + skip * CH, samples + frames * CH);
                        buffer->Unlock();
                    }
                }
                SafeRelease(&buffer);
            }
            SafeRelease(&sample);
            if (FAILED(hr)) break;
        }
        UINT32 padding = 0; hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) break;
        uint64_t played = submitted > padding ? submitted - padding : 0;
        LONGLONG position = anchor + (LONGLONG)(played * SECOND / RATE);
        f->position = f->duration > 0 ? (std::min)(position, f->duration.load()) : position;
        UINT32 available = bufferFrames > padding ? bufferFrames - padding : 0;
        UINT32 count = (UINT32)(std::min)((pending.size() - offset) / CH, (size_t)available);
        if (count) {
            BYTE* output = nullptr; hr = render->GetBuffer(count, &output);
            if (FAILED(hr)) break;
            memcpy(output, pending.data() + offset, (size_t)count * CH * sizeof(float));
            hr = render->ReleaseBuffer(count, 0);
            if (FAILED(hr)) break;
            AcquireSRWLockExclusive(&m_lock);
            for (UINT32 i = 0; i < count; ++i)
                m_ring[(m_writeIdx++) % RING] = (pending[offset + i * CH] + pending[offset + i * CH + 1]) * 0.5f;
            ReleaseSRWLockExclusive(&m_lock);
            offset += (size_t)count * CH; submitted += count;
        }
        // Drain both queues before looping or stopping. The old implementation
        // appended another copy as soon as decoding reached EOF.
        if (eof && offset == pending.size() && padding == 0 && count == 0) {
            if (f->loop && (submitted > 0 || anchor > 0)) hr = seek(0);
            else {
                hr = client->Stop(); running = false; clearRing();
                if (f->duration > 0) f->position = f->duration.load();
                f->paused = true; f->ended = true;
            }
        }
        if (offset > (1 << 18)) { pending.erase(pending.begin(), pending.begin() + offset); offset = 0; }
        Sleep(3);
    }
    if (client) client->Stop();
    if (FAILED(hr)) { f->error = hr; f->paused = true; clearRing(); }
    SafeRelease(&render); SafeRelease(&client); SafeRelease(&device); SafeRelease(&devices);
    SafeRelease(&type); SafeRelease(&reader);
}
