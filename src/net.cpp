// net.cpp - GitLab releases over WinHTTP, SHA-256 over BCrypt (net.h)
#include "net.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

namespace {

std::string LastError(const char* what) {
    DWORD e = GetLastError();
    char buf[256];
    snprintf(buf, sizeof buf, "%s (error %lu)", what, e);
    return buf;
}

struct HInternet {
    HINTERNET h = nullptr;
    HInternet(HINTERNET v = nullptr) : h(v) {}
    ~HInternet() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

// one GET; the body goes to sink (false from sink stops it). Follows GitLab's redirects to its download storage.
template <class Sink>
bool Request(const std::wstring& url, Transfer* t, Sink&& sink, std::string& error) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        error = "Bad address: " + Narrow(url);
        return false;
    }
    std::wstring object = std::wstring(path) + extra;
    HInternet session = WinHttpOpen(L"BarthesLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session.h = WinHttpOpen(L"BarthesLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = LastError("Could not start WinHTTP");
        return false;
    }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // Windows 10 before 1903: no TLS 1.3 here
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    WinHttpSetTimeouts(session, 10000, 15000, 30000, 60000);
    HInternet conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) {
        error = LastError("Could not connect");
        return false;
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HInternet req = WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        error = LastError("Could not make the request");
        return false;
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        error = LastError(("No answer from " + Narrow(host)).c_str());
        return false;
    }
    DWORD status = 0, sz = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        char buf[128];
        snprintf(buf, sizeof buf, "The server answered %lu for ", status);
        error = buf + Narrow(url);
        return false;
    }
    if (t) {
        wchar_t len[32];
        DWORD lsz = sizeof len;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &lsz,
                                WINHTTP_NO_HEADER_INDEX))
            t->total = _wtoi64(len);
    }
    std::unique_ptr<char[]> buf(new char[1 << 16]);
    for (;;) {
        if (t && t->cancel) {
            error = "Cancelled.";
            return false;
        }
        DWORD got = 0;
        if (!WinHttpReadData(req, buf.get(), 1 << 16, &got)) {
            error = LastError("The download stopped");
            return false;
        }
        if (!got) break;
        if (!sink(buf.get(), got)) {
            error = "Could not write the download to disk.";
            return false;
        }
        if (t) InterlockedAdd64(&t->done, got);
    }
    return true;
}

// ── a small JSON reader: just enough for GitLab's release objects ───────────────────────────────────────────────

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    std::string str;
    double num = 0;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    const Json& operator[](const char* k) const {
        static const Json none;
        auto it = obj.find(k);
        return it == obj.end() ? none : it->second;
    }
};

