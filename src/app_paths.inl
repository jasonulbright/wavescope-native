// Assets stay beside the executable. Installed editions keep user data outside
// the managed application directory; portable editions stay self-contained.
#include <shlobj.h>
#include <filesystem>
#include <string>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

struct AppPaths { std::wstring data, recordings; bool installed = false; };
static AppPaths g_paths;

static std::wstring WsWithSlash(std::wstring path) {
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path;
}
static AppPaths WsChoosePaths(const std::wstring& exe, bool installed, bool selftest,
                              const std::wstring& local, const std::wstring& videos) {
    if (!installed || selftest) return { WsWithSlash(exe), WsWithSlash(exe), false };
    std::wstring data = WsWithSlash(local) + L"WaveScope\\";
    return { data, videos.empty() ? data + L"Recordings\\" : WsWithSlash(videos) + L"WaveScope\\", true };
}
static std::wstring WsKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) return {};
    std::wstring result(path); CoTaskMemFree(path); return result;
}
static bool WsEnsureDirectory(const std::wstring& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error && std::filesystem::is_directory(path, error) && !error;
}
static bool WsInitPaths(bool selftest) {
    std::wstring exe = PmExeDir();
    bool installed = GetFileAttributesW((exe + L"installed.flag").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring local = installed && !selftest ? WsKnownFolder(FOLDERID_LocalAppData) : L"";
    if (installed && !selftest && local.empty()) return false;
    g_paths = WsChoosePaths(exe, installed, selftest, local,
                           installed && !selftest ? WsKnownFolder(FOLDERID_Videos) : L"");
    return WsEnsureDirectory(g_paths.data);
}
static std::wstring WsDataDir() { return g_paths.data.empty() ? PmExeDir() : g_paths.data; }
static std::wstring WsRecordingsDir() { return g_paths.recordings.empty() ? PmExeDir() : g_paths.recordings; }
