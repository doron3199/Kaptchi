// ============================================================================
// http_client.cpp -- WinHTTP POST helper, isolated from OpenCV headers
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
#include <string>
#include <vector>

static void HttpLog(const char* msg) {
    OutputDebugStringA("[HTTP] ");
    OutputDebugStringA(msg);
    OutputDebugStringA("\n");
    fprintf(stderr, "[HTTP] %s\n", msg);
    fflush(stderr);
}

std::string HttpPostLocal(int port, const std::string& path,
                          const std::string& body, int timeout_ms) {
    HINTERNET hSess = WinHttpOpen(L"KaptchiAI/1.0",
                                   WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) { HttpLog("WinHttpOpen FAILED"); return {}; }

    HINTERNET hConn = WinHttpConnect(hSess, L"localhost",
                                      static_cast<INTERNET_PORT>(port), 0);
    if (!hConn) { HttpLog("WinHttpConnect FAILED"); WinHttpCloseHandle(hSess); return {}; }

    std::wstring wpath(path.begin(), path.end());
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) {
        HttpLog("WinHttpOpenRequest FAILED");
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return {};
    }
    WinHttpSetTimeouts(hReq, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    BOOL ok = WinHttpSendRequest(
        hReq,
        L"Content-Type: application/json\r\n",
        static_cast<DWORD>(-1L),
        const_cast<void*>(static_cast<const void*>(body.c_str())),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    if (!ok) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "WinHttpSendRequest FAILED err=%lu", GetLastError());
        HttpLog(errbuf);
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return {};
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "WinHttpReceiveResponse FAILED err=%lu", GetLastError());
        HttpLog(errbuf);
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return {};
    }

    std::string response;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        std::vector<char> chunk(static_cast<size_t>(avail) + 1, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hReq, chunk.data(), avail, &read)) break;
        response.append(chunk.data(), read);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return response;
}

std::string HttpGetLocal(int port, const std::string& path, int timeout_ms) {
    HINTERNET hSess = WinHttpOpen(L"KaptchiAI/1.0",
                                   WINHTTP_ACCESS_TYPE_NO_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess) return {};

    HINTERNET hConn = WinHttpConnect(hSess, L"localhost",
                                      static_cast<INTERNET_PORT>(port), 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return {}; }

    std::wstring wpath(path.begin(), path.end());
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", wpath.c_str(),
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return {}; }
    WinHttpSetTimeouts(hReq, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return {};
    }

    std::string response;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        std::vector<char> chunk(static_cast<size_t>(avail) + 1, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(hReq, chunk.data(), avail, &read)) break;
        response.append(chunk.data(), read);
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return response;
}
