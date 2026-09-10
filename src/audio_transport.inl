static std::wstring WsAudioTime(LONGLONG value) {
    LONGLONG seconds = (std::max)(0LL, value / 10000000);
    wchar_t text[40];
    if (seconds >= 3600) _snwprintf_s(text, _TRUNCATE, L"%lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else _snwprintf_s(text, _TRUNCATE, L"%lld:%02lld", seconds / 60, seconds % 60);
    return text;
}
static INT_PTR CALLBACK TransportProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = (App*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_INITDIALOG) {
        a = (App*)lp; SetWindowLongPtrW(dlg, GWLP_USERDATA, lp); a->transportDialog = dlg;
        a->transportDragging = false;
        SendDlgItemMessageW(dlg, IDC_AUDIO_SEEK, TBM_SETRANGE, TRUE, MAKELPARAM(0, 10000));
        SendDlgItemMessageW(dlg, IDC_AUDIO_SEEK, TBM_SETLINESIZE, 0, 100);
        SendDlgItemMessageW(dlg, IDC_AUDIO_SEEK, TBM_SETPAGESIZE, 0, 1000);
        a->RefreshTransport(); SetTimer(dlg, 1, 100, nullptr); return TRUE;
    }
    if (!a) return FALSE;
    if (msg == WM_HSCROLL && (HWND)lp == GetDlgItem(dlg, IDC_AUDIO_SEEK)) {
        int code = LOWORD(wp);
        a->transportDragging = code == TB_THUMBTRACK;
        if (!a->transportDragging) {
            auto f = g_tap.FileState();
            if (f && f->duration > 0) {
                int tick = (int)SendDlgItemMessageW(dlg, IDC_AUDIO_SEEK, TBM_GETPOS, 0, 0);
                g_tap.SeekFile((LONGLONG)((long double)f->duration.load() * tick / 10000));
            }
        }
        a->RefreshTransport(); return TRUE;
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case IDC_AUDIO_PLAY: g_tap.ToggleFilePause(); break;
        case IDC_AUDIO_LOOP: {
            auto f = g_tap.FileState(); if (f) f->loop = IsDlgButtonChecked(dlg, IDC_AUDIO_LOOP) == BST_CHECKED;
            break;
        }
        case IDC_AUDIO_OPEN: a->OpenFile(); break;
        case IDCANCEL: DestroyWindow(dlg); return TRUE;
        }
        a->RefreshTransport(); return TRUE;
    }
    if (msg == WM_TIMER) { a->RefreshTransport(); return TRUE; }
    if (msg == WM_CLOSE) { DestroyWindow(dlg); return TRUE; }
    if (msg == WM_DESTROY) { KillTimer(dlg, 1); a->transportDialog = nullptr; }
    return FALSE;
}
void App::OpenTransport() {
    if (!transportDialog) {
        INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
        CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_TRANSPORT), hwnd, TransportProc, (LPARAM)this);
    }
    if (transportDialog) { ShowWindow(transportDialog, SW_SHOW); SetForegroundWindow(transportDialog); }
}
void App::RefreshTransport() {
    if (!transportDialog) return;
    auto f = g_tap.FileState(); bool active = f && g_tap.Source() == AudioTap::SRC_FILE;
    bool ready = active && f->ready && SUCCEEDED(f->error.load());
    bool canSeek = ready && f->seekable;
    EnableWindow(GetDlgItem(transportDialog, IDC_AUDIO_PLAY), ready && (!f->ended || f->seekable));
    EnableWindow(GetDlgItem(transportDialog, IDC_AUDIO_SEEK), canSeek);
    EnableWindow(GetDlgItem(transportDialog, IDC_AUDIO_LOOP), canSeek);
    std::wstring filename = active ? f->path : L"No audio file selected";
    SetDlgItemTextW(transportDialog, IDC_AUDIO_FILE, filename.c_str());
    LONGLONG position = active ? f->position.load() : 0, duration = active ? f->duration.load() : 0;
    HWND slider = GetDlgItem(transportDialog, IDC_AUDIO_SEEK);
    if (transportDragging && canSeek)
        position = (LONGLONG)((long double)duration * SendMessageW(slider, TBM_GETPOS, 0, 0) / 10000);
    else if (GetCapture() != slider && (!active || (f->seek < 0 && !f->seeking))) {
        position = active ? f->position.load() : 0;
        SendMessageW(slider, TBM_SETPOS, TRUE, duration > 0 ? (LPARAM)((long double)position * 10000 / duration) : 0);
    }
    std::wstring time = WsAudioTime(position) + L" / " + (duration > 0 ? WsAudioTime(duration) : L"--:--");
    SetDlgItemTextW(transportDialog, IDC_AUDIO_TIME, time.c_str());
    SetDlgItemTextW(transportDialog, IDC_AUDIO_PLAY, active && f->ended ? L"&Replay" : active && f->paused ? L"&Play" : L"&Pause");
    CheckDlgButton(transportDialog, IDC_AUDIO_LOOP, active && f->loop ? BST_CHECKED : BST_UNCHECKED);
    std::wstring status;
    if (!active) status = L"Choose a file to play and visualize.";
    else if (FAILED(f->error.load())) {
        wchar_t error[150]; _snwprintf_s(error, _TRUNCATE, L"Playback failed (0x%08X). Check the file and audio output; choose a file to retry.", (unsigned)f->error.load());
        status = error;
    } else if (!f->ready) status = L"Opening audio file...";
    else if (f->ended) status = canSeek ? L"Finished. Replay starts from the beginning." : L"Finished. Choose the file again to replay.";
    else status = f->paused ? L"Paused" : L"Playing";
    if (ready && !canSeek) status += L" (Seeking unavailable for this file.)";
    SetDlgItemTextW(transportDialog, IDC_AUDIO_STATUS, status.c_str());
}
