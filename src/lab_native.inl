static std::wstring WsLabLines(const std::wstring& text) {
    std::wstring result;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\r') { result += L'\n'; if (i + 1 < text.size() && text[i + 1] == L'\n') ++i; }
        else result += text[i];
    }
    return result;
}
static LRESULT CALLBACK LabEditProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = (App*)GetWindowLongPtrW(edit, GWLP_USERDATA);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) { a->CloseLab(); return 0; }
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == VK_RETURN) { a->SyncLabEditor(); a->LabApply(); a->RefreshLabStatus(); return 0; }
            if (wp == 'S') { a->SaveNativeLab(); return 0; }
            if (wp == 'A') { SendMessageW(edit, EM_SETSEL, 0, -1); return 0; }
        }
    }
    return CallWindowProcW(a->labEditProc, edit, msg, wp, lp);
}
static INT_PTR CALLBACK LabDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_INITDIALOG) {
        a = (App*)lp; SetWindowLongPtrW(dlg, GWLP_USERDATA, lp); a->labDialog = dlg;
        HWND edit = GetDlgItem(dlg, IDC_LAB_EDITOR);
        a->labSync = true;
        SendMessageW(edit, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO, 0);
        SendMessageW(edit, EM_EXLIMITTEXT, 0, 1024 * 1024);
        SendMessageW(edit, EM_SETEVENTMASK, 0, ENM_CHANGE);
        a->labNativeFont = CreateFontW(-MulDiv(14, GetDpiForWindow(dlg), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        SendMessageW(edit, WM_SETFONT, (WPARAM)a->labNativeFont, TRUE);
        SetWindowTextW(edit, a->labText.c_str()); SendMessageW(edit, EM_EMPTYUNDOBUFFER, 0, 0);
        SetDlgItemTextW(dlg, IDC_LAB_NAME, a->labSaveName.c_str());
        SendDlgItemMessageW(dlg, IDC_LAB_NAME, EM_SETLIMITTEXT, 120, 0);
        SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)a);
        a->labEditProc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)LabEditProc);
        a->labSync = false; a->RefreshLabStatus();
        SetFocus(edit); return FALSE;
    }
    if (!a) return FALSE;
    if (msg == WM_SIZE) {
        RECT rc; GetClientRect(dlg, &rc); float s = GetDpiForWindow(dlg) / 96.0f;
        auto move = [&](int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(dlg,id),x,y,w,h,TRUE); };
        int pad = (int)(16*s), bottom = rc.bottom - (int)(110*s), buttonY = rc.bottom - (int)(36*s);
        move(IDC_LAB_EDITOR,pad,(int)(40*s),rc.right-2*pad,(std::max)(40,bottom-(int)(40*s)));
        move(IDC_LAB_STATUS,pad,bottom+6,rc.right-2*pad,(int)(28*s));
        move(IDC_LAB_NAME,(int)(190*s),rc.bottom-(int)(70*s),rc.right-(int)(206*s),(int)(26*s));
        move(IDC_LAB_NAME_LABEL,pad,rc.bottom-(int)(66*s),(int)(170*s),(int)(20*s));
        move(IDC_LAB_APPLY,pad,buttonY,(int)(180*s),(int)(28*s));
        move(IDC_LAB_SAVE,(int)(206*s),buttonY,(int)(208*s),(int)(28*s));
        move(IDCANCEL,rc.right-(int)(126*s),buttonY,(int)(110*s),(int)(28*s));
        return TRUE;
    }
    if (msg == WM_GETMINMAXINFO) {
        UINT dpi = GetDpiForWindow(dlg);
        ((MINMAXINFO*)lp)->ptMinTrackSize = {MulDiv(650,dpi,96),MulDiv(450,dpi,96)};
        return TRUE;
    }
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == IDCANCEL) a->CloseLab();
        else if (id == IDC_LAB_EDITOR && HIWORD(wp) == EN_CHANGE && !a->labSync) { a->SyncLabEditor(); a->RefreshLabStatus(); }
        else if (id == IDC_LAB_APPLY) { a->SyncLabEditor(); a->LabApply(); a->RefreshLabStatus(); }
        else if (id == IDC_LAB_SAVE) a->SaveNativeLab();
        return TRUE;
    }
    if (msg == WM_CLOSE) { a->CloseLab(); return TRUE; }
    if (msg == WM_DESTROY) { a->labDialog = nullptr; a->labPanel = false; EnableWindow(a->hwnd, TRUE); }
    if (msg == WM_NCDESTROY) { if (a->labNativeFont) DeleteObject(a->labNativeFont); a->labNativeFont = nullptr; }
    return FALSE;
}
void App::OpenLab() {
    if (labDialog) { CloseLab(); return; }
    if (!pmActive || !pmHost.ready) return;
    static HMODULE richEdit = LoadLibraryW(L"Msftedit.dll");
    if (!richEdit) return;
    if (presetDialog) DestroyWindow(presetDialog);
    if (controlsDialog) DestroyWindow(controlsDialog);
    if (transportDialog) DestroyWindow(transportDialog);
    deckMenu = -1; palPanel = false; help = false;
    labText = WsLabLines(pmHost.CurrentPresetText()); labSavedText = labText;
    labSeedName = pmHost.PresetName();
    labSaveName = labSeedName;
    if (labSaveName.size() >= 5) labSaveName.resize(labSaveName.size() - 5);
    labSaveName += L" edit";
    labStatus = labText.empty() ? L"The source file is unavailable. Choose another preset." : L"Ctrl+Enter applies; Ctrl+S saves a new preset.";
    labStatusErr = labText.empty(); labPanel = true;
    if (!CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_LAB), hwnd, LabDialogProc, (LPARAM)this)) { labPanel = false; return; }
    EnableWindow(hwnd, FALSE); ShowWindow(labDialog, SW_SHOW);
}
void App::SyncLabEditor() {
    if (!labDialog || labSync) return;
    labText = WsLabLines(WsControlText(GetDlgItem(labDialog, IDC_LAB_EDITOR)));
    labSaveName = WsControlText(GetDlgItem(labDialog, IDC_LAB_NAME));
}
void App::RefreshLabStatus() {
    if (!labDialog) return;
    std::wstring title = L"WaveScope Preset Lab - " + labSeedName + (labText != labSavedText ? L" *" : L"");
    SetWindowTextW(labDialog, title.c_str()); SetDlgItemTextW(labDialog, IDC_LAB_STATUS, labStatus.c_str());
}
bool App::SaveNativeLab() {
    SyncLabEditor();
    if (WideToUtf8(labText).size() >= (1 << 20)) { labStatus = L"Preset exceeds the 1 MiB load limit."; labStatusErr = true; }
    else LabSaveAs();
    if (!labStatusErr) labSavedText = labText;
    RefreshLabStatus(); return !labStatusErr;
}
bool App::CloseLab(int choice) {
    if (!labDialog) return true;
    SyncLabEditor();
    if (labText != labSavedText) {
        if (!choice) choice = MessageBoxW(labDialog, L"Save your edits as a new preset before closing?\nYes saves using the name field; No discards changes; Cancel keeps editing.", L"Unsaved preset changes", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL || (choice == IDYES && !SaveNativeLab())) return false;
    }
    DestroyWindow(labDialog); lastShuffle = Now(); ArmVj(); return true;
}
