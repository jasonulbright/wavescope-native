// projectM host: runs the real MilkDrop engine (libprojectM, native MSVC
// build) inside a child window with its own WGL/OpenGL 3.3 context, layered
// over the Direct2D surface. Toggled with the M key. Feeds PCM from the same
// AudioTap and renders raw .milk presets.
//
// libprojectM is the one deliberate dependency (LGPL-2.1, linked as a DLL:
// projectM-4.dll ships next to the exe). Everything else stays OS-only.

#include <projectM-4/projectM.h>
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
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

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
    int presetIdx = 0;
    bool ready = false;

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
            const int attribs[] = {
                WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
                WGL_CONTEXT_MINOR_VERSION_ARB, 3,
                WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                0
            };
            HGLRC core = createAttribs(dc, nullptr, attribs);
            if (core) {
                wglMakeCurrent(dc, core);
                wglDeleteContext(legacy);
                glrc = core;
            } else {
                glrc = legacy; // fall back to whatever legacy gives
            }
        } else {
            glrc = legacy;
        }

        auto swapInterval = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swapInterval) swapInterval(1); // vsync, so the GL loop doesn't spin

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

    // Collect every .milk file in a folder into the playable list.
    void ScanPresets(const std::wstring& folder) {
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
    }

    void RenderFrame(const AudioFrame& f) {
        if (!ready) return;
        wglMakeCurrent(dc, glrc);
        unsigned int maxs = projectm_pcm_get_max_samples();
        unsigned int count = maxs < 2048 ? maxs : 2048;
        projectm_pcm_add_float(pm, f.wave, count, PROJECTM_MONO);
        projectm_opengl_render_frame(pm);
        SwapBuffers(dc);
    }

    void Destroy() {
        if (pm) { projectm_destroy(pm); pm = nullptr; }
        if (glrc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(glrc); glrc = nullptr; }
        if (child && dc) ReleaseDC(child, dc);
        if (child) DestroyWindow(child);
        child = nullptr; dc = nullptr; ready = false;
    }
};
