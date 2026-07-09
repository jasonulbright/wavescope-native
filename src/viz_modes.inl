// SpectraViz visualizer library: palettes, the DrawCtx primitives, and all 34
// modes ported from the WaveScope web engine (the TypeScript draw() functions
// translate nearly line-for-line). Included by main.cpp as one translation
// unit; relies on AudioFrame, Hsla, PI, TAU, BinLog defined there/below.
//
// Single-instance app, so per-mode persistent state lives in function-local
// statics (particle pools, history buffers, peak arrays), mirroring the web's
// per-instance `state` object.

#include <vector>

// ------------------------------------------------------------- small helpers

// Deterministic xorshift, matching the web rng(seed) so layouts are stable.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    float operator()() {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (float)(s % 1000000u) / 1000000.0f;
    }
};

static inline float frand() { return (float)rand() / (float)RAND_MAX; }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// Cheap 2D value noise (flow field), matching the web noise2.
static float Noise2(float x, float y) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    auto h = [](float a, float b) {
        float n = sinf(a * 127.1f + b * 311.7f) * 43758.5453f;
        return n - floorf(n);
    };
    auto s = [](float t) { return t * t * (3 - 2 * t); };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float a = lerp(h((float)ix, (float)iy), h((float)ix + 1, (float)iy), s(fx));
    float b = lerp(h((float)ix, (float)iy + 1), h((float)ix + 1, (float)iy + 1), s(fx));
    return lerp(a, b, s(fy));
}

// n log-spaced band magnitudes into `prev`, lightly smoothed across frames.
static void Bands(const AudioFrame& f, float* prev, int n, float smooth) {
    for (int i = 0; i < n; i++) {
        float target = BinLog(f, (float)i / (n - 1 > 0 ? n - 1 : 1));
        prev[i] += (target - prev[i]) * (1.0f - smooth);
    }
}

// --------------------------------------------------------------- palettes

// Each palette maps a 0-1 position + alpha to a color. Backgrounds are always
// instrument-black; the palette colors the traces.
static D2D1_COLOR_F PalPhosphor(float pos, float a) { return Hsla(232, 0.64f, 0.38f + clampf(pos,0,1) * 0.47f, a); }
static D2D1_COLOR_F PalEmerald(float pos, float a)  { return Hsla(152, 0.60f, 0.38f + clampf(pos,0,1) * 0.47f, a); }
static D2D1_COLOR_F PalAmber(float pos, float a)    { return Hsla(40,  0.72f, 0.38f + clampf(pos,0,1) * 0.47f, a); }
static D2D1_COLOR_F PalPaper(float pos, float a)    { return Hsla(140, 0.06f, 0.60f + clampf(pos,0,1) * 0.36f, a); }
static D2D1_COLOR_F PalSpectrum(float pos, float a) { return Hsla(fmodf(clampf(pos,0,1) * 300 + 220, 360), 0.70f, 0.60f, a); }
static D2D1_COLOR_F PalMagma(float pos, float a)    { float p = clampf(pos,0,1); return Hsla(10 + p * 45, 0.70f + p * 0.20f, 0.30f + p * 0.45f, a); }

// Custom palette: up to 3 interpolated RGB stops. One is active at a time, so
// the stops live in globals that PalCustom reads (keeps the fast fn-pointer
// palette design while allowing runtime, user-created palettes).
static float g_customStops[3][3] = { { 0.2f, 0.27f, 0.79f }, {}, {} };
static int g_customStopCount = 1;
static D2D1_COLOR_F PalCustom(float pos, float a) {
    pos = clampf(pos, 0, 1);
    int n = g_customStopCount;
    if (n <= 1) return D2D1::ColorF(g_customStops[0][0], g_customStops[0][1], g_customStops[0][2], a);
    float t = pos * (n - 1);
    int i = (int)t; if (i > n - 2) i = n - 2;
    float f = t - i;
    return D2D1::ColorF(
        g_customStops[i][0] + (g_customStops[i + 1][0] - g_customStops[i][0]) * f,
        g_customStops[i][1] + (g_customStops[i + 1][1] - g_customStops[i][1]) * f,
        g_customStops[i][2] + (g_customStops[i + 1][2] - g_customStops[i][2]) * f, a);
}

struct Palette { const wchar_t* name; D2D1_COLOR_F (*color)(float, float); };
static const Palette PALETTES[] = {
    { L"ULTRAMARINE",   PalPhosphor },
    { L"GREEN PHOSPHOR",PalEmerald  },
    { L"AMBER CRT",     PalAmber    },
    { L"PAPER WHITE",   PalPaper    },
    { L"FULL SPECTRUM", PalSpectrum },
    { L"MAGMA",         PalMagma    },
};
static const int PAL_COUNT = (int)(sizeof(PALETTES) / sizeof(PALETTES[0]));

// --------------------------------------------------------------- DrawCtx

struct DrawCtx {
    ID2D1RenderTarget* rt;   // window or offscreen target (base interface)
    ID2D1Factory* factory;
    ID2D1SolidColorBrush* brush;
    D2D1_COLOR_F (*pal)(float, float);
    float w, h;
    const AudioFrame* f;

