#pragma once
// Minimal HTTP POST helper that is intentionally kept in its own .cpp so that
// WinHTTP headers never mix with OpenCV headers (they conflict badly on MSVC).
#include <string>

// POST body to http://localhost:<port><path>.
// Returns the raw response body on success, or an empty string on failure.
std::string HttpPostLocal(int port, const std::string& path,
                          const std::string& body, int timeout_ms = 25000);

// GET http://localhost:<port><path>.
std::string HttpGetLocal(int port, const std::string& path, int timeout_ms = 5000);
