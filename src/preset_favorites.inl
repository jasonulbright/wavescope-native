#include <set>

static std::wstring WsFold(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// Bundled/user-library identities survive moving a portable folder. External
// libraries use full paths, so same-named presets in different folders differ.
static std::wstring WsPresetKey(std::wstring path, std::wstring app, std::wstring data) {
    auto norm = [](std::wstring s) {
        for (auto& c : s) if (c == L'/') c = L'\\';
        return WsFold(s);
    };
    path = norm(path); app = norm(WsWithSlash(app)); data = norm(WsWithSlash(data));
    if (path.compare(0, app.size(), app) == 0) return L"app:" + path.substr(app.size());
    if (path.compare(0, data.size(), data) == 0) return L"user:" + path.substr(data.size());
    return L"file:" + path;
}

struct PresetFavorites {
    std::set<std::wstring> keys;
    bool shuffleOnly = false;
    void Load(const std::wstring& path) {
        keys.clear();
        shuffleOnly = GetPrivateProfileIntW(L"favorites", L"shuffleOnly", 0, path.c_str()) != 0;
        int count = (std::min)(4096, (int)GetPrivateProfileIntW(L"favorites", L"count", 0, path.c_str()));
        std::vector<wchar_t> value(32768);
        for (int i = 0; i < count; ++i) {
            std::wstring name = L"p" + std::to_wstring(i);
            GetPrivateProfileStringW(L"favorites", name.c_str(), L"", value.data(), (DWORD)value.size(), path.c_str());
            if (value[0]) keys.insert(value.data());
        }
    }
    void Save(const std::wstring& path) const {
        WritePrivateProfileStringW(L"favorites", nullptr, nullptr, path.c_str());
        WritePrivateProfileStringW(L"favorites", L"shuffleOnly", shuffleOnly ? L"1" : L"0", path.c_str());
        WritePrivateProfileStringW(L"favorites", L"count", std::to_wstring(keys.size()).c_str(), path.c_str());
        int i = 0;
        for (const auto& key : keys)
            WritePrivateProfileStringW(L"favorites", (L"p" + std::to_wstring(i++)).c_str(), key.c_str(), path.c_str());
    }
};