struct Parser {
    const char* p;
    const char* end;
    void Ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    bool Str(std::string& out) {
        if (p >= end || *p != '"') return false;
        for (p++; p < end && *p != '"'; p++) {
            if (*p != '\\') { out += *p; continue; }
            if (++p >= end) return false;
            switch (*p) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (end - p < 5) return false;
                unsigned cp = unsigned(strtoul(std::string(p + 1, 4).c_str(), nullptr, 16));
                p += 4;
                if (cp < 0x80) out += char(cp);
                else if (cp < 0x800) { out += char(0xC0 | cp >> 6); out += char(0x80 | (cp & 0x3F)); }
                else { out += char(0xE0 | cp >> 12); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out += *p;
            }
        }
        if (p >= end) return false;
        p++;
        return true;
    }
    bool Value(Json& v, int depth = 0) {
        if (depth > 64) return false;
        Ws();
        if (p >= end) return false;
        if (*p == '{') {
            v.kind = Json::Obj;
            p++;
            Ws();
            if (p < end && *p == '}') { p++; return true; }
            for (;;) {
                Ws();
                std::string k;
                if (!Str(k)) return false;
                Ws();
                if (p >= end || *p++ != ':') return false;
                if (!Value(v.obj[k], depth + 1)) return false;
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (*p == '[') {
            v.kind = Json::Arr;
            p++;
            Ws();
            if (p < end && *p == ']') { p++; return true; }
            for (;;) {
                v.arr.emplace_back();
                if (!Value(v.arr.back(), depth + 1)) return false;
                Ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (*p == '"') { v.kind = Json::Str; return Str(v.str); }
        if (!strncmp(p, "true", 4)) { v.kind = Json::Bool; v.num = 1; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.kind = Json::Bool; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { p += 4; return true; }
        char* e = nullptr;
        v.kind = Json::Num;
        v.num = strtod(p, &e);
        if (e == p) return false;
        p = e;
        return true;
    }
};

bool EndsWith(const std::string& s, const char* tail) {
    size_t n = strlen(tail);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, tail) == 0;
}

}  // namespace

bool net_Get(const std::wstring& url, std::string& body, std::string& error) {
    body.clear();
    return Request(url, nullptr, [&](const char* d, DWORD n) { body.append(d, n); return true; }, error);
}

bool net_Download(const std::wstring& url, const std::wstring& path, Transfer* t, std::string& error) {
    std::wstring part = path + L".part";
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        error = LastError("Could not create the download file");
        return false;
    }
    t->done = 0;
    t->total = 0;
    bool ok = Request(url, t, [&](const char* d, DWORD n) {
        DWORD put = 0;
        return WriteFile(f, d, n, &put, nullptr) && put == n;
    }, error);
    CloseHandle(f);
    if (ok && !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = LastError("Could not finish the download file");
        ok = false;
    }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

bool net_LatestRelease(const std::string& project, Release& r, std::string& error) {
    std::string enc;
    for (char c : project) enc += c == '/' ? std::string("%2F") : std::string(1, c);
    std::string body;
    error.clear();
    if (!net_Get(Widen("https://gitlab.com/api/v4/projects/" + enc + "/releases?per_page=1"), body, error)) {
        // GitLab answers 404 for a project that is not public yet: to a player that is the same as no release
        if (error.find("answered 404") != std::string::npos) error.clear();
        return false;
    }
    Json root;
    Parser ps{body.data(), body.data() + body.size()};
    if (!ps.Value(root) || root.kind != Json::Arr) {
        error = "GitLab sent something the launcher could not read.";
        return false;
    }
    if (root.arr.empty()) return false;  // no release yet: not an error
    const Json& rel = root.arr[0];
    r = Release{};
    r.tag = rel["tag_name"].str;
    r.name = rel["name"].str;
    r.version = r.tag;
    if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
    // the Windows build: a .zip with "windows" in its name if there are several, else the first .zip
    for (int pass = 0; pass < 2 && r.zip_url.empty(); pass++)
        for (const Json& l : rel["assets"]["links"].arr) {
            std::string name = l["name"].str, url = l["direct_asset_url"].str, lower = name;
            for (char& c : lower) c = char(tolower((unsigned char)c));
            if (url.empty()) url = l["url"].str;
            if (r.zip_url.empty() && EndsWith(name, ".zip") && (pass || lower.find("windows") != std::string::npos)) {
                r.zip_url = url;
                r.zip_name = name;
            }
        }
    if (!r.zip_name.empty())
        for (const Json& l : rel["assets"]["links"].arr)
            if (_stricmp(l["name"].str.c_str(), (r.zip_name + ".sha256").c_str()) == 0) {
                r.sha256_url = l["direct_asset_url"].str.empty() ? l["url"].str : l["direct_asset_url"].str;
            }
    if (r.zip_url.empty()) error = "The newest release (" + r.tag + ") has no Windows .zip attached yet.";
    return r.zip_url.size() > 0;
}

std::string net_Sha256File(const std::wstring& path, Transfer* t) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::unique_ptr<unsigned char[]> buf(new unsigned char[1 << 20]);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.get(), 1 << 20, &got, nullptr) && got) {
            ok = BCryptHashData(hash, buf.get(), got, 0) == 0;
            if (t) InterlockedAdd64(&t->done, got);
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            out = hex;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return out;
}
