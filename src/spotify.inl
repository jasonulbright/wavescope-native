// Spotify one-button: press it and Spotify starts playing, which the loopback
// tap already visualizes. OAuth2 Authorization Code + PKCE (no client secret —
// the right flow for a serverless native app): the browser authorizes once,
// the redirect to http://127.0.0.1:8888/callback is caught on a raw socket,
// and tokens are fetched over WinHTTP. The refresh token is DPAPI-encrypted
// (tied to the Windows user) in wavescope.ini, so re-auth is rare and the
// stored token isn't portable or readable by other accounts.
//
// Playback control (the Player endpoints) is Spotify Premium-only; loopback
// capture of whatever is already playing needs none of this.

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <string>
#include <thread>
#include <atomic>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ws2_32.lib")

// ---- small encoding / crypto helpers (single TU, so file-static is fine) ----

static std::wstring SpWiden(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string SpNarrow(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string a(n > 0 ? n - 1 : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &a[0], n, nullptr, nullptr);
    return a;
}

static std::string SpUrlEncode(const std::string& s) {
    static const char* h = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += h[c >> 4]; o += h[c & 15]; }
    }
    return o;
}
static std::string SpUrlDecode(const std::string& s) {
    auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += (char)((hv(s[i + 1]) << 4) | hv(s[i + 2])); i += 2; }
        else if (s[i] == '+') o += ' ';
        else o += s[i];
    }
    return o;
}

static std::string SpBase64Url(const BYTE* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string o; int val = 0, bits = -6;
    for (size_t i = 0; i < n; i++) {
        val = (val << 8) + p[i]; bits += 8;
        while (bits >= 0) { o += t[(val >> bits) & 0x3F]; bits -= 6; }
    }
    if (bits > -6) o += t[((val << 8) >> (bits + 8)) & 0x3F];
    return o; // URL-safe, unpadded (RFC 7636)
}
static std::string SpHex(const BYTE* p, size_t n) {
    static const char* h = "0123456789abcdef"; std::string o;
    for (size_t i = 0; i < n; i++) { o += h[p[i] >> 4]; o += h[p[i] & 15]; }
    return o;
}
static std::string SpUnHex(const std::string& s) {
    auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    std::string o;
    for (size_t i = 0; i + 1 < s.size(); i += 2) o += (char)((hv(s[i]) << 4) | hv(s[i + 1]));
    return o;
}

static void SpSha256(const std::string& in, BYTE out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(h, (PUCHAR)in.data(), (ULONG)in.size(), 0);
    BCryptFinishHash(h, out, 32, 0);
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(alg, 0);
}
static std::string SpRandVerifier() {
    BYTE b[48] = {};
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return SpBase64Url(b, sizeof(b)); // 64 chars, within the 43-128 range
}

// ---- minimal HTTPS + JSON + localhost listener ----

struct SpResp { DWORD status = 0; std::string body; };

