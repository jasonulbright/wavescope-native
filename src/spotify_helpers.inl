// Small protocol helpers shared by the client and adversarial unit tests.
#include <string>
#include <cstdint>

static int SpHexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool SpValidClientId(const std::string& id) {
    if (id.size() != 32) return false;
    for (char c : id) if (SpHexDigit(c) < 0) return false;
    return true;
}
static std::string SpUrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) return {};
            int a = SpHexDigit(s[i + 1]), b = SpHexDigit(s[i + 2]);
            if (a < 0 || b < 0 || (a == 0 && b == 0)) return {};
            out += char((a << 4) | b); i += 2;
        } else out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}
static std::string SpUnHex(const std::string& s) {
    if (s.size() % 2) return {};
    std::string out;
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = SpHexDigit(s[i]), b = SpHexDigit(s[i + 1]);
        if (a < 0 || b < 0) return {};
        out += char((a << 4) | b);
    }
    return out;
}
static std::string SpBase64Url(const BYTE* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out; uint32_t val = 0; int bits = -6;
    for (size_t i = 0; i < n; ++i) {
        val = (val << 8) | p[i]; bits += 8;
        while (bits >= 0) { out += t[(val >> bits) & 63]; bits -= 6; }
    }
    if (bits > -6) out += t[((val << 8) >> (bits + 8)) & 63];
    return out;
}
// Parse only GET /callback with exact, unique parameter names. Reject an
// incomplete request line, malformed encoding and duplicate parameters.
static std::string SpQueryParam(const std::string& req, const char* key) {
    size_t end = req.find("\r\n");
    if (end == std::string::npos || req.compare(0, 14, "GET /callback?") != 0) return {};
    size_t space = req.find(' ', 14);
    if (space == std::string::npos || space > end ||
        (req.substr(space, end - space) != " HTTP/1.1" && req.substr(space, end - space) != " HTTP/1.0")) return {};
    std::string result; bool found = false;
    for (size_t pos = 14; pos < space;) {
        size_t next = req.find('&', pos);
        if (next == std::string::npos || next > space) next = space;
        size_t eq = req.find('=', pos);
        if (eq == std::string::npos || eq >= next) return {};
        std::string name = SpUrlDecode(req.substr(pos, eq - pos));
        std::string value = SpUrlDecode(req.substr(eq + 1, next - eq - 1));
        if (name.empty() || value.empty()) return {};
        if (name == key) {
            if (found) return {};
            found = true; result = value;
        }
        pos = next + 1;
    }
    return result;
}
