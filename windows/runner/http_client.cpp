// ============================================================================
// http_client.cpp -- WinHTTP POST/GET helpers, isolated from OpenCV headers
// ============================================================================

// Isolated from OpenCV to avoid the Windows / OpenCV header ordering conflict.
// Only include Windows SDK headers here.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma comment(lib, "winhttp.lib")
#include <winhttp.h>

#include "http_client.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static void HttpLog(const char* msg) {
    OutputDebugStringA("[HTTP] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    fprintf(stderr, "[HTTP] %s\n", msg);
    fflush(stderr);
}

namespace {

struct HInternetDeleter {
    void operator()(HINTERNET h) const { if (h) WinHttpCloseHandle(h); }
};
using UniqueHInternet = std::unique_ptr<void, HInternetDeleter>;

static std::string HttpSendLocal(int port, const wchar_t* verb,
                                 const std::string& path,
                                 const std::string* body,
                                 int timeout_ms,
                                 bool log_errors = true) {
    UniqueHInternet sess{WinHttpOpen(L"KaptchiAI/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!sess) {
        if (log_errors) HttpLog("WinHttpOpen FAILED");
        return {};
    }

    UniqueHInternet conn{WinHttpConnect(sess.get(), L"localhost",
        static_cast<INTERNET_PORT>(port), 0)};
    if (!conn) {
        if (log_errors) HttpLog("WinHttpConnect FAILED");
        return {};
    }

    std::wstring wpath(path.begin(), path.end());
    UniqueHInternet req{WinHttpOpenRequest(conn.get(), verb, wpath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
    if (!req) {
        if (log_errors) HttpLog("WinHttpOpenRequest FAILED");
        return {};
    }

    WinHttpSetTimeouts(req.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    LPCWSTR headers = body ? L"Content-Type: application/json\r\n"
                           : WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD hlen = body ? static_cast<DWORD>(-1L) : 0;
    void* data = body ? const_cast<char*>(body->data()) : WINHTTP_NO_REQUEST_DATA;
    DWORD dlen = body ? static_cast<DWORD>(body->size()) : 0;

    if (!WinHttpSendRequest(req.get(), headers, hlen, data, dlen, dlen, 0)) {
        if (log_errors) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "WinHttpSendRequest FAILED err=%lu", GetLastError());
            HttpLog(errbuf);
        }
        return {};
    }
    if (!WinHttpReceiveResponse(req.get(), nullptr)) {
        if (log_errors) {
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "WinHttpReceiveResponse FAILED err=%lu", GetLastError());
            HttpLog(errbuf);
        }
        return {};
    }

    std::string response;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req.get(), &avail) && avail > 0) {
        std::vector<char> chunk(static_cast<size_t>(avail));
        DWORD read = 0;
        if (!WinHttpReadData(req.get(), chunk.data(), avail, &read)) break;
        response.append(chunk.data(), read);
    }
    return response;
}

}  // namespace

std::string HttpPostLocal(int port, const std::string& path,
                          const std::string& body, int timeout_ms) {
    return HttpSendLocal(port, L"POST", path, &body, timeout_ms, true);
}

std::string HttpGetLocal(int port, const std::string& path, int timeout_ms) {
    return HttpSendLocal(port, L"GET", path, nullptr, timeout_ms, false);
}