static SpResp SpHttps(const wchar_t* host, const wchar_t* verb, const wchar_t* path,
                      const std::string& body, const std::string& ctype, const std::string& bearer) {
    SpResp r;
    HINTERNET s = WinHttpOpen(L"WaveScope/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return r;
    // Bounded waits so a hung request can't stall app shutdown behind a join.
    WinHttpSetTimeouts(s, 5000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, verb, path, nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (q) {
        std::wstring hdr;
        if (!ctype.empty())  hdr += L"Content-Type: " + SpWiden(ctype) + L"\r\n";
        if (!bearer.empty()) hdr += L"Authorization: Bearer " + SpWiden(bearer) + L"\r\n";
        BOOL ok = WinHttpSendRequest(q,
            hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
            hdr.empty() ? 0 : (DWORD)-1L,
            (LPVOID)(body.empty() ? nullptr : body.data()), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok && WinHttpReceiveResponse(q, nullptr)) {
            DWORD sc = 0, l = sizeof(sc);
            WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &sc, &l, WINHTTP_NO_HEADER_INDEX);
            r.status = sc;
            DWORD avail;
            do {
                avail = 0; WinHttpQueryDataAvailable(q, &avail);
                if (avail) { std::string ch(avail, 0); DWORD rd = 0;
                    WinHttpReadData(q, &ch[0], avail, &rd); ch.resize(rd); r.body += ch; }
            } while (avail > 0);
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return r;
}

// Extract a JSON string/scalar value for `key` (flat scan; good enough here).
static std::string SpJson(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k); if (p == std::string::npos) return "";
    p = j.find(':', p + k.size()); if (p == std::string::npos) return "";
    p++; while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) p++;
    if (p < j.size() && j[p] == '"') {
        p++; std::string out;
        while (p < j.size() && j[p] != '"') {
            if (j[p] == '\\' && p + 1 < j.size()) { out += j[p + 1]; p += 2; }
            else out += j[p++];
        }
        return out;
    }
    size_t e = p;
    while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != ']') e++;
    return j.substr(p, e - p);
}
static std::string SpFirstDeviceId(const std::string& j) {
    size_t d = j.find("\"devices\"");
    size_t p = j.find("\"id\"", d == std::string::npos ? 0 : d);
    if (p == std::string::npos) return "";
    std::string id = SpJson(j.substr(p), "id");
    return id == "null" ? "" : id;
}

// Extract a query parameter from the request LINE only (never the headers),
// so header content can't spoof code/state.
static std::string SpQueryParam(const std::string& req, const char* key) {
    size_t eol = req.find("\r\n");
    std::string line = req.substr(0, eol == std::string::npos ? req.size() : eol);
    std::string k = std::string(key) + "=";
    size_t p = line.find('?'); if (p == std::string::npos) return "";
    p = line.find(k, p); if (p == std::string::npos) return "";
    p += k.size(); size_t e = p;
    while (e < line.size() && line[e] != '&' && line[e] != ' ' && line[e] != '#') e++;
    return SpUrlDecode(line.substr(p, e - p));
}

// Bind the OAuth redirect listener on 127.0.0.1:8888. Must succeed BEFORE the
// browser opens: an already-consented account redirects instantly, and the
// listener has to be there to catch it. Exclusive bind so no other local
// process can steal the port mid-flow.
static SOCKET SpListen() {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return srv;
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&yes, sizeof(yes));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(8888);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only, never 0.0.0.0
    if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 1) != 0) {
        closesocket(srv);
        return INVALID_SOCKET;
    }
    return srv;
}

