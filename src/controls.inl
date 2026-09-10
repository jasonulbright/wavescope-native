static bool WsControlCombo(int i, bool pm) {
    return (i == App::DK_PICK && !pm) || i == App::DK_TRACE || i == App::DK_SHUF ||
           i == App::DK_RES || i == App::DK_DISP || i == App::DK_MORPHT;
}
static INT_PTR CALLBACK ControlsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_INITDIALOG) {
        a = (App*)lp; SetWindowLongPtrW(dlg, GWLP_USERDATA, lp); a->controlsDialog = dlg;
        a->controlsEngine = -1; a->RefreshControls(); SetTimer(dlg, 1, 250, nullptr); return TRUE;
    }
    if (!a) return FALSE;
    if (msg == WM_COMMAND) { a->ControlsCommand(LOWORD(wp), HIWORD(wp)); return TRUE; }
    if (msg == WM_TIMER) a->RefreshControls();
    if (msg == WM_CLOSE) { DestroyWindow(dlg); return TRUE; }
    if (msg == WM_DESTROY) { KillTimer(dlg, 1); a->controlsDialog = nullptr; }
    return FALSE;
}
void App::OpenControls() {
    if (!controlsDialog) CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_CONTROLS), hwnd, ControlsProc, (LPARAM)this);
    if (controlsDialog) { ShowWindow(controlsDialog, SW_SHOW); SetForegroundWindow(controlsDialog); }
}
void App::RefreshControls() {
    int engine = pmActive ? 2 : shaderActive ? 1 : 0;
    CheckDlgButton(controlsDialog, IDC_CONTROLS_PIN, deckPinned ? BST_CHECKED : BST_UNCHECKED);
    if (engine != controlsEngine) {
        for (int i = 0; i < DK_N; ++i) {
            DestroyWindow(GetDlgItem(controlsDialog, IDC_CONTROL_BASE + i));
            DestroyWindow(GetDlgItem(controlsDialog, IDC_CONTROL_BASE + DK_N + i));
        }
        RECT rc; GetClientRect(controlsDialog, &rc);
        float sc = GetDpiForWindow(controlsDialog) / 96.0f;
        int colW = (rc.right - 24) / 3, rowH = (int)(54 * sc), count = 0;
        HFONT font = (HFONT)SendMessageW(controlsDialog, WM_GETFONT, 0, 0);
        for (int i = 0; i < DK_N; ++i) {
            if (DeckHidden(i) || i == DK_CONTROLS || i == DK_PIN) continue;
            int x = 12 + (count % 3) * colW, y = (int)(52 * sc) + (count / 3) * rowH; ++count;
            bool combo = WsControlCombo(i, pmActive);
            std::wstring label = DeckLabel(i);
            if (i == DK_PREV) label = L"Previous visual";
            if (i == DK_NEXT) label = L"Next visual";
            if (i == DK_HELP) label = L"Help (F1)";
            if (i <= DK_SPOT) label = L"Source: " + label;
            if (i == DK_PICK && pmActive) label = L"Search presets / favorites (F3)";
            if (combo) {
                const wchar_t* name = i == DK_DISP ? L"Display" : i == DK_MORPHT ? L"Morph target" : DeckDescriptor(i == DK_PICK ? DK_PREV : i);
                HWND caption = CreateWindowW(L"STATIC", name, WS_CHILD | WS_VISIBLE,
                    x, y, colW - 10, (int)(18 * sc), controlsDialog, (HMENU)(INT_PTR)(IDC_CONTROL_BASE + DK_N + i), inst, nullptr);
                SendMessageW(caption, WM_SETFONT, (WPARAM)font, TRUE);
            }
            HWND control = CreateWindowW(combo ? L"COMBOBOX" : L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                (combo ? CBS_DROPDOWNLIST | WS_VSCROLL : BS_PUSHBUTTON), x, y + (combo ? (int)(18 * sc) : 0), colW - 10,
                (int)((combo ? 220 : 34) * sc), controlsDialog, (HMENU)(INT_PTR)(IDC_CONTROL_BASE + i), inst, nullptr);
            SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
            if (combo) for (int k = 0; k < MenuCount(i); ++k) SendMessageW(control, CB_ADDSTRING, 0, (LPARAM)MenuItem(i, k));
        }
        controlsEngine = engine;
    }
    for (int i = 0; i < DK_N; ++i) {
        HWND control = GetDlgItem(controlsDialog, IDC_CONTROL_BASE + i);
        if (!control) continue;
        if (WsControlCombo(i, pmActive)) {
            if (!SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0)) SendMessageW(control, CB_SETCURSEL, MenuCurrent(i), 0);
        } else SendMessageW(control, BM_SETSTATE, DeckActive(i), 0);
    }
}
void App::ControlsCommand(int id, int code) {
    if (id == IDCANCEL || id == IDOK) { DestroyWindow(controlsDialog); return; }
    if (id == IDC_CONTROLS_PIN) {
        deckPinned = IsDlgButtonChecked(controlsDialog, id) == BST_CHECKED; SaveConfig(); return;
    }
    int i = id - IDC_CONTROL_BASE;
    if (i < 0 || i >= DK_N || DeckHidden(i)) return;
    if (WsControlCombo(i, pmActive)) {
        if (code != CBN_SELCHANGE) return;
        int selection = (int)SendDlgItemMessageW(controlsDialog, id, CB_GETCURSEL, 0, 0);
        if (selection >= 0 && selection < MenuCount(i) && !MenuDisabled(i, selection)) MenuSelect(i, selection);
    } else if (code == BN_CLICKED) DeckDispatch(i);
    RefreshControls();
}
