// projectM host: runs the real MilkDrop engine (libprojectM, native MSVC
// build) inside a child window with its own WGL/OpenGL 3.3 context, layered
// over the Direct2D surface. Toggled with the M key. Feeds PCM from the same
// AudioTap and renders raw .milk presets.
//
// libprojectM is the one deliberate dependency (LGPL-2.1, linked as a DLL:
// projectM-4.dll ships next to the exe). Everything else stays OS-only.

#include <projectM-4/projectM.h>
#include <GL/gl.h>
#include <shlobj.h>
#include <string>
#include <vector>

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

// WGL context-creation extension (avoid needing wglext.h).
typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int);
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002

// GL tokens for the UI-overlay texture (not in the 1.1 gl.h on Windows).
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// GL 2.0+ entry points we need only to reset state before the legacy-GL HUD
// overlay (loaded at runtime; not in opengl32.lib's 1.1 export set).
typedef void(WINAPI* PFN_glUseProgram)(GLuint);
typedef void(WINAPI* PFN_glBindVertexArray)(GLuint);

// GL loader projectM's internal glad uses to resolve functions.
static void* ProjectMGlLoad(const char* name, void*) {
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        static HMODULE gl = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(gl, name);
    }
    return p;
}

// Directory holding raw .milk presets, resolved next to the exe. The user can
// repoint this at their own preset archive (folder picker).
static std::wstring PmExeDir() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\'); if (slash) slash[1] = 0;
    return std::wstring(path);
}

struct ProjectMHost {
    HWND child = nullptr;
    HDC dc = nullptr;
    HGLRC glrc = nullptr;
    projectm_handle pm = nullptr;
    std::vector<std::wstring> presetPaths;
    std::wstring curName = L"(none)";
    std::wstring scanDir;             // folder currently scanned/watched
    HANDLE presetWatch = nullptr;     // hot-reload change notification
    int presetIdx = 0;
    bool ready = false;
    GLuint fontBase = 0;      // wglUseFontBitmaps display-list base for the HUD
    GLuint overlayTex = 0;    // UI overlay uploaded from the D2D-rendered bitmap
    int overlayW = 0, overlayH = 0;
    int cw = 1, ch = 1;       // child client size, for the HUD ortho
    PFN_glUseProgram p_glUseProgram = nullptr;
    PFN_glBindVertexArray p_glBindVertexArray = nullptr;

