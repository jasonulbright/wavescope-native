// Spotify one-button: press it and Spotify starts playing, which the loopback
// tap already visualizes. Sign-in is the Authorization Code flow with a code
// challenge (no client secret — the right flow for a serverless native app):
// the browser signs in once, the return to http://127.0.0.1:8888/callback is
// caught on a raw socket, and tokens are fetched over WinHTTP. The refresh
// token is kept through the OS secret store (tied to the Windows user) in
// wavescope.ini, so signing in again is rare and the stored login isn't
// portable or readable by other accounts.
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
#include <mutex>
#include "spotify_helpers.inl"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#pragma comment(lib, "windowsapp.lib")

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ws2_32.lib")

// ---- small encoding / crypto helpers (single TU, so file-static is fine) ----

static std::wstring SpWiden(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string SpNarrow(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n > 0 ? n : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), a.data(), n, nullptr, nullptr);
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
static std::string SpHex(const BYTE* p, size_t n) {
    static const char* h = "0123456789abcdef"; std::string o;
    for (size_t i = 0; i < n; i++) { o += h[p[i] >> 4]; o += h[p[i] & 15]; }
    return o;
}
static bool SpSha256(const std::string& in, BYTE out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0 &&
              BCryptHashData(h, (PUCHAR)in.data(), (ULONG)in.size(), 0) >= 0 &&
              BCryptFinishHash(h, out, 32, 0) >= 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
static std::string SpRandToken() {
    BYTE b[48] = {};
    if (BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
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
    WinHttpSetTimeouts(s, 3000, 5000, 5000, 5000);
    // Don't follow redirects: Spotify's token/API endpoints answer in place,
    // so an unexpected redirect should fail loudly rather than re-send the
    // Authorization header somewhere else.
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
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
            const ULONGLONG deadline = GetTickCount64() + 15000;
            char chunk[8192]; DWORD rd = 0;
            for (;;) {
                if (GetTickCount64() >= deadline || !WinHttpReadData(q, chunk, sizeof(chunk), &rd)) {
                    r = {}; break;
                }
                if (!rd) break;
                if (r.body.size() + rd > 1024 * 1024) { r = {}; break; }
                r.body.append(chunk, rd);
            }
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return r;
}

// Use the OS JSON parser: nested objects, key ordering and Unicode escapes
// are part of the API contract, and must not be parsed by substring searches.
static std::string SpJson(const std::string& body, const char* key) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonObject obj;
        if (!JsonObject::TryParse(SpWiden(body), obj)) return {};
        auto value = obj.GetNamedValue(SpWiden(key), nullptr);
        if (!value) return {};
        if (value.ValueType() == JsonValueType::String) return SpNarrow(std::wstring(value.GetString()));
        return {}; // OAuth token fields must be strings, never null/scalars
    } catch (const winrt::hresult_error&) { return {}; }
}
static std::string SpFirstDeviceId(const std::string& body) {
    using namespace winrt::Windows::Data::Json;
    try {
        auto obj = JsonObject::Parse(SpWiden(body));
        auto devices = obj.GetNamedArray(L"devices");
        std::string fallback;
        for (auto value : devices) {
            auto dev = value.GetObject();
            if (dev.GetNamedBoolean(L"is_restricted", false)) continue;
            auto id = dev.GetNamedString(L"id", L"");
            if (id.empty()) continue;
            std::string candidate = SpNarrow(std::wstring(id));
            if (dev.GetNamedBoolean(L"is_active", false)) return candidate;
            if (fallback.empty()) fallback = candidate;
        }
        return fallback;
    } catch (const winrt::hresult_error&) { return {}; }
}
static std::wstring SpTrackText(const std::string& body) {
    using namespace winrt::Windows::Data::Json;
    try {
        auto obj = JsonObject::Parse(SpWiden(body));
        if (!obj.GetNamedBoolean(L"is_playing", false)) return {};
        auto item = obj.GetNamedObject(L"item");
        std::wstring track(item.GetNamedString(L"name"));
        auto artists = item.GetNamedArray(L"artists", nullptr);
        if (artists && artists.Size()) {
            std::wstring artist(artists.GetObjectAt(0).GetNamedString(L"name", L""));
            if (!artist.empty()) track += L" \x2014 " + artist;
        }
        return track;
    } catch (const winrt::hresult_error&) { return {}; }
}

