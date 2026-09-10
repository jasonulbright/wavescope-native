// Save As must never truncate an existing preset or report a partial write as
// success. Publish a completed sibling temporary file with a no-replace rename.
#include <algorithm>
#include <cwctype>
static bool WsPresetFileName(std::wstring input, std::wstring& name) {
    size_t first = input.find_first_not_of(L" \t\r\n"), last = input.find_last_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    input = input.substr(first, last - first + 1);
    if (input.size() > 120) return false;
    for (wchar_t c : input) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
    if (input.back() == L'.') return false;
    if (input.size() >= 5 && _wcsicmp(input.c_str() + input.size() - 5, L".milk") == 0)
        input.resize(input.size() - 5);
    if (input.empty() || input.back() == L'.' || input.back() == L' ') return false;
    std::wstring base = input.substr(0, input.find(L'.'));
    while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) base.pop_back();
    for (wchar_t& c : base) c = (wchar_t)towupper(c);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
        base == L"CONIN$" || base == L"CONOUT$") return false;
    if (base.size() == 4 && (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") &&
        (wcschr(L"123456789\x00b9\x00b2\x00b3", base[3]) != nullptr)) return false;
    name = input + L".milk";
    return true;
}
static DWORD WsSaveNewPreset(const std::wstring& directory, const std::wstring& name,
                             const std::string& text) {
    std::wstring validated;
    if (!WsPresetFileName(name, validated) || validated != name) return ERROR_INVALID_NAME;
    if (!WsEnsureDirectory(directory)) return ERROR_PATH_NOT_FOUND;
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) return ERROR_GEN_FAILURE;
    wchar_t unique[40]; StringFromGUID2(guid, unique, 40);
    std::wstring temp = WsWithSlash(directory) + unique + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    size_t offset = 0;
    while (offset < text.size()) {
        DWORD count = (DWORD)(std::min)(text.size() - offset, (size_t)(1024 * 1024)), written = 0;
        if (!WriteFile(file, text.data() + offset, count, &written, nullptr)) { error = GetLastError(); break; }
        if (written == 0) { error = ERROR_WRITE_FAULT; break; }
        offset += written;
    }
    if (!error && !FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && !error) error = GetLastError();
    if (!error && !MoveFileExW(temp.c_str(), (WsWithSlash(directory) + name).c_str(), MOVEFILE_WRITE_THROUGH))
        error = GetLastError();
    if (error) DeleteFileW(temp.c_str());
    return error;
}