    static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        // Forward mouse input to the parent so the shared control deck is
        // clickable over the GL engine (child coords == parent client coords).
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONUP: case WM_LBUTTONDBLCLK: {
            HWND p = GetParent(h); if (p) PostMessageW(p, m, w, l); return 0;
        }
        }
        return DefWindowProc(h, m, w, l);
    }

    bool Init(HINSTANCE inst, HWND parent) {
        if (ready) return true;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"WaveScopeGL";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);   // else the frame's resize cursor shows through
        wc.style = CS_OWNDC;
        RegisterClassW(&wc);

        RECT rc; GetClientRect(parent, &rc);
        child = CreateWindowExW(0, L"WaveScopeGL", L"", WS_CHILD,
                                0, 0, rc.right, rc.bottom, parent, nullptr, inst, nullptr);
        if (!child) return false;
        dc = GetDC(child);

        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
        int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) return false;

        // Legacy context first, to fetch the modern-context creator.
        HGLRC legacy = wglCreateContext(dc);
        if (!legacy) return false;
        wglMakeCurrent(dc, legacy);

        auto createAttribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
        if (createAttribs) {
            // Compatibility profile: full GL 3.3 for projectM AND legacy
            // immediate-mode + wglUseFontBitmaps for the HUD overlay.
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
                0
            };
            HGLRC ctx = createAttribs(dc, nullptr, attribs);
            if (ctx) {
                wglMakeCurrent(dc, ctx);
                wglDeleteContext(legacy);
                glrc = ctx;
            } else {
                glrc = legacy;
            }
        } else {
            glrc = legacy;
        }

        auto swapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swapInterval) swapInterval(1); // vsync, so the GL loop doesn't spin

        p_glUseProgram = (PFN_glUseProgram)wglGetProcAddress("glUseProgram");
        p_glBindVertexArray = (PFN_glBindVertexArray)wglGetProcAddress("glBindVertexArray");

        // Build a bitmap font for the HUD from a GDI font.
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

        pm = projectm_create_with_opengl_load_proc(ProjectMGlLoad, nullptr);
        if (!pm) return false;
        projectm_set_window_size(pm, (size_t)(rc.right > 0 ? rc.right : 1),
                                 (size_t)(rc.bottom > 0 ? rc.bottom : 1));
        projectm_set_mesh_size(pm, 48, 32);
        projectm_set_fps(pm, 60);
        projectm_set_preset_duration(pm, 30.0);
        projectm_set_soft_cut_duration(pm, 2.7);
        ScanPresets(PmExeDir() + L"presets");
        LoadPreset(0, false);
        ready = true;
        return true;
    }

    // Collect every .milk file in a folder into the playable list, and (re)arm
    // a folder watch so dropping/removing presets hot-reloads the library.
    void ScanPresets(const std::wstring& folder) {
        scanDir = folder;
        presetPaths.clear();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((folder + L"\\*.milk").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    presetPaths.push_back(folder + L"\\" + fd.cFileName);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (presetWatch && presetWatch != INVALID_HANDLE_VALUE)
            FindCloseChangeNotification(presetWatch);
        presetWatch = FindFirstChangeNotificationW(folder.c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
    }

    // Rescan (non-blocking) when the watched preset folder changes on disk.
    void PollReload() {
        if (!presetWatch || presetWatch == INVALID_HANDLE_VALUE) return;
        if (WaitForSingleObject(presetWatch, 0) != WAIT_OBJECT_0) return;
        std::wstring keep = curName;
        ScanPresets(scanDir);   // re-arms the watch
        // Keep the same preset selected if it survived the change.
        for (size_t i = 0; i < presetPaths.size(); i++) {
            const wchar_t* base = wcsrchr(presetPaths[i].c_str(), L'\\');
            if (keep == (base ? base + 1 : presetPaths[i].c_str())) { presetIdx = (int)i; break; }
        }
    }

    // Load a preset by index from its raw .milk text.
    void LoadPreset(int idx, bool smooth) {
        if (!pm || presetPaths.empty()) return;
        int n = (int)presetPaths.size();
        presetIdx = ((idx % n) + n) % n;
        const std::wstring& path = presetPaths[presetIdx];
        const wchar_t* base = wcsrchr(path.c_str(), L'\\');
        curName = base ? base + 1 : path;
        FILE* fp = _wfopen(path.c_str(), L"rb");
        if (!fp) return;
        fseek(fp, 0, SEEK_END); long len = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (len > 0 && len < 1 << 20) {
            char* buf = (char*)malloc(len + 1);
            if (buf) {
                size_t rd = fread(buf, 1, len, fp); buf[rd] = 0;
                projectm_load_preset_data(pm, buf, smooth);
                free(buf);
            }
        }
        fclose(fp);
    }

    // Hard cut (smooth=false): projectM's soft-cut cross-fade blends the new
    // preset in before its first frame has initialized, which flashes white on
    // switch. A hard swap renders the new preset directly and avoids it.
    void NextPreset(int dir) { if (ready) LoadPreset(presetIdx + dir, false); }

    // Repoint at a folder of presets (the user's .milk archive).
    void SetPresetFolder(const wchar_t* folder) {
        ScanPresets(folder);
        if (!presetPaths.empty()) LoadPreset(0, true);
    }

    const wchar_t* PresetName() const { return curName.c_str(); }
    int PresetCount() const { return (int)presetPaths.size(); }
    int PresetIndex() const { return presetIdx; }
    // Bare filename of preset i, for the picker dropdown.
    const wchar_t* PresetNameAt(int i) const {
        if (i < 0 || i >= (int)presetPaths.size()) return L"";
        const wchar_t* base = wcsrchr(presetPaths[i].c_str(), L'\\');
        return base ? base + 1 : presetPaths[i].c_str();
    }

    void Show(bool on) {
        if (child) ShowWindow(child, on ? SW_SHOW : SW_HIDE);
    }

    void Resize(int w, int h) {
        if (!ready || w <= 0 || h <= 0) return;
        SetWindowPos(child, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE);
        wglMakeCurrent(dc, glrc);
        projectm_set_window_size(pm, (size_t)w, (size_t)h);
        cw = w; ch = h;
    }

    // Render the MilkDrop frame; leaves the context current with no swap, so
    // the shared UI can be drawn over it via BeginOverlay + a GLPainter.
    void RenderEngine(const AudioFrame& f) {
        if (!ready) return;
        wglMakeCurrent(dc, glrc);
        unsigned int maxs = projectm_pcm_get_max_samples();
        unsigned int count = maxs < 2048 ? maxs : 2048;
        projectm_pcm_add_float(pm, f.wave, count, PROJECTM_MONO);
        projectm_opengl_render_frame(pm);
    }

    // Reset modern-GL state and set a top-left ortho so the UIPainter can draw
    // with legacy immediate-mode calls over the rendered frame.
    void BeginOverlay() {
        if (p_glUseProgram) p_glUseProgram(0);
        if (p_glBindVertexArray) p_glBindVertexArray(0);
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

    // Read the just-rendered (pre-present) backbuffer; bottom-up BGRA.
    bool ReadBack(std::vector<uint8_t>& out, int& w, int& h) {
        if (!ready) return false;
        wglMakeCurrent(dc, glrc);
        w = cw; h = ch;
        if (w <= 0 || h <= 0) return false;
        out.resize((size_t)w * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, w, h, GL_BGRA_EXT, GL_UNSIGNED_BYTE, out.data());
        return glGetError() == 0;
    }

    void Destroy() {
        if (presetWatch && presetWatch != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(presetWatch); presetWatch = nullptr; }
        if (glrc) wglMakeCurrent(dc, glrc);
        if (overlayTex) { glDeleteTextures(1, &overlayTex); overlayTex = 0; }
        if (fontBase && glrc) { glDeleteLists(fontBase, 256); fontBase = 0; }
        if (pm) { projectm_destroy(pm); pm = nullptr; }
        if (glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glrc); glrc = nullptr; }
        if (child && dc) ReleaseDC(child, dc);
        if (child) DestroyWindow(child);
        child = nullptr; dc = nullptr; ready = false;
    }
};