// Accept connections until one carries a code with the expected CSRF `state`
// (a stray local connection must not kill a pending login), the 3-minute
// deadline passes, or the app is shutting down. Responses are static; nothing
// attacker-controlled is ever echoed back.
static bool SpWaitForCode(SOCKET srv, std::string& code, const std::string& expectState,
                          std::atomic<bool>& quit) {
    bool ok = false;
    ULONGLONG deadline = GetTickCount64() + 180000;
    while (!ok && !quit.load() && GetTickCount64() < deadline) {
        fd_set rs; FD_ZERO(&rs); FD_SET(srv, &rs);
        timeval tv = { 0, 500000 };   // 500 ms tick keeps shutdown responsive
        if (select(0, &rs, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET cS = accept(srv, nullptr, nullptr);
        if (cS == INVALID_SOCKET) continue;
        char buf[4096];
        int n = recv(cS, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string req(buf, (size_t)n);
            std::string c = SpQueryParam(req, "code");
            if (!c.empty() && SpQueryParam(req, "state") == expectState) {
                code = c;
                ok = true;
            }
        }
        const char* resp = ok
            ? "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
              "<html><body style='font-family:Segoe UI,sans-serif;background:#0b0d0c;"
              "color:#e8eae8;text-align:center;padding-top:12%'><h2>WaveScope connected</h2>"
              "<p>You can close this tab and return to the visualizer.</p></body></html>"
            : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\nWaveScope auth failed.";
        send(cS, resp, (int)strlen(resp), 0);
        closesocket(cS);
    }
    return ok;
}

// ---- the client ----

// Serializes every write to wavescope.ini (the Spotify worker thread and the
// UI thread both persist through the profile APIs).
static SRWLOCK g_iniLock = SRWLOCK_INIT;

struct SpotifyClient {
    std::string clientId = "89df5ee986864e558952ba356aa968ea";
    std::string redirect = "http://127.0.0.1:8888/callback";
    std::string scopes   = "user-modify-playback-state user-read-playback-state user-read-currently-playing";
    std::string accessToken, refreshToken;   // guarded by tokLock
    std::atomic<bool> busy{ false };
    std::atomic<bool> quitFlag{ false };
    std::thread worker;
    // Now-playing readout for the HUD, refreshed by a poller thread.
    std::thread poller;
    std::atomic<bool> pollerOn{ false };
    SRWLOCK tokLock = SRWLOCK_INIT;
    SRWLOCK npLock = SRWLOCK_INIT;
    std::wstring npText;

    std::wstring IniPath() const { return PmExeDir() + L"wavescope.ini"; }

    std::string Bearer() {
        AcquireSRWLockShared(&tokLock);
        std::string t = accessToken;
        ReleaseSRWLockShared(&tokLock);
        return t;
    }
    void SetNowPlaying(const std::wstring& s) {
        AcquireSRWLockExclusive(&npLock);
        npText = s;
        ReleaseSRWLockExclusive(&npLock);
    }
    std::wstring NowPlaying() {
        AcquireSRWLockShared(&npLock);
        std::wstring s = npText;
        ReleaseSRWLockShared(&npLock);
        return s;
    }

    std::string Dpapi(const std::string& in, bool enc) {
        DATA_BLOB i = { (DWORD)in.size(), (BYTE*)in.data() }, o = {};
        BOOL ok = enc ? CryptProtectData(&i, L"WaveScope", nullptr, nullptr, nullptr, 0, &o)
                      : CryptUnprotectData(&i, nullptr, nullptr, nullptr, nullptr, 0, &o);
        if (!ok) return "";
        std::string r((char*)o.pbData, o.cbData);
        LocalFree(o.pbData);
        return r;
    }
    void SaveToken() {
        AcquireSRWLockShared(&tokLock);
        std::string rt = refreshToken;
        ReleaseSRWLockShared(&tokLock);
        if (rt.empty()) return;
        std::string blob = Dpapi(rt, true);
        std::wstring hx = SpWiden(SpHex((const BYTE*)blob.data(), blob.size()));
        AcquireSRWLockExclusive(&g_iniLock);
        WritePrivateProfileStringW(L"spotify", L"refresh", hx.c_str(), IniPath().c_str());
        ReleaseSRWLockExclusive(&g_iniLock);
    }
    void LoadToken() {
        std::wstring path = IniPath();
        wchar_t buf[4096];
        GetPrivateProfileStringW(L"spotify", L"refresh", L"", buf, 4096, path.c_str());
        if (buf[0]) {
            std::string rt = Dpapi(SpUnHex(SpNarrow(buf)), false);
            AcquireSRWLockExclusive(&tokLock);
            refreshToken = rt;
            ReleaseSRWLockExclusive(&tokLock);
            if (!rt.empty()) StartPoller();   // resume the HUD readout silently
        }
        // Client ID lives in the ini (configurable, not baked in). Seed the
        // default on first run so it's there to edit; the ini is the source of
        // truth thereafter.
        wchar_t cid[128];
        GetPrivateProfileStringW(L"spotify", L"clientId", L"", cid, 128, path.c_str());
        if (cid[0]) clientId = SpNarrow(cid);
        else {
            AcquireSRWLockExclusive(&g_iniLock);
            WritePrivateProfileStringW(L"spotify", L"clientId", SpWiden(clientId).c_str(), path.c_str());
            ReleaseSRWLockExclusive(&g_iniLock);
        }
    }

    void Notify(const std::wstring& m) {
        MessageBoxW(nullptr, m.c_str(), L"WaveScope \x2014 Spotify",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
    }

    SpResp Player(const wchar_t* verb, const wchar_t* path, const std::string& body) {
        return SpHttps(L"api.spotify.com", verb, path, body,
                       body.empty() ? "" : "application/json", Bearer());
    }

    bool RefreshAccess() {
        AcquireSRWLockShared(&tokLock);
        std::string rt = refreshToken;
        ReleaseSRWLockShared(&tokLock);
        if (rt.empty()) return false;
        std::string body = "grant_type=refresh_token&refresh_token=" + SpUrlEncode(rt) +
                           "&client_id=" + SpUrlEncode(clientId);
        SpResp r = SpHttps(L"accounts.spotify.com", L"POST", L"/api/token", body,
                           "application/x-www-form-urlencoded", "");
        if (r.status != 200) return false;
        std::string at = SpJson(r.body, "access_token");
        std::string nr = SpJson(r.body, "refresh_token");
        AcquireSRWLockExclusive(&tokLock);
        accessToken = at;
        if (!nr.empty()) refreshToken = nr;
        ReleaseSRWLockExclusive(&tokLock);
        if (!nr.empty()) SaveToken();
        return !at.empty();
    }

    bool DoPkceAuth() {
        std::string verifier = SpRandVerifier();
        BYTE hash[32]; SpSha256(verifier, hash);
        std::string challenge = SpBase64Url(hash, 32);
        std::string state = SpRandVerifier().substr(0, 16);

        // Listener FIRST, then the browser: an already-consented account can
        // bounce off /authorize and hit the redirect within milliseconds.
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        SOCKET srv = SpListen();
        if (srv == INVALID_SOCKET) {
            WSACleanup();
            if (!quitFlag.load())
                Notify(L"Couldn't open the sign-in port (127.0.0.1:8888).\n"
                       L"Another program may be using it. Close it and try again.");
            return false;
        }

        std::string url = "https://accounts.spotify.com/authorize?client_id=" + SpUrlEncode(clientId) +
            "&response_type=code&redirect_uri=" + SpUrlEncode(redirect) +
            "&code_challenge_method=S256&code_challenge=" + challenge +
            "&scope=" + SpUrlEncode(scopes) + "&state=" + state;
        ShellExecuteW(nullptr, L"open", SpWiden(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        std::string code;
        bool got = SpWaitForCode(srv, code, state, quitFlag);
        closesocket(srv);
        WSACleanup();
        if (!got) return false;

        std::string body = "grant_type=authorization_code&code=" + SpUrlEncode(code) +
            "&redirect_uri=" + SpUrlEncode(redirect) + "&client_id=" + SpUrlEncode(clientId) +
            "&code_verifier=" + verifier;
        SpResp r = SpHttps(L"accounts.spotify.com", L"POST", L"/api/token", body,
                           "application/x-www-form-urlencoded", "");
        if (r.status != 200) return false;
        std::string at = SpJson(r.body, "access_token");
        std::string rt = SpJson(r.body, "refresh_token");
        AcquireSRWLockExclusive(&tokLock);
        accessToken = at;
        refreshToken = rt;
        ReleaseSRWLockExclusive(&tokLock);
        SaveToken();
        return !at.empty();
    }

    bool EnsureToken() {
        if (!Bearer().empty()) return true;
        if (RefreshAccess()) return true;
        return DoPkceAuth();
    }

    // ---- now-playing (HUD readout) ----

    // Best-effort parse of /currently-playing: within `item`, "duration_ms"
    // precedes the track-level "name", and the first "name" after "artists"
    // is the artist. Flat scans, only on the trusted API response.
    void UpdateNowPlaying() {
        SpResp r = Player(L"GET", L"/v1/me/player/currently-playing", "");
        if (r.status == 401) {
            if (!RefreshAccess()) { SetNowPlaying(L""); return; }
            r = Player(L"GET", L"/v1/me/player/currently-playing", "");
        }
        if (r.status != 200 || r.body.empty()) { SetNowPlaying(L""); return; }
        if (SpJson(r.body, "is_playing") != "true") { SetNowPlaying(L""); return; }
        size_t art = r.body.find("\"artists\"");
        std::string artist = art == std::string::npos ? "" : SpJson(r.body.substr(art), "name");
        size_t dur = r.body.find("\"duration_ms\"", art == std::string::npos ? 0 : art);
        std::string track = dur == std::string::npos ? "" : SpJson(r.body.substr(dur), "name");
        if (track.empty()) { SetNowPlaying(L""); return; }
        std::wstring w = SpWiden(track);
        if (!artist.empty()) w += L" \x2014 " + SpWiden(artist);
        if (w.size() > 60) { w.resize(59); w += L"\x2026"; }
        SetNowPlaying(w);
    }

    // Poll every 10 s while a session exists. Never opens the browser: it
    // only refreshes with the stored token, so it's silent at startup.
    void StartPoller() {
        if (pollerOn.exchange(true)) return;
        poller = std::thread([this] {
            int tick = 0;
            while (!quitFlag.load()) {
                if (tick == 0) {
                    if (!Bearer().empty() || RefreshAccess()) UpdateNowPlaying();
                    tick = 20;           // 20 x 500 ms = 10 s cadence
                }
                tick--;
                Sleep(500);
            }
        });
    }

    bool StartPlayback() {
        SpResp dev = Player(L"GET", L"/v1/me/player/devices", "");
        if (dev.status == 401 && (RefreshAccess() || DoPkceAuth()))
            dev = Player(L"GET", L"/v1/me/player/devices", "");
        if (dev.status != 200) {
            Notify(L"Couldn't reach Spotify. Playback control needs a Premium account.");
            return false;
        }
        std::string id = SpFirstDeviceId(dev.body);
        if (id.empty()) {
            Notify(L"No Spotify device is available.\n\nOpen Spotify on this PC (or your phone), "
                   L"play any track once, then press the button again.");
            return false;
        }
        // Transfer to the device with play:true = select it and start playing.
        std::string body = "{\"device_ids\":[\"" + id + "\"],\"play\":true}";
        SpResp pr = Player(L"PUT", L"/v1/me/player", body);
        if (pr.status == 401 && RefreshAccess()) pr = Player(L"PUT", L"/v1/me/player", body);
        if (pr.status == 204 || pr.status == 202 || pr.status == 200) return true; // playing
        if (pr.status == 403) {
            Notify(L"Spotify refused playback control.\nThis feature requires Spotify Premium.");
            return false;
        }
        Notify(L"Spotify playback couldn't start (HTTP " + std::to_wstring(pr.status) + L").");
        return false;
    }

    void Flow() {
        if (!EnsureToken()) {
            if (!quitFlag.load())
                Notify(L"Spotify sign-in didn't complete. Try the button again.");
            return;
        }
        StartPoller();       // begin/continue the now-playing HUD readout
        if (!quitFlag.load())
            StartPlayback(); // notifies on the device/premium cases; silent on success
    }

    // The one button — runs the whole flow on a worker thread so the render
    // loop never stalls waiting on the browser or the network.
    void OneButton() {
        if (busy.exchange(true)) return;
        if (worker.joinable()) worker.join();   // reap the previous, finished flow
        worker = std::thread([this] { Flow(); busy = false; });
    }

    // Joined at app exit so the flows can't run through CRT teardown; the
    // quit flag wakes the listener's and poller's 500 ms ticks.
    void Shutdown() {
        quitFlag = true;
        if (worker.joinable()) worker.join();
        if (poller.joinable()) poller.join();
    }
};

static SpotifyClient g_spotify;
