static std::wstring WsControlText(HWND control) {
    int n = GetWindowTextLengthW(control);
    std::wstring s((size_t)n + 1, L'\0');
    s.resize(GetWindowTextW(control, s.data(), n + 1));
    return s;
}

static INT_PTR CALLBACK PresetBrowserProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_INITDIALOG) {
        a = (App*)lp; SetWindowLongPtrW(dlg, GWLP_USERDATA, lp); a->presetDialog = dlg;
        SendDlgItemMessageW(dlg, IDC_PRESET_QUERY, EM_SETLIMITTEXT, 256, 0);
        CheckDlgButton(dlg, IDC_PRESET_SHUFFLE, a->presetFavorites.shuffleOnly ? BST_CHECKED : BST_UNCHECKED);
        a->RefreshPresetBrowser(); SetTimer(dlg, 1, 250, nullptr);
        return TRUE;
    }
    if (!a) return FALSE;
    if (msg == WM_COMMAND) { a->PresetBrowserCommand(LOWORD(wp), HIWORD(wp)); return TRUE; }
    if (msg == WM_TIMER && (a->browserRevision != a->pmHost.presetsRevision || a->browserLibrary != a->pmHost.scanDir))
        a->RefreshPresetBrowser();
    if (msg == WM_CLOSE) { DestroyWindow(dlg); return TRUE; }
    if (msg == WM_DESTROY) { KillTimer(dlg, 1); a->presetDialog = nullptr; }
    return FALSE;
}

void App::OpenPresetBrowser() {
    if (!pmActive) EnterProjectM(false);
    if (!pmActive || !pmHost.ready) return;
    deckMenu = -1;
    if (!presetDialog) CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_PRESETS), hwnd, PresetBrowserProc, (LPARAM)this);
    if (presetDialog) { ShowWindow(presetDialog, SW_SHOW); SetForegroundWindow(presetDialog); SetFocus(GetDlgItem(presetDialog, IDC_PRESET_QUERY)); }
}

void App::RefreshPresetBrowser() {
    HWND list = GetDlgItem(presetDialog, IDC_PRESET_LIST);
    int old = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    std::wstring keep = old >= 0 && old < (int)browserPaths.size() ? browserPaths[old] : pmHost.curPath;
    std::wstring query = WsFold(WsControlText(GetDlgItem(presetDialog, IDC_PRESET_QUERY)));
    bool only = IsDlgButtonChecked(presetDialog, IDC_PRESET_ONLY) == BST_CHECKED;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0); SendMessageW(list, LB_RESETCONTENT, 0, 0);
    browserPaths.clear(); int selected = -1, longest = 0;
    for (int i = 0; i < pmHost.PresetCount(); ++i) {
        const auto& path = pmHost.presetPaths[i]; bool favorite = IsPresetFavorite(path);
        std::wstring name = pmHost.PresetNameAt(i);
        if ((only && !favorite) || WsFold(name).find(query) == std::wstring::npos) continue;
        if (path == keep) selected = (int)browserPaths.size();
        browserPaths.push_back(path);
        std::wstring label = (favorite ? L"*  " : L"   ") + name;
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        longest = (std::max)(longest, (int)label.size());
    }
    if (selected < 0 && !browserPaths.empty()) selected = 0;
    SendMessageW(list, LB_SETCURSEL, selected, 0);
    SendMessageW(list, LB_SETHORIZONTALEXTENT, longest * 9, 0);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, TRUE);
    browserRevision = pmHost.presetsRevision; browserLibrary = pmHost.scanDir;
    std::wstring status = std::to_wstring(browserPaths.size()) + L" matching presets. Double-click or Play to preview.";
    if (browserPaths.empty()) status = L"No matching presets. Clear the search or Favorites only filter.";
    SetDlgItemTextW(presetDialog, IDC_PRESET_STATUS, status.c_str());
    PresetBrowserCommand(IDC_PRESET_LIST, LBN_SELCHANGE);
}

void App::PresetBrowserCommand(int id, int code) {
    if (id == IDCANCEL) { DestroyWindow(presetDialog); return; }
    if ((id == IDC_PRESET_QUERY && code == EN_CHANGE) || id == IDC_PRESET_ONLY) { RefreshPresetBrowser(); return; }
    if (id == IDC_PRESET_SHUFFLE) {
        presetFavorites.shuffleOnly = IsDlgButtonChecked(presetDialog, id) == BST_CHECKED;
        SaveConfig(); return;
    }
    int row = (int)SendDlgItemMessageW(presetDialog, IDC_PRESET_LIST, LB_GETCURSEL, 0, 0);
    bool valid = row >= 0 && row < (int)browserPaths.size();
    EnableWindow(GetDlgItem(presetDialog, IDOK), valid);
    EnableWindow(GetDlgItem(presetDialog, IDC_PRESET_FAVORITE), valid);
    if (!valid) return;
    std::wstring path = browserPaths[row];
    SetDlgItemTextW(presetDialog, IDC_PRESET_FAVORITE, IsPresetFavorite(path) ? L"&Remove favorite" : L"&Add favorite");
    if (id == IDC_PRESET_FAVORITE) {
        auto key = FavoriteKey(path);
        if (!presetFavorites.keys.erase(key) && presetFavorites.keys.size() < 4096) presetFavorites.keys.insert(key);
        SaveConfig(); RefreshPresetBrowser(); return;
    }
    if (id == IDOK || (id == IDC_PRESET_LIST && code == LBN_DBLCLK)) {
        auto it = std::find(pmHost.presetPaths.begin(), pmHost.presetPaths.end(), path);
        if (it == pmHost.presetPaths.end()) { RefreshPresetBrowser(); return; }
        if (!pmActive) { if (shaderActive) ToggleShader(); ToggleProjectM(); }
        pmHost.LoadPreset((int)(it - pmHost.presetPaths.begin()), false);
        SetDlgItemTextW(presetDialog, IDC_PRESET_STATUS, pmHost.loadFailed ? L"This preset could not be loaded. The previous visual is still playing." : L"Playing selected preset.");
        hudActivity = Now();
    }
}
