static void TestStorage() {
    auto portable = WsChoosePaths(L"C:\\portable\\", false, false, L"C:\\profile", L"C:\\videos");
    auto installed = WsChoosePaths(L"C:\\programs\\", true, false, L"C:\\profile", L"C:\\videos");
    auto selftest = WsChoosePaths(L"C:\\test\\", true, true, L"C:\\profile", L"C:\\videos");
    CHECK(portable.data == L"C:\\portable\\" && portable.recordings == portable.data, "portable data stays beside app");
    CHECK(installed.data == L"C:\\profile\\WaveScope\\" && installed.recordings == L"C:\\videos\\WaveScope\\", "installed user data is separate");
    CHECK(selftest.data == L"C:\\test\\" && !selftest.installed, "selftest cannot access installed user data");
    CHECK(WsChoosePaths(L"C:\\app", true, false, L"C:\\profile", L"").recordings == L"C:\\profile\\WaveScope\\Recordings\\", "recording fallback");
    std::wstring name;
    CHECK(WsPresetFileName(L"  My preset.MILK  ", name) && name == L"My preset.milk", "normalize preset extension and outer whitespace");
    CHECK(WsPresetFileName(L"\x661f\x7a7a", name), "Unicode preset names supported");
    for (const wchar_t* invalid : {L"", L"..", L".milk", L"CON", L"nul.milk", L"LPT1.edit", L"COM\x00b9", L"../escape", L"a:b", L"a?b", L"a*b", L"bad.", L"name .milk"})
        CHECK(!WsPresetFileName(invalid, name), "invalid/reserved preset name rejected");
    wchar_t parent[MAX_PATH], temp[MAX_PATH]; GetTempPathW(MAX_PATH, parent); GetTempFileNameW(parent, L"wss", 0, temp);
    DeleteFileW(temp); CreateDirectoryW(temp, nullptr);
    std::wstring dir = temp, file = dir + L"\\original.milk";
    CHECK(WsSaveNewPreset(dir, L"original.milk", "original") == 0, "initial preset save succeeds");
    CHECK(WsSaveNewPreset(dir, L"original.milk", "replacement") != 0, "Save As refuses overwrite");
    CHECK(WsSaveNewPreset(dir, L"ORIGINAL.milk", "replacement") != 0, "case-insensitive overwrite refused");
    FILE* fp = nullptr; _wfopen_s(&fp, file.c_str(), L"rb"); char bytes[32] = {};
    if (fp) { fread(bytes, 1, sizeof(bytes) - 1, fp); fclose(fp); }
    CHECK(std::string(bytes) == "original", "existing preset preserved byte-for-byte");
    CHECK(WsSaveNewPreset(dir, L"..\\escape.milk", "x") == ERROR_INVALID_NAME, "save helper also rejects paths");
    CHECK(WsSaveNewPreset(file, L"blocked.milk", "x") != 0, "directory/write failure reported");
    std::atomic<bool> start{false}; DWORD result[2] = {};
    std::thread a([&] { while (!start.load()) std::this_thread::yield(); result[0] = WsSaveNewPreset(dir, L"race.milk", "a"); });
    std::thread b([&] { while (!start.load()) std::this_thread::yield(); result[1] = WsSaveNewPreset(dir, L"race.milk", "b"); });
    start = true; a.join(); b.join();
    CHECK((result[0] == 0) != (result[1] == 0), "concurrent saves cannot overwrite each other");
    size_t files = 0;
    for (auto& entry : std::filesystem::directory_iterator(dir)) { (void)entry; ++files; }
    CHECK(files == 2, "failed saves leave no temporary files");
    DeleteFileW(file.c_str()); DeleteFileW((dir + L"\\race.milk").c_str()); RemoveDirectoryW(dir.c_str());
}