    void SetColor(float pos, float alpha) { brush->SetColor(pal(pos, alpha)); }
    void RawColor(D2D1_COLOR_F c) { brush->SetColor(c); }
    void FillRect(float x, float y, float rw, float rh) {
        rt->FillRectangle(D2D1::RectF(x, y, x + rw, y + rh), brush);
    }
    void Line(float x0, float y0, float x1, float y1, float width) {
        rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, width);
    }
    void FillCircle(float cx, float cy, float r) {
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush);
    }
    void StrokeCircle(float cx, float cy, float r, float width) {
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, width);
    }
    void FillPoly(const D2D1_POINT_2F* pts, UINT n) {
        if (n < 2) return;
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geo->Open(&sink))) { geo->Release(); return; }
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
        for (UINT i = 1; i < n; i++) sink->AddLine(pts[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close(); sink->Release();
        rt->FillGeometry(geo, brush);
        geo->Release();
    }
    void StrokePoly(const D2D1_POINT_2F* pts, UINT n, float width, bool closed) {
        if (n < 2) return;
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(geo->Open(&sink))) { geo->Release(); return; }
        sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        for (UINT i = 1; i < n; i++) sink->AddLine(pts[i]);
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close(); sink->Release();
        rt->DrawGeometry(geo, brush, width);
        geo->Release();
    }
    // Draw a CPU BGRA buffer scaled to the whole target (plasma, waterfall).
    void Blit(const uint8_t* bgra, UINT bw, UINT bh) {
        ID2D1Bitmap* bmp = nullptr;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(bw, bh), bgra, bw * 4, props, &bmp))) return;
        rt->DrawBitmap(bmp, D2D1::RectF(0, 0, w, h), 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        bmp->Release();
    }
};

struct Mode {
    const wchar_t* name;
    float fade; // background wash alpha per frame; 1 = full clear
    void (*draw)(DrawCtx&);
    bool flash;  // beat-burst mode; excluded by calm mode (default false)
};

struct Particle { float x, y, vx, vy, life, hue, size; };

// ===================================================================
// SPECTRUM family
// ===================================================================

static void DrawBars(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    int n = (int)(g.w / 28.0f); if (n < 24) n = 24; if (n > 96) n = 96;
    static float smooth[96] = {};
    float gap = g.w / n / 6.0f, bw = g.w / n - gap;
    for (int i = 0; i < n; i++) {
        float target = BinLog(f, (float)i / (n - 1));
        smooth[i] += (target - smooth[i]) * 0.65f;
        float bh = smooth[i] * g.h * 0.85f;
        g.SetColor((float)i / n, 0.95f);
        g.FillRect(i * (bw + gap) + gap / 2, g.h - bh, bw, bh);
    }
}

static void DrawMirrorBars(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    int n = (int)(g.w / 20.0f); if (n < 32) n = 32; if (n > 128) n = 128;
    static float b[128] = {}; Bands(f, b, n, 0.35f);
    float bw = g.w / n;
    for (int i = 0; i < n; i++) {
        float bh = powf(b[i] * g.h * 0.45f, 1.02f);
        g.SetColor(b[i], 0.9f);
        g.FillRect(i * bw + 1, g.h / 2 - bh, bw - 2, bh * 2);
    }
}

static void DrawRadial(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 96;
    static float smooth[n] = {};
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h;
    float r0 = mn * (0.18f + f.bass * 0.05f), rot = f.t * 0.15f;
    float lw = TAU * r0 / n / 2.4f; if (lw < 2) lw = 2;
    for (int i = 0; i < n; i++) {
        float target = BinLog(f, (float)i / (n - 1));
        smooth[i] += (target - smooth[i]) * 0.65f;
        float a = (float)i / n * TAU + rot, len = smooth[i] * mn * 0.3f;
        g.SetColor((float)i / n, 0.95f);
        g.Line(cx + cosf(a) * r0, cy + sinf(a) * r0,
               cx + cosf(a) * (r0 + len), cy + sinf(a) * (r0 + len), lw);
    }
}

static void DrawPeakLadder(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 48;
    static float b[n] = {}, peaks[n] = {};
    Bands(f, b, n, 0.35f);
    float bw = g.w / n, seg = g.h / 48.0f; if (seg < 6) seg = 6;
    for (int i = 0; i < n; i++) {
        peaks[i] = peaks[i] - f.dt * 0.25f; if (peaks[i] < b[i]) peaks[i] = b[i];
        int segs = (int)(b[i] * g.h * 0.85f / seg);
        for (int s = 0; s < segs; s++) {
            g.SetColor(s / (g.h / seg), 0.9f);
            g.FillRect(i * bw + 2, g.h - (s + 1) * seg + 1, bw - 4, seg - 2);
        }
        float py = g.h - peaks[i] * g.h * 0.85f;
        g.SetColor(1, 1);
        g.FillRect(i * bw + 2, py, bw - 4, seg / 4 > 2 ? seg / 4 : 2);
    }
}

static void DrawRibbon(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 128;
    static float b[n] = {}; Bands(f, b, n, 0.5f);
    for (int layer = 2; layer >= 0; layer--) {
        float scale = 0.9f - layer * 0.22f;
        D2D1_POINT_2F pts[n + 2];
        pts[0] = D2D1::Point2F(0, g.h);
        for (int i = 0; i < n; i++)
            pts[i + 1] = D2D1::Point2F((float)i / (n - 1) * g.w, g.h - b[i] * g.h * scale);
        pts[n + 1] = D2D1::Point2F(g.w, g.h);
        g.SetColor(layer / 2.5f, 0.35f + layer * 0.15f);
        g.FillPoly(pts, n + 2);
    }
}

