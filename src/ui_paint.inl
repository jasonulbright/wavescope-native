// One UI, every engine. The HUD, control deck, and help overlay are drawn
// against this abstract painter so the exact same layout code runs whether the
// frame underneath came from Direct2D (built-in modes) or OpenGL (projectM /
// GPU shaders). D2DPainter is the one backend: the built-in modes draw it
// straight onto the window target, while the GL engines render the same UI
// into a WIC bitmap and composite it as a texture over their frame — crisp
// DirectWrite everywhere, and the controls never vanish on engine switch.

struct UIPainter {
    virtual ~UIPainter() {}
    virtual float W() = 0;
    virtual float H() = 0;
    // Filled rectangle in pixels; `round` is a hint honored by D2D, ignored by GL.
    virtual void Rect(float x, float y, float w, float h,
                      float r, float g, float b, float a, bool round = false) = 0;
    // Outline-only rectangle (transparent interior) — for the web-style deck.
    virtual void Stroke(float x, float y, float w, float h,
                        float r, float g, float b, float a,
                        float thickness = 1.0f, bool round = false) = 0;
    // Text in the box (x,y,w,h). `center` centers horizontally+vertically
    // (deck buttons); otherwise top-left with \n line breaks (HUD, help).
    // `tiny` uses the small label font (group descriptors).
    virtual void Text(float x, float y, float w, float h, const wchar_t* s,
                      float r, float g, float b, float a,
                      bool center = false, bool tiny = false) = 0;
};

// Direct2D backend: used for the built-in modes' frame. `scale` is the
// monitor DPI factor: the caller sets a matching scale transform on the
// target, so layout code sees logical (96-DPI) dimensions here while text
// and geometry rasterize at full physical resolution.
struct D2DPainter : UIPainter {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* br;
    IDWriteTextFormat* leftFont;    // top-left, multi-line (HUD, help)
    IDWriteTextFormat* centerFont;  // centered (deck buttons)
    IDWriteTextFormat* tinyFont;    // small right-aligned (group descriptors)
    float scale;

    D2DPainter(ID2D1RenderTarget* r, ID2D1SolidColorBrush* b,
               IDWriteTextFormat* lf, IDWriteTextFormat* cf, IDWriteTextFormat* tf = nullptr,
               float sc = 1.0f)
        : rt(r), br(b), leftFont(lf), centerFont(cf), tinyFont(tf), scale(sc > 0 ? sc : 1.0f) {}

    float W() override { return rt->GetSize().width / scale; }
    float H() override { return rt->GetSize().height / scale; }

    void Rect(float x, float y, float w, float h,
              float r, float g, float b, float a, bool round) override {
        br->SetColor(D2D1::ColorF(r, g, b, a));
        D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
        if (round) { D2D1_ROUNDED_RECT rr = { rc, 5, 5 }; rt->FillRoundedRectangle(rr, br); }
        else rt->FillRectangle(rc, br);
    }

    void Stroke(float x, float y, float w, float h,
                float r, float g, float b, float a, float thickness, bool round) override {
        br->SetColor(D2D1::ColorF(r, g, b, a));
        // Inset by half the stroke so the outline sits fully inside the button box.
        float o = thickness * 0.5f;
        D2D1_RECT_F rc = D2D1::RectF(x + o, y + o, x + w - o, y + h - o);
        if (round) { D2D1_ROUNDED_RECT rr = { rc, 5, 5 }; rt->DrawRoundedRectangle(rr, br, thickness); }
        else rt->DrawRectangle(rc, br, thickness);
    }

    void Text(float x, float y, float w, float h, const wchar_t* s,
              float r, float g, float b, float a, bool center, bool tiny) override {
        br->SetColor(D2D1::ColorF(r, g, b, a));
        IDWriteTextFormat* f = (tiny && tinyFont) ? tinyFont : (center ? centerFont : leftFont);
        rt->DrawTextW(s, (UINT32)wcslen(s), f, D2D1::RectF(x, y, x + w, y + h), br);
    }
};
