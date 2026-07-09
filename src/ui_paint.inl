// One UI, every engine. The HUD, control deck, and help overlay are drawn
// against this abstract painter so the exact same layout code runs whether the
// frame underneath came from Direct2D (built-in modes) or OpenGL (projectM /
// GPU shaders). Two backends implement it: D2DPainter draws into the D2D
// target; GLPainter (in main.cpp, after the GL headers) draws with legacy GL
// over the child window. That kills the old "the deck disappears in MilkDrop"
// split — the controls now live on top of all three engines identically.

struct UIPainter {
    virtual ~UIPainter() {}
    virtual float W() = 0;
    virtual float H() = 0;
    // Filled rectangle in pixels; `round` is a hint honored by D2D, ignored by GL.
    virtual void Rect(float x, float y, float w, float h,
                      float r, float g, float b, float a, bool round = false) = 0;
    // Text in the box (x,y,w,h). `center` centers horizontally+vertically
    // (deck buttons); otherwise top-left with \n line breaks (HUD, help).
    virtual void Text(float x, float y, float w, float h, const wchar_t* s,
                      float r, float g, float b, float a, bool center = false) = 0;
};

// Direct2D backend: used for the built-in modes' frame.
struct D2DPainter : UIPainter {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* br;
    IDWriteTextFormat* leftFont;    // top-left, multi-line (HUD, help)
    IDWriteTextFormat* centerFont;  // centered (deck buttons)

    D2DPainter(ID2D1RenderTarget* r, ID2D1SolidColorBrush* b,
               IDWriteTextFormat* lf, IDWriteTextFormat* cf)
        : rt(r), br(b), leftFont(lf), centerFont(cf) {}

    float W() override { return rt->GetSize().width; }
    float H() override { return rt->GetSize().height; }

    void Rect(float x, float y, float w, float h,
              float r, float g, float b, float a, bool round) override {
        br->SetColor(D2D1::ColorF(r, g, b, a));
        D2D1_RECT_F rc = D2D1::RectF(x, y, x + w, y + h);
        if (round) { D2D1_ROUNDED_RECT rr = { rc, 6, 6 }; rt->FillRoundedRectangle(rr, br); }
        else rt->FillRectangle(rc, br);
    }

    void Text(float x, float y, float w, float h, const wchar_t* s,
              float r, float g, float b, float a, bool center) override {
        br->SetColor(D2D1::ColorF(r, g, b, a));
        rt->DrawTextW(s, (UINT32)wcslen(s), center ? centerFont : leftFont,
                      D2D1::RectF(x, y, x + w, y + h), br);
    }
};