// Extract a query parameter from the request LINE only (never the headers),
// so only the request line's own code/state is read.
// Bind the sign-in return listener on 127.0.0.1:8888. Must succeed BEFORE the
// browser opens: an already-signed-in account returns instantly, and the
// listener has to be there to catch it. Bound so only this process holds the
// port for the duration of the flow.
static SOCKET SpListen(unsigned short port = 8888) {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return srv;
    BOOL yes = TRUE;
    setsockopt(srv, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&yes, sizeof(yes));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // this machine only, never 0.0.0.0
    if (bind(srv, (sockaddr*)&a, sizeof(a)) != 0 || listen(srv, 1) != 0) {
        closesocket(srv);
        return INVALID_SOCKET;
    }
    return srv;
}

// Accept connections until one carries a code with the expected `state`
// (a stray local connection must not kill a pending sign-in), the 3-minute
// deadline passes, or the app is shutting down. Responses are static; nothing
// from the request is ever echoed back.
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
        DWORD sendTimeout = 500;
        setsockopt(cS, SOL_SOCKET, SO_SNDTIMEO, (char*)&sendTimeout, sizeof(sendTimeout));
        std::string req;
        ULONGLONG requestDeadline = GetTickCount64() + 2000;
        while (req.find("\r\n") == std::string::npos && req.size() < 4096 &&
               !quit.load() && GetTickCount64() < requestDeadline) {
            fd_set incoming; FD_ZERO(&incoming); FD_SET(cS, &incoming);
            timeval wait = {0, 100000};
            if (select(0, &incoming, nullptr, nullptr, &wait) <= 0) continue;
            char buf[1024]; int n = recv(cS, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, n);
        }
        if (req.size() <= 4096 && !quit.load()) {
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
    ~SpotifyClient() { Shutdown(); }
    std::string clientId; // public OAuth client ID supplied in Options
    std::string redirect = "http://127.0.0.1:8888/callback";
    std::string scopes   = "user-modify-playback-state user-read-playback-state user-read-currently-playing";
    std::string accessToken, refreshToken;   // guarded by tokLock
    std::atomic<bool> busy{ false };
    std::atomic<bool> quitFlag{ false };
    std::thread worker;
    std::mutex sessionLock; // serialize refresh rotation, sign-in, and config changes
    std::wstring status;
    std::atomic<ULONGLONG> statusAt{0};
    // Now-playing readout for the HUD, refreshed by a poller thread.
    std::thread poller;
    std::atomic<bool> pollerOn{ false };
    SRWLOCK tokLock = SRWLOCK_INIT;
    SRWLOCK npLock = SRWLOCK_INIT;
    std::wstring npText;

    std::wstring IniPath() const { return WsDataDir() + L"wavescope.ini"; }

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

    std::string Protect(const std::string& in, bool enc) {
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
        std::string blob = Protect(rt, true);
        if (blob.empty()) { Notify(L"Windows could not encrypt the Spotify login. It was not saved."); return; }
        std::wstring hx = SpWiden(SpHex((const BYTE*)blob.data(), blob.size()));
        AcquireSRWLockExclusive(&g_iniLock);
        bool saved = WritePrivateProfileStringW(L"spotify", L"refresh", hx.c_str(), IniPath().c_str()) != 0;
        ReleaseSRWLockExclusive(&g_iniLock);
        if (!saved) Notify(L"Could not save the Spotify login. Check that the app folder is writable.");
    }
    void LoadToken() {
        std::wstring path = IniPath();
        wchar_t cid[128] = {};
        GetPrivateProfileStringW(L"spotify", L"clientId", L"", cid, 128, path.c_str());
        clientId = SpNarrow(cid);
        if (!SpValidClientId(clientId)) { clientId.clear(); return; }
        wchar_t buf[4096];
        GetPrivateProfileStringW(L"spotify", L"refresh", L"", buf, 4096, path.c_str());
        if (buf[0]) {
            std::string rt = Protect(SpUnHex(SpNarrow(buf)), false);
            AcquireSRWLockExclusive(&tokLock);
            refreshToken = rt;
            ReleaseSRWLockExclusive(&tokLock);
            if (!rt.empty()) StartPoller();   // resume the HUD readout silently
        }

    }

    void Notify(const std::wstring& m) {
        // Never display a modal box on a worker that shutdown must join.
        AcquireSRWLockExclusive(&npLock);
        status = m;
        statusAt = GetTickCount64();
        for (wchar_t& c : status) if (c == L'\n' || c == L'\r') c = L' ';
        ReleaseSRWLockExclusive(&npLock);
    }
    std::wstring Status() {
        AcquireSRWLockShared(&npLock);
        std::wstring s = status;
        ReleaseSRWLockShared(&npLock);
        return s;
    }
    bool StatusVisible() const { return statusAt.load() != 0 && GetTickCount64() - statusAt.load() < 15000; }
    bool Configure(const std::string& id) {
        if (!id.empty() && !SpValidClientId(id)) return false;
        std::unique_lock<std::mutex> guard(sessionLock, std::try_to_lock);
        if (!guard.owns_lock()) {
            Notify(L"Spotify is busy. Wait for the current request, then save again.");
            return false;
        }
        AcquireSRWLockExclusive(&g_iniLock);
        bool ok = true;
        // Changing the app or explicitly disconnecting invalidates its login.
        if (ok && (id != clientId || id.empty()))
            ok = WritePrivateProfileStringW(L"spotify", L"refresh", nullptr, IniPath().c_str()) != 0;
        if (ok) ok = WritePrivateProfileStringW(L"spotify", L"clientId", SpWiden(id).c_str(), IniPath().c_str()) != 0;
        ReleaseSRWLockExclusive(&g_iniLock);
        if (!ok) { Notify(L"Could not save wavescope.ini. Check that the app folder is writable."); return false; }
        if (id != clientId || id.empty()) {
            AcquireSRWLockExclusive(&tokLock);
            accessToken.clear(); refreshToken.clear();
            ReleaseSRWLockExclusive(&tokLock);
            SetNowPlaying(L"");
        }
        clientId = id;
        Notify(id.empty() ? L"Spotify disconnected." : L"Client ID saved. Press B to connect.");
        return true;
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

    bool DoSignIn() {
        std::string verifier = SpRandToken();
        BYTE hash[32] = {};
        if (verifier.empty() || !SpSha256(verifier, hash)) return false;
        std::string challenge = SpBase64Url(hash, 32);
        std::string state = SpRandToken();
        if (state.empty()) return false;

        // Listener FIRST, then the browser: an already-signed-in account can
        // bounce off /authorize and return within milliseconds.
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
        return DoSignIn();
    }

    // ---- now-playing (HUD readout) ----

    // Read the track and artist from their actual JSON objects.
    void UpdateNowPlaying() {
        SpResp r = Player(L"GET", L"/v1/me/player/currently-playing", "");
        if (r.status == 401) {
            if (!RefreshAccess()) { SetNowPlaying(L""); return; }
            r = Player(L"GET", L"/v1/me/player/currently-playing", "");
        }
        if (r.status != 200 || r.body.empty()) { SetNowPlaying(L""); return; }
        std::wstring w = SpTrackText(r.body);
        if (w.size() > 60) { w.resize(59); w += L"\x2026"; }
        SetNowPlaying(w);
    }

    // Poll every 10 s while a session exists. Never opens the browser: it
    // only refreshes with the stored token, so it's silent at startup.
    void StartPoller() {
        if (pollerOn.exchange(true)) return;
        poller = std::thread([this] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            int tick = 0;
            while (!quitFlag.load()) {
                if (tick == 0) {
                    std::lock_guard<std::mutex> guard(sessionLock);
                    if (!quitFlag.load() && (!Bearer().empty() || RefreshAccess())) UpdateNowPlaying();
                    tick = 20;           // 20 x 500 ms = 10 s cadence
                }
                tick--;
                Sleep(500);
            }
            winrt::uninit_apartment();
        });
    }

    bool StartPlayback() {
        SpResp dev = Player(L"GET", L"/v1/me/player/devices", "");
        if (dev.status == 401 && (RefreshAccess() || DoSignIn()))
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
        std::string quoted = SpNarrow(std::wstring(winrt::Windows::Data::Json::JsonValue::CreateStringValue(SpWiden(id)).Stringify()));
        std::string body = "{\"device_ids\":[" + quoted + "],\"play\":true}";
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
        if (!quitFlag.load() && StartPlayback()) Notify(L"Spotify connected.");
    }

    // The one button — runs the whole flow on a worker thread so the render
    // loop never stalls waiting on the browser or the network.
    void OneButton() {
        if (busy.exchange(true)) return;
        if (worker.joinable()) worker.join();   // reap the previous, finished flow
        worker = std::thread([this] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            std::lock_guard<std::mutex> guard(sessionLock);
            if (SpValidClientId(clientId)) { Notify(L"Connecting to Spotify..."); Flow(); }
            else Notify(L"Open Options (F2) to enter your Spotify client ID.");
            busy = false;
            winrt::uninit_apartment();
        });
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
