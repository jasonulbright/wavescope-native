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
    int cw = 1, ch = 1;       // child client size, for the HUD ortho
    PFN_glUseProgram p_glUseProgram = nullptr;
    PFN_glBindVertexArray p_glBindVertexArray = nullptr;

    static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        return DefWindowProc(h, m, w, l);
    }

    bool Init(HINSTANCE inst, HWND parent) {
        if (ready) return true;

        WNDCLASSW wc = {};
        wc.lpfnWndProc = ChildProc;
        wc.hInstance = inst;
        wc.lpszClassName = L"SpectraVizGL";
        wc.style = CS_OWNDC;
        RegisterClassW(&wc);

        RECT rc; GetClientRect(parent, &rc);
        child = CreateWindowExW(0, L"SpectraVizGL", L"", WS_CHILD,
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

    void NextPreset(int dir) { if (ready) LoadPreset(presetIdx + dir, true); }

    // Repoint at a folder of presets (the user's .milk archive).
    void SetPresetFolder(const wchar_t* folder) {
        ScanPresets(folder);
        if (!presetPaths.empty()) LoadPreset(0, true);
    }

    const wchar_t* PresetName() const { return curName.c_str(); }
    int PresetCount() const { return (int)presetPaths.size(); }

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

    // Draw two ASCII HUD lines with legacy GL (bitmap font), after projectM
    // has rendered, so the HUD stays visible over the MilkDrop engine.
    void DrawHud(const char* line1, const char* line2) {
        if (!fontBase) return;
        if (p_glUseProgram) p_glUseProgram(0);       // back to fixed-function
        if (p_glBindVertexArray) p_glBindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
        glOrtho(0, cw, ch, 0, -1, 1);               // top-left origin
        glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
        // Readability bar.
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.043f, 0.051f, 0.047f, 0.55f);
        glBegin(GL_QUADS);
        glVertex2i(0, 0); glVertex2i(cw, 0); glVertex2i(cw, 44); glVertex2i(0, 44);
        glEnd();
        glDisable(GL_BLEND);
        glColor3f(0.95f, 0.96f, 0.95f);
        glListBase(fontBase);
        if (line1) { glRasterPos2i(14, 18); glCallLists((GLsizei)strlen(line1), GL_UNSIGNED_BYTE, line1); }
        if (line2) { glRasterPos2i(14, 36); glCallLists((GLsizei)strlen(line2), GL_UNSIGNED_BYTE, line2); }
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
    }

    void RenderFrame(const AudioFrame& f, const char* hud1, const char* hud2) {
        if (!ready) return;
        wglMakeCurrent(dc, glrc);
        unsigned int maxs = projectm_pcm_get_max_samples();
        unsigned int count = maxs < 2048 ? maxs : 2048;
        projectm_pcm_add_float(pm, f.wave, count, PROJECTM_MONO);
        projectm_opengl_render_frame(pm);
        if (hud1) DrawHud(hud1, hud2);
        SwapBuffers(dc);
    }

    void Destroy() {
        if (presetWatch && presetWatch != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(presetWatch); presetWatch = nullptr; }
        if (fontBase && glrc) { wglMakeCurrent(dc, glrc); glDeleteLists(fontBase, 256); fontBase = 0; }
        if (pm) { projectm_destroy(pm); pm = nullptr; }
        if (glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glrc); glrc = nullptr; }
        if (child && dc) ReleaseDC(child, dc);
        if (child) DestroyWindow(child);
        child = nullptr; dc = nullptr; ready = false;
    }
};
