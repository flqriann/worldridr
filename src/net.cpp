#include "net.h"
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>

static HINTERNET session() {
    static HINTERNET s = [] {
        HINTERNET h = WinHttpOpen(L"worldridr/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (h) {
            WinHttpSetTimeouts(h, 8000, 8000, 20000, 60000);
            DWORD d = WINHTTP_DECOMPRESSION_FLAG_ALL;
            WinHttpSetOption(h, WINHTTP_OPTION_DECOMPRESSION, &d, sizeof d);
        }
        return h;
    }();
    return s;
}

bool httpRequest(const std::string& url, std::vector<uint8_t>& out, int& status, const std::string& body) {
    out.clear();
    status = 0;
    if (!session()) return false;
    std::wstring wurl(url.begin(), url.end());
    std::vector<wchar_t> host(512), path(16384), extra(16384);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host.data(); uc.dwHostNameLength = (DWORD)host.size();
    uc.lpszUrlPath = path.data(); uc.dwUrlPathLength = (DWORD)path.size();
    uc.lpszExtraInfo = extra.data(); uc.dwExtraInfoLength = (DWORD)extra.size();
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    HINTERNET c = WinHttpConnect(session(), host.data(), uc.nPort, 0);
    if (!c) return false;
    std::wstring full = std::wstring(path.data()) + extra.data();
    HINTERNET r = WinHttpOpenRequest(c, body.empty() ? L"GET" : L"POST", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    bool ok = false;
    if (r) {
        const wchar_t* hdr = body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(r, hdr, body.empty() ? 0 : (DWORD)-1, body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                               (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD st = 0, sz = sizeof st;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
            status = (int)st;
            ok = true;
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(r, &avail)) { ok = false; break; }
                if (!avail) break;
                size_t old = out.size();
                out.resize(old + avail);
                DWORD got = 0;
                if (!WinHttpReadData(r, out.data() + old, avail, &got)) { ok = false; break; }
                out.resize(old + got);
            }
        }
        WinHttpCloseHandle(r);
    }
    WinHttpCloseHandle(c);
    return ok;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

void writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::create_directories(p.parent_path(), ec);
    std::string tmp = path + ".tmp" + std::to_string(GetCurrentThreadId());
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return;
        f.write((const char*)data.data(), (std::streamsize)data.size());
    }
    std::filesystem::rename(tmp, p, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

bool cachedGet(const std::string& url, const std::string& cachePath, std::vector<uint8_t>& out) {
    if (!cachePath.empty() && readFile(cachePath, out)) return true;
    int status = 0;
    if (!httpRequest(url, out, status) || status != 200 || out.empty()) return false;
    if (!cachePath.empty()) writeFile(cachePath, out);
    return true;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