static void DrawSkyline(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 20;
    static float b[n] = {}; Bands(f, b, n, 0.55f);
    float bw = g.w / n;
    for (int i = 0; i < n; i++) {
        float bh = b[i] * g.h * 0.8f, x = i * bw;
        g.SetColor((float)i / n, 0.25f); g.FillRect(x + 3, g.h - bh, bw - 6, bh);
        float cap = g.h * 0.006f; if (cap < 3) cap = 3;
        g.SetColor((float)i / n, 1); g.FillRect(x + 3, g.h - bh, bw - 6, cap);
        int rows = (int)(bh / (g.h * 0.04f));
        for (int r = 0; r < rows; r += 2) {
            g.SetColor(1 - (float)i / n, 0.5f);
            float wy = g.h - bh + r * g.h * 0.04f + 6;
            g.FillRect(x + bw * 0.25f, wy, bw * 0.1f, 4);
            g.FillRect(x + bw * 0.6f, wy, bw * 0.1f, 4);
        }
    }
}

static void DrawWaterfall(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const UINT W = 512, H = 288;
    static uint8_t buf[W * H * 4] = {};
    // Scroll down one row.
    memmove(buf + W * 4, buf, W * 4 * (H - 1));
    for (UINT i = 0; i < W; i++) {
        float m = BinLog(f, (float)i / W);
        D2D1_COLOR_F c = m < 0.02f ? D2D1::ColorF(0.043f, 0.051f, 0.047f)
                                   : g.pal(m, 1.0f);
        uint8_t* px = buf + i * 4;
        px[0] = (uint8_t)(c.b * 255); px[1] = (uint8_t)(c.g * 255);
        px[2] = (uint8_t)(c.r * 255); px[3] = 255;
    }
    g.Blit(buf, W, H);
}

static void DrawDualArc(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 64;
    static float b[n] = {}; Bands(f, b, n, 0.35f);
    float cx = g.w / 2, cy = g.h * 0.55f, mn = g.w < g.h ? g.w : g.h, R = mn * 0.42f;
    float lw = R / 90.0f; if (lw < 2) lw = 2;
    for (int i = 0; i < n; i++) {
        float spread = (float)i / n * PI * 0.85f + PI * 0.075f;
        float len = R * (0.25f + b[i] * 0.75f);
        for (int d = 0; d < 2; d++) {
            float dir = d ? 1.0f : -1.0f, a = PI / 2 + dir * spread;
            g.SetColor((float)i / n, 0.85f);
            g.Line(cx, cy, cx + cosf(a) * len, cy - fabsf(sinf(a)) * len, lw);
        }
    }
}

static void DrawDotMatrix(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int cols = 48, rows = 24;
    static float b[cols] = {}; Bands(f, b, cols, 0.35f);
    float cw = g.w / cols, rh = g.h / rows, r = (cw < rh ? cw : rh) * 0.28f;
    for (int i = 0; i < cols; i++) {
        int lit = (int)(b[i] * rows + 0.5f);
        for (int j = 0; j < rows; j++) {
            bool on = j < lit;
            g.SetColor(on ? (float)j / rows : 0.0f, on ? 0.95f : 0.1f);
            g.FillCircle(i * cw + cw / 2, g.h - (j + 0.5f) * rh, on ? r : r * 0.6f);
        }
    }
}

// ===================================================================
// WAVEFORM family
// ===================================================================

static void DrawScope(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    g.SetColor(0.8f, 0.95f);
    float lw = g.h / 320.0f; if (lw < 2) lw = 2;
    D2D1_POINT_2F pts[256]; int m = 0;
    for (int i = 0; i < 2048; i += 8)
        pts[m++] = D2D1::Point2F((float)i / 2047.0f * g.w, g.h / 2 + f.wave[i] * g.h * 0.4f);
    g.StrokePoly(pts, m, lw, false);
}

static void DrawRingScope(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h;
    float R = mn * (0.28f + f.bass * 0.08f), lw = R / 90.0f; if (lw < 2) lw = 2;
    g.SetColor(0.75f, 0.95f);
    D2D1_POINT_2F pts[260]; int m = 0;
    for (int i = 0; i <= 2048; i += 8) {
        float s = f.wave[i % 2048];
        float a = (float)i / 2048 * TAU + f.t * 0.1f, rr = R + s * R * 0.55f;
        pts[m++] = D2D1::Point2F(cx + cosf(a) * rr, cy + sinf(a) * rr);
    }
    g.StrokePoly(pts, m, lw, false);
}

static void DrawLissajous(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, S = mn * 0.42f;
    const int delay = 180; float lw = S / 260.0f; if (lw < 1.5f) lw = 1.5f;
    g.SetColor(0.85f, 0.8f);
    static D2D1_POINT_2F pts[1100]; int m = 0;
    for (int i = 0; i < 2048 - delay && m < 1100; i += 2)
        pts[m++] = D2D1::Point2F(cx + f.wave[i] * S, cy + f.wave[i + delay] * S);
    g.StrokePoly(pts, m, lw, false);
}

