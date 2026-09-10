// A native dialog supplies keyboard navigation, paste, labels, and screen
// reader support for the optional Spotify configuration.
static INT_PTR CALLBACK OptionsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        RECT owner, box; GetWindowRect(GetParent(dlg), &owner); GetWindowRect(dlg, &box);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST), &mi);
        int w = box.right - box.left, h = box.bottom - box.top;
        int x = (owner.left + owner.right - w) / 2, y = (owner.top + owner.bottom - h) / 2;
        x = (std::max)(mi.rcWork.left, (std::min)((LONG)x, mi.rcWork.right - w));
        y = (std::max)(mi.rcWork.top, (std::min)((LONG)y, mi.rcWork.bottom - h));
        SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        SetDlgItemTextW(dlg, IDC_SP_REDIRECT, L"http://127.0.0.1:8888/callback");
        SetDlgItemTextW(dlg, IDC_SP_CLIENT, SpWiden(g_spotify.clientId).c_str());
        SendDlgItemMessageW(dlg, IDC_SP_CLIENT, EM_SETLIMITTEXT, 127, 0);
        SetDlgItemTextW(dlg, IDC_SP_STATUS, L"Your client ID is public. Stored login tokens are encrypted for your Windows account.");
        SetFocus(GetDlgItem(dlg, IDC_SP_CLIENT));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_SP_DISCONNECT) {
            wchar_t buf[128] = {};
            if (LOWORD(wp) == IDOK) GetDlgItemTextW(dlg, IDC_SP_CLIENT, buf, 128);
            std::string id = SpNarrow(buf);
            size_t first = id.find_first_not_of(" \t\r\n"), last = id.find_last_not_of(" \t\r\n");
            id = first == std::string::npos ? "" : id.substr(first, last - first + 1);
            if (!id.empty() && !SpValidClientId(id)) {
                SetDlgItemTextW(dlg, IDC_SP_STATUS, L"Enter the 32 hexadecimal characters from your Spotify app's Client ID, or leave it blank to disconnect.");
                SetFocus(GetDlgItem(dlg, IDC_SP_CLIENT));
                return TRUE;
            }
            if (!g_spotify.Configure(id)) {
                SetDlgItemTextW(dlg, IDC_SP_STATUS, g_spotify.Status().c_str());
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    case WM_CLOSE: EndDialog(dlg, IDCANCEL); return TRUE;
    }
    return FALSE;
}
