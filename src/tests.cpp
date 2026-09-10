// Unit tests for the pure math, mirroring the web repo's __tests__ approach
// (registry integrity + palette behavior) plus the analysis contract the two
// apps must share. Console exe, zero frameworks: every CHECK prints on
// failure and the process exit code fails the build (see build.bat).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#pragma comment(lib, "d2d1.lib")   // the mode registry references D2D matrix helpers

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <shellapi.h>
static std::wstring g_testDir;
static std::wstring PmExeDir() { return g_testDir; }
#include "spotify.inl"

template <typename T> static void SafeRelease(T** p) {
    if (*p) { (*p)->Release(); *p = nullptr; }
}

#include "analysis.inl"
#include "viz_modes.inl"

static int g_failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        g_failures++; \
        printf("FAIL %s:%d  %s\n      ", __FILE__, __LINE__, #cond); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } } while (0)

static bool Near(float a, float b, float eps) { return fabsf(a - b) <= eps; }
#include "security_tests.inl"

// ---------------------------------------------------------------- FFT

static void TestFftImpulse() {
    // delta[0] -> flat spectrum of magnitude 1 in every bin.
    static float re[2048], im[2048];
    memset(re, 0, sizeof(re)); memset(im, 0, sizeof(im));
    re[0] = 1.0f;
    Fft2048(re, im);
    for (int i = 0; i < 2048; i += 97) {
        float mag = sqrtf(re[i] * re[i] + im[i] * im[i]);
        CHECK(Near(mag, 1.0f, 1e-3f), "impulse bin %d mag=%f", i, mag);
    }
}

static void TestFftSine() {
    // A sine exactly on bin k concentrates all energy there (mag = N/2).
    const int k = 100;
    static float re[2048], im[2048];
    for (int i = 0; i < 2048; i++) {
        re[i] = sinf(TAU * k * i / 2048.0f);
        im[i] = 0;
    }
    Fft2048(re, im);
    float peak = sqrtf(re[k] * re[k] + im[k] * im[k]);
    CHECK(Near(peak, 1024.0f, 1.0f), "sine peak=%f want 1024", peak);
    for (int i = 0; i < 1024; i++) {
        if (i == k) continue;
        float mag = sqrtf(re[i] * re[i] + im[i] * im[i]);
        CHECK(mag < 0.5f, "sine leakage at bin %d mag=%f", i, mag);
        if (g_failures > 5) return;
    }
}

static void TestFftParseval() {
    // sum |x|^2 == (1/N) sum |X|^2 for a noisy signal.
    static float re[2048], im[2048];
    srand(7);
    double tsum = 0;
    for (int i = 0; i < 2048; i++) {
        re[i] = (float)rand() / RAND_MAX - 0.5f;
        im[i] = 0;
        tsum += (double)re[i] * re[i];
    }
    Fft2048(re, im);
    double fsum = 0;
    for (int i = 0; i < 2048; i++) fsum += (double)re[i] * re[i] + (double)im[i] * im[i];
    fsum /= 2048.0;
    CHECK(Near((float)tsum, (float)fsum, 0.01f * (float)tsum),
          "parseval time=%f freq=%f", tsum, fsum);
}

// ------------------------------------------------------------ analysis

// Run AnalyzeWindow until smoothing converges on a steady signal.
static void Converge(AnalysisState& st, const float* window, float sr, AudioFrame& f) {
    for (int i = 0; i < 120; i++)
        AnalyzeWindow(st, window, sr, 0.016f * (i + 1), f);
}

static void TestAnalyserMapping() {
    // Full-scale 1 kHz sine at 48k: the fed bin must sit near the top of the
    // [-100,-30] dB window; far-away bins near the floor.
    const float sr = 48000.0f;
    float hzPerBin = sr / 2 / 1024;                  // 23.4 Hz
    int bin = (int)(1000.0f / hzPerBin + 0.5f);      // ~bin 43
    float k = bin * 2048.0f / 2048.0f;               // cycles per window = bin
    static float window[2048];
    for (int i = 0; i < 2048; i++) window[i] = sinf(TAU * bin * i / 2048.0f);
    AnalysisState st; AudioFrame f;
    Converge(st, window, sr, f);
    // Blackman coherent gain 0.42: peak magnitude ~0.5*0.42 -> ~ -13.5 dB ->
    // norm ~ (100-13.5)/70 ~ 1.0+ (clamped). Expect the hot bin pegged high.
    CHECK(f.fft[bin] > 0.95f, "hot bin %d = %f", bin, f.fft[bin]);
    CHECK(f.fft[900] < 0.35f, "far bin 900 = %f", f.fft[900]);
    // Level: RMS of a full-scale sine is 0.707, smoothed toward it.
    CHECK(Near(f.level, 0.707f, 0.03f), "level=%f", f.level);
    (void)k;
}

static void TestBandsSplit() {
    // Synthetic frame: energy ONLY in the bass range must move bass, not mid.
    const float sr = 48000.0f;
    static float window[2048];
    for (int i = 0; i < 2048; i++) window[i] = 0.8f * sinf(TAU * 4 * i / 2048.0f); // ~94 Hz
    AnalysisState st; AudioFrame f;
    Converge(st, window, sr, f);
    CHECK(f.bass > f.mid * 1.5f, "bass=%f mid=%f", f.bass, f.mid);
    CHECK(f.bass > f.treble * 1.5f, "bass=%f treble=%f", f.bass, f.treble);

    // Same tone at 44.1k must land in the same Hz band (bins differ).
    AnalysisState st2; AudioFrame f2;
    Converge(st2, window, 44100.0f, f2);
    CHECK(f2.bass > f2.mid * 1.5f, "44.1k bass=%f mid=%f", f2.bass, f2.mid);
}

static void TestBeatEnvelope() {
    // Quiet noise floor, then a bass burst: envelope charges to ~1 and
    // decays like e^(-6 dt).
    const float sr = 48000.0f;
    static float quiet[2048], loud[2048];
    for (int i = 0; i < 2048; i++) {
        quiet[i] = 0.02f * sinf(TAU * 4 * i / 2048.0f);
        loud[i]  = 0.90f * sinf(TAU * 4 * i / 2048.0f);
    }
    AnalysisState st; AudioFrame f;
    float now = 0;
    for (int i = 0; i < 200; i++) { now += 0.016f; AnalyzeWindow(st, quiet, sr, now, f); }
    float calmBeat = f.beat;
    CHECK(calmBeat < 0.05f, "beat should be idle, got %f", calmBeat);
    now += 0.016f; AnalyzeWindow(st, loud, sr, now, f);
    now += 0.016f; AnalyzeWindow(st, loud, sr, now, f);
    float charged = f.beat;
    CHECK(charged > 0.8f, "beat should charge on a bass jump, got %f", charged);
    // Decay: hold the loud signal (avg catches up, gate closes), measure the slope.
    float prev = f.beat;
    for (int i = 0; i < 30; i++) { now += 0.016f; AnalyzeWindow(st, loud, sr, now, f); }
    float expect = prev * expf(-6.0f * 0.016f * 30);
    CHECK(Near(f.beat, expect, 0.15f), "decay got %f want ~%f", f.beat, expect);
}

static void TestBinLog() {
    // BinLog must match the web bin(): floor(pow(1023, pos*0.92+0.08)).
    AudioFrame f;
    for (int i = 0; i < 1024; i++) f.fft[i] = (float)i;
    const float poses[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (float pos : poses) {
        int want = (int)powf(1023.0f, pos * 0.92f + 0.08f);
        if (want > 1023) want = 1023;
        CHECK((int)BinLog(f, pos) == want, "pos %f -> %d want %d", pos, (int)BinLog(f, pos), want);
    }
    CHECK((int)BinLog(f, -1.0f) == (int)BinLog(f, 0.0f), "clamp low");
    CHECK((int)BinLog(f, 2.0f) == 1023, "clamp high");
}

// ----------------------------------------------------- palettes + registry

static void TestPalettes() {
    CHECK(PAL_COUNT == 6, "PAL_COUNT=%d", PAL_COUNT);
    for (int i = 0; i < PAL_COUNT; i++) {
        CHECK(PALETTES[i].name && *PALETTES[i].name, "palette %d unnamed", i);
        // Colors must be valid and get brighter (or equal) along the axis for
        // the mono ramps; all channels in range everywhere.
        D2D1_COLOR_F lo = PALETTES[i].color(0.0f, 1.0f);
        D2D1_COLOR_F hi = PALETTES[i].color(1.0f, 1.0f);
        auto lum = [](D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
        for (float p = 0; p <= 1.001f; p += 0.1f) {
            D2D1_COLOR_F c = PALETTES[i].color(p, 0.5f);
            CHECK(c.r >= 0 && c.r <= 1 && c.g >= 0 && c.g <= 1 && c.b >= 0 && c.b <= 1,
                  "palette %d out of range at %f", i, p);
            CHECK(Near(c.a, 0.5f, 1e-4f), "palette %d alpha", i);
        }
        if (i != 4) // FULL SPECTRUM travels hue, not lightness
            CHECK(lum(hi) > lum(lo), "palette %d should brighten: %f -> %f", i, lum(lo), lum(hi));
    }
    // The default palette is the brand ultramarine: hue ~232.
    D2D1_COLOR_F c = PALETTES[0].color(0.5f, 1.0f);
    CHECK(c.b > c.r && c.b > c.g, "ultramarine should be blue-dominant");

    // Custom palettes: 1 stop = constant, 3 stops interpolate endpoints.
    g_customStopCount = 1;
    g_customStops[0][0] = 0.2f; g_customStops[0][1] = 0.4f; g_customStops[0][2] = 0.6f;
    D2D1_COLOR_F c1 = PalCustom(0.0f, 1.0f), c2 = PalCustom(1.0f, 1.0f);
    CHECK(Near(c1.g, c2.g, 1e-5f), "1-stop custom should be constant");
    g_customStopCount = 3;
    float stops[3][3] = { {0, 0, 0}, {0.5f, 0.5f, 0.5f}, {1, 1, 1} };
    memcpy(g_customStops, stops, sizeof(stops));
    CHECK(Near(PalCustom(0.0f, 1).r, 0.0f, 1e-5f), "custom start");
    CHECK(Near(PalCustom(0.5f, 1).r, 0.5f, 1e-5f), "custom middle");
    CHECK(Near(PalCustom(1.0f, 1).r, 1.0f, 1e-5f), "custom end");
    CHECK(Near(PalCustom(0.25f, 1).r, 0.25f, 1e-5f), "custom interp");
}

static void TestModeRegistry() {
    CHECK(MODE_COUNT == 34, "MODE_COUNT=%d", MODE_COUNT);
    int flash = 0;
    for (int i = 0; i < MODE_COUNT; i++) {
        CHECK(MODES[i].name && *MODES[i].name, "mode %d unnamed", i);
        CHECK(MODES[i].draw != nullptr, "mode %d has no draw", i);
        CHECK(MODES[i].fade > 0.0f && MODES[i].fade <= 1.0f, "mode %d fade=%f", i, MODES[i].fade);
        for (int j = i + 1; j < MODE_COUNT; j++)
            CHECK(wcscmp(MODES[i].name, MODES[j].name) != 0, "duplicate name %d/%d", i, j);
        if (MODES[i].flash) flash++;
    }
    // Calm mode excludes exactly the 5 beat-burst modes (web parity).
    CHECK(flash == 5, "flash modes=%d want 5", flash);
}

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestSpotifyProtocol();
    TestSpotifyCallback();
    TestSpotifyConfig();
    TestFftImpulse();
    TestFftSine();
    TestFftParseval();
    TestAnalyserMapping();
    TestBandsSplit();
    TestBeatEnvelope();
    TestBinLog();
    TestPalettes();
    TestModeRegistry();
    winrt::uninit_apartment();
    if (g_failures == 0) {
        printf("tests: ALL PASS\n");
        return 0;
    }
    printf("tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
