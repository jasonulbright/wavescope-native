// GPU shader host: runs true per-pixel fragment-shader visuals on the GPU,
// the native counterpart of the web build's WebGPU engine (see the frozen
// wavescope webgpu.ts). Toggled with the G key. Like the projectM host it
// lives in a child window with its own WGL/OpenGL 3.3 (compatibility) context
// layered over the Direct2D surface, so the same bitmap-font HUD draws over
// it. Unlike projectM this pulls in NO third-party code: the three shaders are
// GLSL 330 ports of the frozen WGSL, compiled at runtime. Pure GPU + OS.
//
// The WGSL->GLSL port is 1:1 on the math; only the boilerplate differs
// (uniform block -> individual uniforms, gl_VertexID fullscreen triangle,
// atan2 -> atan). Keep the three fragment bodies in sync with webgpu.ts.

#include <GL/gl.h>
#include <string>

#pragma comment(lib, "opengl32.lib")

// WGL + GL entry points. Identical typedef/macro redefinitions across this and
// projectm_host.inl are legal in one TU, so no guards are needed.
typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int);
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// GL 2.0+ / 3.0 functions resolved at runtime (not in opengl32's 1.1 exports).
typedef GLuint(WINAPI* PFN_glCreateShader)(GLenum);
typedef void  (WINAPI* PFN_glShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
typedef void  (WINAPI* PFN_glCompileShader)(GLuint);
typedef void  (WINAPI* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);
typedef void  (WINAPI* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
typedef void  (WINAPI* PFN_glDeleteShader)(GLuint);
typedef GLuint(WINAPI* PFN_glCreateProgram)(void);
typedef void  (WINAPI* PFN_glAttachShader)(GLuint, GLuint);
typedef void  (WINAPI* PFN_glLinkProgram)(GLuint);
typedef void  (WINAPI* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);
typedef void  (WINAPI* PFN_glUseProgram2)(GLuint);
typedef GLint (WINAPI* PFN_glGetUniformLocation)(GLuint, const char*);
typedef void  (WINAPI* PFN_glUniform1f)(GLint, GLfloat);
typedef void  (WINAPI* PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void  (WINAPI* PFN_glGenVertexArrays)(GLsizei, GLuint*);
typedef void  (WINAPI* PFN_glBindVertexArray2)(GLuint);

// Shared vertex shader: a single oversized triangle covering the viewport,
// positions synthesized from gl_VertexID (matches the WGSL vs()).
static const char* SV_VERT =
    "#version 330\n"
    "out vec2 vUv;\n"
    "void main(){\n"
    "  vec2 p = vec2((gl_VertexID==1)?3.0:-1.0, (gl_VertexID==2)?3.0:-1.0);\n"
    "  gl_Position = vec4(p,0.0,1.0);\n"
    "  vUv = p*0.5+0.5;\n"
    "}\n";

// Common fragment prelude: the audio uniforms + varying, mirroring struct U.
static const char* SV_FRAG_PRELUDE =
    "#version 330\n"
    "in vec2 vUv;\n"
    "out vec4 fragColor;\n"
    "uniform vec2 uRes;\n"
    "uniform float uTime,uLevel,uBass,uMid,uTreble,uBeat;\n";

// The three fragment bodies, ported 1:1 from webgpu.ts GPU_MODES.
static const char* SV_FRAG_BODY[3] = {
    // Plasma field
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float t = uTime;\n"
    "  float boost = 0.5 + uLevel*2.0;\n"
    "  float v = sin(p.x*6.0+t) + sin(p.y*6.0-t*1.2)\n"
    "          + sin((p.x+p.y)*5.0+t*0.7) + sin(length(p)*8.0-t*2.0+uBass*6.0);\n"
    "  v = v/4.0;\n"
    "  float hue = 0.6 + 0.15*v + uMid*0.3;\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)) + v*3.0);\n"
    "  float bright = clamp((0.4+0.6*abs(v))*boost + uBeat*0.4, 0.0, 1.0);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // Warp tunnel
    "void main(){\n"
    "  vec2 p = vUv*2.0-1.0; p.x *= uRes.x/uRes.y;\n"
    "  float r = length(p);\n"
    "  float a = atan(p.y,p.x);\n"
    "  float depth = 0.4/(r+0.05) + uTime*(0.5+uLevel*1.5);\n"
    "  float ang = a*3.0 + uTime*0.2;\n"
    "  float stripes = 0.5 + 0.5*sin(depth*12.566);\n"
    "  float spokes = 0.5 + 0.5*sin(ang*6.0 + uBass*10.0);\n"
    "  float m = stripes*spokes;\n"
    "  float fade = smoothstep(0.0, 0.15, r);\n"
    "  float hue = fract(depth*0.1 + uTreble*0.3);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = m*fade*(0.6 + uLevel + uBeat*0.5);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
    // Fractal fold
    "void main(){\n"
    "  vec2 p = (vUv*2.0-1.0)*1.5; p.x *= uRes.x/uRes.y;\n"
    "  vec2 z = p;\n"
    "  vec2 c = vec2(0.7+0.15*sin(uTime*0.3)+uBass*0.2,\n"
    "                0.5+0.15*cos(uTime*0.27)+uMid*0.2);\n"
    "  float orbit = 1e9;\n"
    "  for(int i=0;i<12;i++){ z = abs(z)/dot(z,z) - c; orbit = min(orbit, length(z)); }\n"
    "  float g = exp(-orbit*3.0);\n"
    "  float hue = fract(0.55 + orbit*0.3 + uTreble*0.4 + uTime*0.02);\n"
    "  vec3 col = 0.5 + 0.5*cos(6.28318*(hue+vec3(0.0,0.33,0.67)));\n"
    "  float bright = clamp(g*(1.0+uLevel*2.0) + uBeat*0.3, 0.0, 1.0);\n"
    "  fragColor = vec4(col*bright, 1.0);\n"
    "}\n",
};

static const wchar_t* SV_NAMES[3] = { L"Plasma field", L"Warp tunnel", L"Fractal fold" };
static const int SV_COUNT = 3;

struct ShaderProg {
    GLuint prog = 0;
    GLint uRes = -1, uTime = -1, uLevel = -1, uBass = -1, uMid = -1, uTreble = -1, uBeat = -1;
};

struct ShaderHost {
    HWND child = nullptr;
    HDC dc = nullptr;
    HGLRC glrc = nullptr;
    GLuint vao = 0;
    ShaderProg progs[3] = {};
    bool ready = false;
    GLuint fontBase = 0;
    GLuint overlayTex = 0;    // UI overlay uploaded from the D2D-rendered bitmap
    int overlayW = 0, overlayH = 0;
    int cw = 1, ch = 1;

    // GL function pointers.
    PFN_glCreateShader p_CreateShader = nullptr;
    PFN_glShaderSource p_ShaderSource = nullptr;
    PFN_glCompileShader p_CompileShader = nullptr;
    PFN_glGetShaderiv p_GetShaderiv = nullptr;
    PFN_glGetShaderInfoLog p_GetShaderInfoLog = nullptr;
    PFN_glDeleteShader p_DeleteShader = nullptr;
    PFN_glCreateProgram p_CreateProgram = nullptr;
    PFN_glAttachShader p_AttachShader = nullptr;
    PFN_glLinkProgram p_LinkProgram = nullptr;
    PFN_glGetProgramiv p_GetProgramiv = nullptr;
    PFN_glUseProgram2 p_UseProgram = nullptr;
    PFN_glGetUniformLocation p_GetUniformLocation = nullptr;
    PFN_glUniform1f p_Uniform1f = nullptr;
    PFN_glUniform2f p_Uniform2f = nullptr;
    PFN_glGenVertexArrays p_GenVertexArrays = nullptr;
    PFN_glBindVertexArray2 p_BindVertexArray = nullptr;

    static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        // Forward mouse input to the parent so the shared control deck is
        // clickable over the shader engine too.
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONUP: case WM_LBUTTONDBLCLK: {
            HWND p = GetParent(h); if (p) PostMessageW(p, m, w, l); return 0;
        }
        }
        return DefWindowProc(h, m, w, l);
    }

    template <class T> void LoadGL(T& fn, const char* name) {
        fn = (T)wglGetProcAddress(name);
    }

    GLuint Compile(GLenum type, const char* src1, const char* src2) {
        GLuint s = p_CreateShader(type);
        const char* parts[2] = { src1, src2 ? src2 : "" };
        GLsizei n = src2 ? 2 : 1;
        p_ShaderSource(s, n, parts, nullptr);
        p_CompileShader(s);
        GLint ok = 0; p_GetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {}; p_GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
            OutputDebugStringA("WaveScope shader compile error:\n");
            OutputDebugStringA(log);
            p_DeleteShader(s);
            return 0;
        }
        return s;
    }

    bool BuildProgram(int idx) {
        GLuint vs = Compile(GL_VERTEX_SHADER, SV_VERT, nullptr);
        GLuint fs = Compile(GL_FRAGMENT_SHADER, SV_FRAG_PRELUDE, SV_FRAG_BODY[idx]);
        if (!vs || !fs) return false;
        GLuint prog = p_CreateProgram();
        p_AttachShader(prog, vs);
        p_AttachShader(prog, fs);
        p_LinkProgram(prog);
        p_DeleteShader(vs); p_DeleteShader(fs);
        GLint ok = 0; p_GetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) return false;
        ShaderProg& p = progs[idx];
        p.prog = prog;
        p.uRes = p_GetUniformLocation(prog, "uRes");
        p.uTime = p_GetUniformLocation(prog, "uTime");
        p.uLevel = p_GetUniformLocation(prog, "uLevel");
        p.uBass = p_GetUniformLocation(prog, "uBass");
        p.uMid = p_GetUniformLocation(prog, "uMid");
        p.uTreble = p_GetUniformLocation(prog, "uTreble");
        p.uBeat = p_GetUniformLocation(prog, "uBeat");
        return true;
    }

    bool Init(HINSTANCE inst, HWND parent) {
        if (ready) return true;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"WaveScopeShader";
        wc.style = CS_OWNDC;
        RegisterClassW(&wc);

        RECT rc; GetClientRect(parent, &rc);
        child = CreateWindowExW(0, L"WaveScopeShader", L"", WS_CHILD,
                                0, 0, rc.right, rc.bottom, parent, nullptr, inst, nullptr);
        if (!child) return false;
        dc = GetDC(child);

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
        int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) return false;

        HGLRC legacy = wglCreateContext(dc);
        if (!legacy) return false;
        wglMakeCurrent(dc, legacy);

        auto createAttribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        if (createAttribs) {
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
                0
            };
            HGLRC ctx = createAttribs(dc, nullptr, attribs);
            if (ctx) { wglMakeCurrent(dc, ctx); wglDeleteContext(legacy); glrc = ctx; }
            else glrc = legacy;
        } else {
            glrc = legacy;
        }

        auto swapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swapInterval) swapInterval(1);

        LoadGL(p_CreateShader, "glCreateShader");
        LoadGL(p_ShaderSource, "glShaderSource");
        LoadGL(p_CompileShader, "glCompileShader");
        LoadGL(p_GetShaderiv, "glGetShaderiv");
        LoadGL(p_GetShaderInfoLog, "glGetShaderInfoLog");
        LoadGL(p_DeleteShader, "glDeleteShader");
        LoadGL(p_CreateProgram, "glCreateProgram");
        LoadGL(p_AttachShader, "glAttachShader");
        LoadGL(p_LinkProgram, "glLinkProgram");
        LoadGL(p_GetProgramiv, "glGetProgramiv");
        LoadGL(p_UseProgram, "glUseProgram");
        LoadGL(p_GetUniformLocation, "glGetUniformLocation");
        LoadGL(p_Uniform1f, "glUniform1f");
        LoadGL(p_Uniform2f, "glUniform2f");
        LoadGL(p_GenVertexArrays, "glGenVertexArrays");
        LoadGL(p_BindVertexArray, "glBindVertexArray");
        if (!p_CreateShader || !p_CreateProgram || !p_UseProgram || !p_GenVertexArrays)
            return false;

        p_GenVertexArrays(1, &vao);

        HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FIXED_PITCH | FF_MODERN, L"Consolas");
        HFONT oldFont = (HFONT)SelectObject(dc, font);
        fontBase = glGenLists(256);
        wglUseFontBitmapsW(dc, 0, 256, fontBase);
        SelectObject(dc, oldFont);
        DeleteObject(font);

        cw = rc.right > 0 ? rc.right : 1;
        ch = rc.bottom > 0 ? rc.bottom : 1;

        bool any = false;
        for (int i = 0; i < SV_COUNT; i++) any |= BuildProgram(i);
        if (!any) return false;

        ready = true;
        return true;
    }

    const wchar_t* ShaderName(int idx) const { return SV_NAMES[(idx % SV_COUNT + SV_COUNT) % SV_COUNT]; }
    int ShaderCount() const { return SV_COUNT; }

    void Show(bool on) { if (child) ShowWindow(child, on ? SW_SHOW : SW_HIDE); }

    void Resize(int w, int h) {
        if (!ready || w <= 0 || h <= 0) return;
        SetWindowPos(child, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE);
        cw = w; ch = h;
    }

    // Render the shader frame; leaves the context current with no swap so the
    // shared UI can be drawn over it via BeginOverlay + a GLPainter.
    void RenderEngine(int idx, const AudioFrame& f) {
        if (!ready) return;
        idx = (idx % SV_COUNT + SV_COUNT) % SV_COUNT;
        ShaderProg& s = progs[idx];
        if (!s.prog) return;
        wglMakeCurrent(dc, glrc);
        glViewport(0, 0, cw, ch);
        glClearColor(0.043f, 0.051f, 0.047f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        p_UseProgram(s.prog);
        p_BindVertexArray(vao);
        p_Uniform2f(s.uRes, (float)cw, (float)ch);
        p_Uniform1f(s.uTime, f.t);
        p_Uniform1f(s.uLevel, f.level);
        p_Uniform1f(s.uBass, f.bass);
        p_Uniform1f(s.uMid, f.mid);
        p_Uniform1f(s.uTreble, f.treble);
        p_Uniform1f(s.uBeat, f.beat);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Reset modern-GL state and set a top-left ortho for the UIPainter.
    void BeginOverlay() {
        p_UseProgram(0);
        p_BindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, cw, ch, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    }

    // Composite the D2D-rendered UI overlay (premultiplied BGRA) as a
    // full-window textured quad. Re-uploads only when `dirty`.
    void DrawOverlayTex(const void* bgra, int tw, int th, bool dirty) {
        glEnable(GL_TEXTURE_2D);
        if (!overlayTex) glGenTextures(1, &overlayTex);
        glBindTexture(GL_TEXTURE_2D, overlayTex);
        if (dirty && bgra && tw > 0 && th > 0) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, bgra);
            overlayW = tw; overlayH = th;
        }
        if (overlayW <= 0) { glDisable(GL_TEXTURE_2D); return; }
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied alpha
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f((float)cw, 0);
        glTexCoord2f(1, 1); glVertex2f((float)cw, (float)ch);
        glTexCoord2f(0, 1); glVertex2f(0, (float)ch);
        glEnd();
        glDisable(GL_BLEND);
        glDisable(GL_TEXTURE_2D);
    }

    void Present() {
        glDisable(GL_BLEND);
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        SwapBuffers(dc);
    }

    void Destroy() {
        if (glrc) wglMakeCurrent(dc, glrc);
        if (overlayTex) { glDeleteTextures(1, &overlayTex); overlayTex = 0; }
        if (fontBase && glrc) { glDeleteLists(fontBase, 256); fontBase = 0; }
        if (glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glrc); glrc = nullptr; }
        if (child && dc) ReleaseDC(child, dc);
        if (child) DestroyWindow(child);
        child = nullptr; dc = nullptr; ready = false;
    }
};