static void DrawBraid(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    float lw = g.h / 400.0f; if (lw < 1.5f) lw = 1.5f;
    for (int k = 0; k < 3; k++) {
        int shift = k * 340; float off = (k - 1) * g.h * 0.05f;
        g.SetColor(0.35f + k * 0.3f, 0.85f);
        static D2D1_POINT_2F pts[1100]; int m = 0;
        for (int i = 0; i < 2048 - shift && m < 1100; i += 2) {
            float x = (float)i / 2047.0f * g.w, env = sinf((float)i / 2048 * PI);
            pts[m++] = D2D1::Point2F(x, g.h / 2 + off + f.wave[i + shift] * g.h * 0.35f * env);
        }
        g.StrokePoly(pts, m, lw, false);
    }
}

static void DrawRidgeline(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int ROWS = 36, N = 160;
    static float hist[ROWS][N] = {}; static int count = 0;
    // Shift history down, insert newest at row 0.
    for (int r = (count < ROWS ? count : ROWS - 1); r > 0; r--)
        memcpy(hist[r], hist[r - 1], sizeof(float) * N);
    for (int i = 0; i < N; i++)
        hist[0][i] = f.wave[(int)((float)i / N * 2048)];
    if (count < ROWS) count++;

    float mx = g.w * 0.18f, my = g.h * 0.12f, rh = (g.h - my * 2) / ROWS;
    float lw = g.h / 800.0f; if (lw < 1.2f) lw = 1.2f;
    for (int r = count - 1; r >= 0; r--) {
        float y0 = my + r * rh, env = sinf((float)(r + 1) / ROWS * PI) * 0.4f + 0.6f;
        D2D1_POINT_2F pts[N + 3]; int m = 0;
        pts[m++] = D2D1::Point2F(mx, y0);
        for (int i = 0; i < N; i++) {
            float x = mx + (float)i / (N - 1) * (g.w - mx * 2);
            float bell = sinf((float)i / (N - 1) * PI); bell *= bell;
            pts[m++] = D2D1::Point2F(x, y0 - fabsf(hist[r][i]) * rh * 9 * bell * env);
        }
        pts[m++] = D2D1::Point2F(g.w - mx, y0 + rh);
        pts[m++] = D2D1::Point2F(mx, y0 + rh);
        g.RawColor(D2D1::ColorF(0.043f, 0.051f, 0.047f));
        g.FillPoly(pts, m);
        g.SetColor(1 - (float)r / ROWS, 0.9f);
        g.StrokePoly(pts, m, lw, true);
    }
}

static void DrawStrings(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 24; float gap = g.w / (n + 1);
    float lw = g.w / 1200.0f; if (lw < 1.5f) lw = 1.5f;
    for (int s = 0; s < n; s++) {
        float x0 = gap * (s + 1); int phase = (int)((float)s / n * 1024);
        g.SetColor((float)s / n, 0.85f);
        D2D1_POINT_2F pts[41]; int m = 0;
        for (int i = 0; i <= 40; i++) {
            float yt = (float)i / 40, bell = sinf(yt * PI);
            float sample = f.wave[(phase + i * 24) % 2048];
            pts[m++] = D2D1::Point2F(x0 + sample * gap * 1.4f * bell, yt * g.h);
        }
        g.StrokePoly(pts, m, lw, false);
    }
}

// ===================================================================
// PARTICLES family
// ===================================================================

static void DrawNebula(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 420;
    static std::vector<Particle> ps; static bool init = false;
    if (!init) { init = true; Rng r(7); ps.resize(n);
        for (int i = 0; i < n; i++) { ps[i].x = r() * TAU; ps[i].y = 0.25f + r() * 0.75f;
            ps[i].vx = 0.2f + r() * 0.8f; ps[i].hue = (float)i / n; ps[i].size = 1 + r() * 2.2f; } }
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, R = mn * 0.44f;
    for (auto& p : ps) {
        p.x += f.dt * p.vx * (0.25f + f.mid * 2.2f);
        float swell = 1 + f.bass * 0.5f + f.beat * 0.25f, rr = p.y * R * swell;
        g.SetColor(p.hue, 0.35f + f.level);
        g.FillCircle(cx + cosf(p.x) * rr, cy + sinf(p.x) * rr * 0.72f,
                     p.size * (g.h / 900) * (1 + f.beat));
    }
}

static void DrawFountain(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 500;
    static std::vector<Particle> ps; static bool init = false; static int cursor = 0;
    if (!init) { init = true; Rng r(11); ps.resize(n); for (auto& p : ps) { p.life = 0; p.size = 1 + r() * 2; } }
    int burst = f.beat > 0.6f ? 26 : (int)(f.level * 6);
    for (int k = 0; k < burst; k++) {
        Particle& p = ps[cursor % n]; cursor++;
        p.x = g.w / 2 + (frand() - 0.5f) * g.w * 0.08f; p.y = g.h * 0.92f;
        float a = -PI / 2 + (frand() - 0.5f) * 0.9f;
        float speed = (0.55f + frand() * 0.6f) * g.h * (0.9f + f.bass);
        p.vx = cosf(a) * speed; p.vy = sinf(a) * speed; p.life = 1; p.hue = frand();
    }
    for (auto& p : ps) {
        if (p.life <= 0) continue;
        p.life -= f.dt * 0.55f; p.vy += g.h * 1.1f * f.dt; p.x += p.vx * f.dt; p.y += p.vy * f.dt;
        g.SetColor(p.hue, p.life > 0 ? p.life : 0);
        g.FillCircle(p.x, p.y, p.size * (g.h / 800));
    }
}

