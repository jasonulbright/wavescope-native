static void TestSpotifyProtocol() {
    CHECK(!SpValidClientId(""), "empty client ID is unconfigured");
    CHECK(SpValidClientId("0123456789abcdef0123456789abcdef"), "valid public client ID");
    CHECK(!SpValidClientId("0123456789abcdef0123456789abcdeg"), "reject nonhex client ID");
    CHECK(SpUrlDecode("a%2Bb+c") == "a+b c", "URL decoding");
    for (const char* input : {"%", "%0", "%GG", "%00", "%/0"})
        CHECK(SpUrlDecode(input).empty(), "reject malformed URL encoding");
    CHECK(SpUnHex("a").empty() && SpUnHex("gg").empty(), "reject corrupt token encoding");
    CHECK(SpUnHex("00FF") == std::string("\0\xff", 2), "binary hex roundtrip");
    BYTE hash[32] = {};
    CHECK(SpSha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", hash), "PKCE hash succeeds");
    CHECK(SpBase64Url(hash, 32) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", "RFC 7636 PKCE challenge");
    CHECK(SpWiden("").empty() && SpNarrow(L"").empty(), "empty UTF strings");
    CHECK(SpNarrow(SpWiden("\xf0\x9f\x8e\xb5")) == "\xf0\x9f\x8e\xb5", "Unicode roundtrip");
    const std::string good = "GET /callback?code=abc%2B123&state=expected HTTP/1.1\r\nHost: localhost\r\n\r\n";
    CHECK(SpQueryParam(good, "code") == "abc+123", "valid callback");
    for (const char* req : {
        "GET /callback?xcode=abc&state=expected HTTP/1.1\r\n",
        "POST /callback?code=abc&state=expected HTTP/1.1\r\n",
        "GET /other?code=abc&state=expected HTTP/1.1\r\n",
        "GET /callback?code=abc&code=def HTTP/1.1\r\n",
        "GET /callback?code=%GG HTTP/1.1\r\n",
        "GET /callback HTTP/1.1\r\nHeader: ?code=abc&state=expected\r\n",
        "GET /callback?code=abc HTTP/1.1"})
        CHECK(SpQueryParam(req, "code").empty(), "reject invalid callback: %s", req);
    CHECK(SpJson("{\"nested\":{\"access_token\":\"wrong\"},\"access_token\":\"right\"}", "access_token") == "right", "top-level token only");
    CHECK(SpJson("{\"name\":\"Beyonc\\u00e9 \\ud83c\\udfb5\"}", "name") == "Beyonc\xc3\xa9 \xf0\x9f\x8e\xb5", "JSON Unicode escapes");
    CHECK(SpJson("{broken", "access_token").empty(), "invalid JSON rejected");
    CHECK(SpJson("{\"access_token\":null}", "access_token").empty(), "null token rejected");
    CHECK(SpFirstDeviceId("{\"devices\":[{\"id\":\"one\"},{\"id\":\"two\",\"is_active\":true}]}") == "two", "prefer current device");
    CHECK(SpTrackText("{\"item\":{\"album\":{\"name\":\"Album\"},\"name\":\"Track\",\"artists\":[{\"name\":\"Artist\"}]},\"is_playing\":true}") == L"Track \x2014 Artist", "track name independent of key order");
    CHECK(SpTrackText("{\"is_playing\":true,\"item\":null}").empty(), "null playing item");
}

static SOCKET TestConnect(SOCKET listener) {
    sockaddr_in addr = {}; int size = sizeof(addr);
    if (getsockname(listener, (sockaddr*)&addr, &size)) return INVALID_SOCKET;
    SOCKET client = socket(AF_INET, SOCK_STREAM, 0);
    if (client != INVALID_SOCKET && connect(client, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(client); return INVALID_SOCKET;
    }
    return client;
}
static void TestSpotifyCallback() {
    WSADATA data;
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup");
    SOCKET listener = SpListen(0); // ephemeral port, never the user's Spotify port
    CHECK(listener != INVALID_SOCKET, "test listener");
    if (listener == INVALID_SOCKET) { WSACleanup(); return; }
    std::atomic<bool> quit{false}; std::string code; bool accepted = false;
    std::thread receiver([&] { accepted = SpWaitForCode(listener, code, "expected", quit); });
    SOCKET client = TestConnect(listener);
    CHECK(client != INVALID_SOCKET, "callback client");
    if (client != INVALID_SOCKET) {
        const char* wrong = "GET /callback?code=bad&xstate=expected HTTP/1.1\r\n\r\n";
        send(client, wrong, (int)strlen(wrong), 0); closesocket(client);
    }
    client = TestConnect(listener);
    if (client != INVALID_SOCKET) {
        const char* first = "GET /callback?code=good&sta";
        send(client, first, (int)strlen(first), 0);
        Sleep(50);
        const char* last = "te=expected HTTP/1.1\r\n\r\n";
        send(client, last, (int)strlen(last), 0);
        char response[1024]; DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        recv(client, response, sizeof(response), 0); closesocket(client);
    }
    quit = true; receiver.join();
    CHECK(accepted && code == "good", "reject stray request, accept fragmented valid request");
    closesocket(listener);
    listener = SpListen(0); quit = false; code.clear();
    if (listener != INVALID_SOCKET) {
        std::thread idle([&] { SpWaitForCode(listener, code, "expected", quit); });
        client = TestConnect(listener); Sleep(100); // peer sends nothing
        ULONGLONG start = GetTickCount64(); quit = true; idle.join();
        CHECK(GetTickCount64() - start < 1500, "idle peer cannot hold shutdown hostage");
        if (client != INVALID_SOCKET) closesocket(client);
        closesocket(listener);
    }
    WSACleanup();
}

static void TestSpotifyConfig() {
    wchar_t temp[MAX_PATH] = {}, name[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"wst", 0, name);
    DeleteFileW(name); CreateDirectoryW(name, nullptr);
    g_testDir = std::wstring(name) + L"\\";
    SpotifyClient client;
    client.LoadToken();
    CHECK(client.clientId.empty(), "no bundled client ID");
    CHECK(GetFileAttributesW(client.IniPath().c_str()) == INVALID_FILE_ATTRIBUTES, "loading never creates settings");
    CHECK(client.Configure("0123456789abcdef0123456789abcdef"), "save user client ID");
    client.accessToken = "test-access"; client.refreshToken = "test-refresh";
    client.SaveToken();
    CHECK(client.Configure("abcdef0123456789abcdef0123456789"), "change client ID");
    CHECK(client.accessToken.empty() && client.refreshToken.empty(), "changing client ID clears old login");
    wchar_t token[4096];
    GetPrivateProfileStringW(L"spotify", L"refresh", L"", token, 4096, client.IniPath().c_str());
    CHECK(!token[0], "old encrypted token removed from disk");
    CHECK(client.Configure(""), "disconnect");
    DeleteFileW(client.IniPath().c_str()); RemoveDirectoryW(name);
}
