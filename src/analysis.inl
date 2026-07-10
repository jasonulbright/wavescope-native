// The pure analysis math, shared by the app (main.cpp) and the unit tests
// (tests.cpp): the AudioFrame contract, HSL color, log-axis FFT sampling,
// the radix-2 FFT, and the Web-Audio-analyser-compatible per-window analysis
// (Blackman window, |X|/N, 0.8 smoothing on linear magnitudes, [-100,-30] dB
// mapping, Hz-based band split, RMS level, bass-flux beat envelope).
// No I/O, no threads, no Windows calls — everything here is testable.

static const float PI  = 3.14159265358979f;
static const float TAU = 6.28318530717959f;

// One analyzed slice per rendered frame; mirrors the web AudioFrame.
struct AudioFrame {
    float fft[1024];   // 0..1, the analyser's byte mapping
    float wave[2048];  // -1..1 mono time domain
    float level = 0, bass = 0, mid = 0, treble = 0, beat = 0;
    float t = 0, dt = 0.016f;
};

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

// Log-axis FFT sampling, matching the web engine's bin() helper.
static float BinLog(const AudioFrame& f, float pos) {
    if (pos < 0) pos = 0;
    if (pos > 1) pos = 1;
    int idx = (int)powf(1023.0f, pos * 0.92f + 0.08f);
    if (idx > 1023) idx = 1023;
    return f.fft[idx];
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

// Everything the per-window analysis remembers between frames.
struct AnalysisState {
    float smooth[1024] = {};
    float level = 0, bassAvg = 0, beat = 0, lastT = 0;
};

// Analyze one 2048-sample window into an AudioFrame, exactly like the Web
// Audio analyser the web app reads: Blackman window, FFT, |X|/N, per-bin
// smoothing 0.8 on LINEAR magnitudes, then the [-100,-30] dB byte mapping.
// Bands split at 250 Hz / 4 kHz computed from the REAL sample rate; level is
// quarter-decimated RMS smoothed by 0.2; the beat envelope charges when bass
// exceeds 1.35x its own moving average (and an 0.08 floor) and decays e^-6dt.
static void AnalyzeWindow(AnalysisState& st, const float window[2048],
                          float sampleRate, float now, AudioFrame& f) {
    float dt = now - st.lastT;
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.1f)   dt = 0.1f;
    st.lastT = now;
    f.t = now;
    f.dt = dt;

    for (int i = 0; i < 2048; i++) f.wave[i] = window[i];

    static thread_local float re[2048], im[2048];
    for (int i = 0; i < 2048; i++) {
        float bw = 0.42f - 0.5f * cosf(TAU * i / 2048.0f)
                         + 0.08f * cosf(2.0f * TAU * i / 2048.0f);
        re[i] = window[i] * bw;
        im[i] = 0.0f;
    }
    Fft2048(re, im);
    for (int i = 0; i < 1024; i++) {
        float mag = sqrtf(re[i] * re[i] + im[i] * im[i]) / 2048.0f;
        st.smooth[i] = st.smooth[i] * 0.8f + mag * 0.2f;
        float db = 20.0f * log10f(st.smooth[i] + 1e-12f);
        float norm = (db + 100.0f) / 70.0f;
        if (norm < 0) norm = 0;
        if (norm > 1) norm = 1;
        f.fft[i] = norm;
    }

    float sr = sampleRate > 0 ? sampleRate : 48000.0f;
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
    st.level += (rms - st.level) * 0.2f;
    f.level = st.level;

    st.bassAvg += (f.bass - st.bassAvg) * 0.04f;
    if (f.bass > st.bassAvg * 1.35f && f.bass > 0.08f) st.beat = 1.0f;
    st.beat *= expf(-dt * 6.0f);
    f.beat = st.beat;
}