static void DrawStarfield(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 420;
    static std::vector<Particle> ps; static bool init = false;
    if (!init) { init = true; Rng r(23); ps.resize(n);
        for (auto& p : ps) { p.x = (r() - 0.5f) * 2; p.y = (r() - 0.5f) * 2;
            p.life = 0.05f + r() * 0.95f; p.hue = r(); p.size = 0.6f + r() * 1.6f; } }
    float cx = g.w / 2, cy = g.h / 2, speed = 0.12f + f.level * 2.4f + f.beat * 0.8f;
    for (auto& p : ps) {
        p.life -= f.dt * speed;
        if (p.life <= 0.02f) { p.x = (frand() - 0.5f) * 2; p.y = (frand() - 0.5f) * 2; p.life = 1; }
        float z = p.life, px = cx + (p.x / z) * cx * 0.9f, py = cy + (p.y / z) * cy * 0.9f;
        float pz = (1 - z) * 2; if (pz > 1.5f) pz = 1.5f;
        g.SetColor(p.hue, pz > 1 ? 1 : pz);
        g.FillCircle(px, py, p.size * pz * (g.h / 700));
    }
}

static void DrawSwarm(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 260;
    static std::vector<Particle> ps; static bool init = false;
    if (!init) { init = true; Rng r(31); ps.resize(n);
        for (auto& p : ps) { p.x = r() * g.w; p.y = r() * g.h; p.hue = r(); p.size = 1 + r() * 1.8f; } }
    float tx = g.w / 2 + cosf(f.t * 0.6f) * g.w * 0.22f;
    float ty = g.h / 2 + sinf(f.t * 0.83f) * g.h * 0.22f;
    float agit = 40 + f.treble * 900;
    for (auto& p : ps) {
        p.vx += ((tx - p.x) * 1.1f + (frand() - 0.5f) * agit) * f.dt;
        p.vy += ((ty - p.y) * 1.1f + (frand() - 0.5f) * agit) * f.dt;
        p.vx *= 0.985f; p.vy *= 0.985f; p.x += p.vx * f.dt * 3; p.y += p.vy * f.dt * 3;
        g.SetColor(p.hue, 0.6f);
        g.FillCircle(p.x, p.y, p.size * (g.h / 900) * (1 + f.beat * 0.8f));
    }
}

static void DrawOrbitals(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int shells = 7;
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, R = mn * 0.46f;
    for (int s = 0; s < shells; s++) {
        int idx = (int)((float)s / shells * 512);
        float e = f.fft[idx];
        float r0 = R * (float)(s + 1) / shells; int cnt = 6 + s * 4;
        for (int i = 0; i < cnt; i++) {
            float a = (float)i / cnt * TAU + f.t * (0.3f + s * 0.12f) * (s % 2 ? -1 : 1);
            float wob = 1 + e * 0.18f * sinf(a * 5 + f.t * 3);
            g.SetColor((float)s / shells, 0.3f + e * 0.7f);
            g.FillCircle(cx + cosf(a) * r0 * wob, cy + sinf(a) * r0 * wob * 0.85f,
                         (g.h / 340) * (0.5f + e * 1.6f));
        }
    }
}

static void DrawComets(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 7;
    for (int i = 0; i < n; i++) {
        float speed = 0.35f + f.level * 1.8f, t = f.t * speed + (float)i * TAU / n;
        float x = g.w / 2 + sinf(t * 1.3f + i) * cosf(t * 0.7f) * g.w * 0.4f;
        float y = g.h / 2 + cosf(t * 1.7f + i * 2) * sinf(t * 0.9f) * g.h * 0.4f;
        g.SetColor((float)i / n, 0.95f);
        g.FillCircle(x, y, (g.h / 300) * (1 + f.beat));
    }
}

// ===================================================================
// GEOMETRY family
// ===================================================================

static void DrawPolygonMorph(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, R = mn * 0.4f;
    int sides = 3 + (int)(f.mid * 6 + 0.5f);
    for (int ring = 6; ring >= 1; ring--) {
        float rr = R * (ring / 6.0f) * (1 + f.beat * 0.12f);
        float rot = f.t * 0.25f * (ring % 2 ? 1 : -1);
        float lw = R / 110.0f; if (lw < 1.5f) lw = 1.5f;
        g.SetColor(ring / 6.0f, 0.75f);
        D2D1_POINT_2F pts[40]; int m = 0;
        for (int i = 0; i <= sides && m < 40; i++) {
            float a = (float)i / sides * TAU + rot;
            pts[m++] = D2D1::Point2F(cx + cosf(a) * rr, cy + sinf(a) * rr);
        }
        g.StrokePoly(pts, m, lw, true);
    }
}

static void DrawTunnel(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    static float rings[5] = { 0.1f, 0.3f, 0.5f, 0.7f, 0.9f };
    float cx = g.w / 2, cy = g.h / 2, R = sqrtf(g.w * g.w + g.h * g.h) / 2;
    float speed = 0.12f + f.level * 0.7f + f.beat * 0.4f, lw = g.h / 300.0f; if (lw < 2) lw = 2;
    float squash = 0.75f + f.bass * 0.2f;
    for (int i = 0; i < 5; i++) {
        rings[i] += f.dt * speed; if (rings[i] > 1) rings[i] -= 1;
        float e = powf(rings[i], 2.2f);
        g.SetColor(1 - rings[i], e * 3 > 1 ? 1 : e * 3);
        D2D1::Matrix3x2F m = D2D1::Matrix3x2F::Rotation(f.t * 0.1f * 180.0f / PI, D2D1::Point2F(cx, cy));
        g.rt->SetTransform(m);
        g.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), e * R, e * R * squash), g.brush, lw);
        g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

static void DrawKaleido(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int seg = 10;
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, R = mn * 0.46f;
    float lw = R / 200.0f; if (lw < 1.5f) lw = 1.5f;
    for (int s = 0; s < seg; s++) {
        D2D1::Matrix3x2F rot = D2D1::Matrix3x2F::Rotation((float)s / seg * 360.0f + f.t * 0.12f * 180.0f / PI);
        D2D1::Matrix3x2F flip = (s % 2) ? D2D1::Matrix3x2F::Scale(1, -1) : D2D1::Matrix3x2F::Identity();
        g.rt->SetTransform(flip * rot * D2D1::Matrix3x2F::Translation(cx, cy));
        g.SetColor((float)s / seg, 0.8f);
        D2D1_POINT_2F pts[64]; int m = 0;
        for (int i = 0; i < 64; i++) {
            float q = (float)i / 63, sample = f.wave[(int)(q * 2047)];
            pts[m++] = D2D1::Point2F(q * R, sample * R * 0.3f * sinf(q * PI));
        }
        g.StrokePoly(pts, m, lw, false);
    }
    g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

static void DrawSpiro(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, R = mn * 0.38f;
    float k = 0.25f + f.mid * 0.5f, l = 0.5f + f.treble * 0.45f, t0 = f.t * (0.8f + f.level * 2);
    float lw = R / 220.0f; if (lw < 1.5f) lw = 1.5f;
    g.SetColor(fmodf(f.t * 0.06f, 1.0f), 0.85f);
    D2D1_POINT_2F pts[141]; int m = 0;
    for (int i = 0; i <= 140; i++) {
        float t = t0 + i * 0.045f;
        pts[m++] = D2D1::Point2F(
            cx + R * ((1 - k) * cosf(t) + l * k * cosf(((1 - k) / k) * t)),
            cy + R * ((1 - k) * sinf(t) - l * k * sinf(((1 - k) / k) * t)));
    }
    g.StrokePoly(pts, m, lw, false);
}

static void DrawShards(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    struct Shard { float a, d, vd, rot, vr, size, life, hue; };
    static std::vector<Shard> shards;
    float mn = g.w < g.h ? g.w : g.h, cx = g.w / 2, cy = g.h / 2;
    if (f.beat > 0.7f && shards.size() < 140) {
        for (int k = 0; k < 16; k++)
            shards.push_back({ frand() * TAU, mn * 0.05f, (0.25f + frand() * 0.5f) * mn,
                frand() * TAU, (frand() - 0.5f) * 6, (0.02f + frand() * 0.045f) * mn, 1, frand() });
    }
    float lw = g.h / 500.0f; if (lw < 1.5f) lw = 1.5f;
    for (int i = (int)shards.size() - 1; i >= 0; i--) {
        Shard& s = shards[i];
        s.life -= f.dt * 0.7f;
        if (s.life <= 0) { shards.erase(shards.begin() + i); continue; }
        s.d += s.vd * f.dt; s.rot += s.vr * f.dt;
        float x = cx + cosf(s.a) * s.d, y = cy + sinf(s.a) * s.d;
        g.rt->SetTransform(D2D1::Matrix3x2F::Rotation(s.rot * 180.0f / PI) *
                           D2D1::Matrix3x2F::Translation(x, y));
        g.SetColor(s.hue, s.life);
        D2D1_POINT_2F pts[3] = { D2D1::Point2F(-s.size / 2, s.size / 2),
            D2D1::Point2F(0, -s.size / 2), D2D1::Point2F(s.size / 2, s.size / 2) };
        g.StrokePoly(pts, 3, lw, true);
    }
    g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g.SetColor(0.5f, 0.25f + f.level);
    g.FillCircle(cx, cy, mn * 0.012f * (1 + f.bass * 2));
}

static void DrawLattice(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int rows = 12;
    static float b[rows] = {}; Bands(f, b, rows, 0.35f);
    float cx = g.w / 2, cy = g.h / 2, mn = g.w < g.h ? g.w : g.h, R = mn * 0.5f;
    float lw = R / 260.0f; if (lw < 1.2f) lw = 1.2f;
    g.rt->SetTransform(D2D1::Matrix3x2F::Rotation(f.t * 0.08f * 180.0f / PI, D2D1::Point2F(cx, cy)));
    for (int i = 0; i < rows; i++) {
        float q = (float)i / (rows - 1), y = (q - 0.5f) * R * 1.6f, stretch = 0.5f + b[i] * 0.9f;
        g.SetColor(q, 0.55f + b[i] * 0.45f);
        g.Line(cx - R * stretch, cy + y, cx + R * stretch, cy + y, lw);
        g.Line(cx + y, cy - R * stretch, cx + y, cy + R * stretch, lw);
    }
    g.rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

// ===================================================================
// FIELD family
// ===================================================================

static void DrawPlasma(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const UINT W = 160, H = 90;
    static uint8_t buf[W * H * 4];
    static uint8_t lut[256 * 3]; static void* lutPal = nullptr;
    if (lutPal != (void*)g.pal) {
        lutPal = (void*)g.pal;
        for (int i = 0; i < 256; i++) {
            D2D1_COLOR_F c = g.pal(i / 255.0f, 1.0f);
            lut[i * 3] = (uint8_t)(c.r * 255); lut[i * 3 + 1] = (uint8_t)(c.g * 255); lut[i * 3 + 2] = (uint8_t)(c.b * 255);
        }
    }
    float t = f.t * (0.4f + f.level * 1.6f), boost = 0.5f + f.level * 1.6f;
    for (UINT j = 0; j < H; j++) {
        for (UINT i = 0; i < W; i++) {
            float u = (float)i / W, q = (float)j / H;
            float s = sinf(u * 7 + t) + sinf((u + q) * 5 - t * 1.3f) +
                      sinf(sqrtf((u - 0.5f) * (u - 0.5f) + (q - 0.5f) * (q - 0.5f)) * 14 - t * 2);
            float m = clampf((s / 3 + 0.5f) * boost, 0, 1);
            int li = (int)(m * 255) * 3;
            uint8_t* px = buf + (j * W + i) * 4;
            px[0] = lut[li + 2]; px[1] = lut[li + 1]; px[2] = lut[li]; px[3] = 255;
        }
    }
    g.Blit(buf, W, H);
}

static void DrawInterference(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    struct Src { float px, py, e; };
    Src srcs[3] = { { 0.3f, 0.4f, f.bass }, { 0.7f, 0.35f, f.mid }, { 0.5f, 0.7f, f.treble } };
    float mn = g.w < g.h ? g.w : g.h, lw = g.h / 500.0f; if (lw < 1.5f) lw = 1.5f;
    for (int s = 0; s < 3; s++) {
        float cx = srcs[s].px * g.w, cy = srcs[s].py * g.h, e = srcs[s].e;
        for (int i = 0; i < 9; i++) {
            float phase = fmodf(f.t * (0.3f + e * 1.2f) + (float)i / 9, 1.0f);
            float r = phase * mn * 0.6f;
            g.SetColor(s / 2.2f, (1 - phase) * (0.15f + e));
            g.StrokeCircle(cx, cy, r, lw);
        }
    }
}

static void DrawFlowField(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int n = 900;
    static std::vector<Particle> gr; static bool init = false;
    if (!init) { init = true; Rng r(41); gr.resize(n); for (auto& p : gr) { p.x = r(); p.y = r(); p.hue = r(); } }
    float scale = 3.2f, speed = (0.04f + f.level * 0.35f + f.beat * 0.1f) * f.dt * 60;
    float size = g.h / 720.0f; if (size < 1) size = 1;
    for (auto& p : gr) {
        float a = Noise2(p.x * scale, p.y * scale + f.t * 0.12f) * TAU * 2 + f.mid * 3;
        p.x += cosf(a) * speed / 100; p.y += sinf(a) * speed / 100;
        if (p.x < 0 || p.x > 1 || p.y < 0 || p.y > 1) { p.x = frand(); p.y = frand(); }
        g.SetColor(p.hue, 0.55f);
        g.FillRect(p.x * g.w, p.y * g.h, size, size);
    }
}

static void DrawAurora(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int layers = 5;
    for (int l = 0; l < layers; l++) {
        float q = (float)l / (layers - 1), yBase = g.h * (0.25f + q * 0.4f);
        float amp = g.h * (0.06f + f.mid * 0.22f) * (1 - q * 0.4f);
        D2D1_POINT_2F pts[93]; int m = 0;
        pts[m++] = D2D1::Point2F(0, g.h);
        for (int i = 0; i <= 90; i++) {
            float u = (float)i / 90;
            float y = yBase + sinf(u * 5 + f.t * (0.5f + l * 0.17f)) * amp +
                      sinf(u * 13 - f.t * 0.9f) * amp * 0.35f + BinLog(f, u) * -g.h * 0.12f;
            pts[m++] = D2D1::Point2F(u * g.w, y);
        }
        pts[m++] = D2D1::Point2F(g.w, g.h);
        g.SetColor(0.25f + q * 0.6f, 0.16f + f.level * 0.2f);
        g.FillPoly(pts, m);
    }
}

static void DrawRain(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    int cols = (int)(g.w / 26.0f); if (cols < 24) cols = 24; if (cols > 72) cols = 72;
    static float drops[72]; static int inited = 0;
    if (inited != cols) { inited = cols; for (int i = 0; i < cols; i++) drops[i] = frand(); }
    float cw = g.w / cols, dash = g.h / 60.0f; if (dash < 6) dash = 6;
    for (int i = 0; i < cols; i++) {
        float e = BinLog(f, (float)i / cols);
        drops[i] += f.dt * (0.08f + e * 1.4f); if (drops[i] > 1.2f) drops[i] = -0.2f;
        float y = drops[i] * g.h;
        for (int k = 0; k < 7; k++) {
            float yy = y - k * dash * 1.6f;
            if (yy < -dash || yy > g.h) continue;
            g.SetColor(e, (1 - (float)k / 7) * (0.3f + e * 0.7f));
            g.FillRect(i * cw + cw * 0.35f, yy, cw * 0.3f, dash);
        }
    }
}

static void DrawPulseRings(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    struct Ring { float x, y, r, life, hue; };
    static std::vector<Ring> rings; static bool wasBeat = false;
    float mn = g.w < g.h ? g.w : g.h;
    if (f.beat > 0.7f && !wasBeat && rings.size() < 24)
        rings.push_back({ g.w * (0.2f + frand() * 0.6f), g.h * (0.2f + frand() * 0.6f), 0, 1, frand() });
    wasBeat = f.beat > 0.7f;
    float grow = mn * 0.55f;
    for (int i = (int)rings.size() - 1; i >= 0; i--) {
        Ring& ring = rings[i];
        ring.life -= f.dt * 0.5f; ring.r += grow * f.dt;
        if (ring.life <= 0) { rings.erase(rings.begin() + i); continue; }
        float lw = (g.h / 260.0f); if (lw < 2) lw = 2; lw *= ring.life;
        g.SetColor(ring.hue, ring.life * 0.9f);
        g.StrokeCircle(ring.x, ring.y, ring.r, lw);
    }
    float lw2 = g.h / 400.0f; if (lw2 < 1.5f) lw2 = 1.5f;
    g.SetColor(0.5f, 0.2f + f.level * 0.5f);
    g.StrokeCircle(g.w / 2, g.h / 2, mn * (0.05f + f.bass * 0.12f), lw2);
}

static void DrawSunburst(DrawCtx& g) {
    const AudioFrame& f = *g.f;
    const int rays = 48;
    float cx = g.w / 2, cy = g.h / 2, R = sqrtf(g.w * g.w + g.h * g.h) * 0.6f;
    for (int i = 0; i < rays; i++) {
        float q = (float)i / rays, e = BinLog(f, q), a = q * TAU + f.t * 0.1f;
        float width = (TAU / rays) * (0.15f + f.bass * 0.5f + e * 0.3f);
        float rr = R * (0.3f + e * 0.7f);
        g.SetColor(q, 0.12f + e * 0.55f);
        D2D1_POINT_2F pts[8]; int m = 0;
        pts[m++] = D2D1::Point2F(cx, cy);
        for (int k = 0; k <= 5; k++) {
            float aa = a - width / 2 + width * (k / 5.0f);
            pts[m++] = D2D1::Point2F(cx + cosf(aa) * rr, cy + sinf(aa) * rr);
        }
        g.FillPoly(pts, m);
    }
}

// --------------------------------------------------------------- registry

static const Mode MODES[] = {
    // spectrum
    { L"BARS",          1.00f, DrawBars       },
    { L"MIRROR BARS",   1.00f, DrawMirrorBars },
    { L"RADIAL BARS",   0.30f, DrawRadial     },
    { L"PEAK LADDER",   1.00f, DrawPeakLadder },
    { L"RIBBON",        1.00f, DrawRibbon     },
    { L"SKYLINE",       1.00f, DrawSkyline    },
    { L"WATERFALL",     1.00f, DrawWaterfall  },
    { L"DUAL ARC",      1.00f, DrawDualArc    },
    { L"DOT MATRIX",    1.00f, DrawDotMatrix  },
    // waveform
    { L"OSCILLOSCOPE",  0.35f, DrawScope      },
    { L"RING SCOPE",    0.30f, DrawRingScope  },
    { L"LISSAJOUS",     0.18f, DrawLissajous  },
    { L"BRAID",         0.40f, DrawBraid      },
    { L"RIDGELINE",     1.00f, DrawRidgeline  },
    { L"STRINGS",       0.25f, DrawStrings    },
    // particles
    { L"NEBULA",        0.12f, DrawNebula     },
    { L"FOUNTAIN",      0.16f, DrawFountain,   true },
    { L"STARFIELD",     0.30f, DrawStarfield,  true },
    { L"SWARM",         0.14f, DrawSwarm      },
    { L"ORBITALS",      0.20f, DrawOrbitals   },
    { L"COMETS",        0.045f,DrawComets     },
    // geometry
    { L"POLYGON MORPH", 0.22f, DrawPolygonMorph },
    { L"TUNNEL",        0.25f, DrawTunnel,    true },
    { L"KALEIDOSCOPE",  0.14f, DrawKaleido    },
    { L"SPIROGRAPH",    0.06f, DrawSpiro      },
    { L"SHARDS",        0.13f, DrawShards,    true },
    { L"LATTICE",       0.30f, DrawLattice    },
    // field
    { L"PLASMA",        1.00f, DrawPlasma     },
    { L"INTERFERENCE",  0.50f, DrawInterference },
    { L"FLOW FIELD",    0.055f,DrawFlowField  },
    { L"AURORA",        1.00f, DrawAurora     },
    { L"SIGNAL RAIN",   0.22f, DrawRain       },
    { L"PULSE RINGS",   0.09f, DrawPulseRings,true },
    { L"SUNBURST",      0.16f, DrawSunburst   },
};
static const int MODE_COUNT = (int)(sizeof(MODES) / sizeof(MODES[0]));
