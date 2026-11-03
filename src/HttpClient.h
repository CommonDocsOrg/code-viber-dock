#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

// One synchronous WinHTTP request (the system's automatic proxy settings), for worker threads:
// the weather service and the widgets' `http` API share it. TLS for https URLs; no
// WINHTTP_OPTION_DECOMPRESSION (on Windows 11 build 26300 it makes WinHttpQueryDataAvailable fail
// with E_ABORT in synchronous mode).
namespace Http {

struct Request {
    std::wstring method = L"GET";
    std::wstring url;      // http:// or https://
    std::wstring headers;  // extra request headers, "Name: value\r\n" each
    std::string body;      // sent as is (POST / PUT)
    DWORD timeoutMs = 10000;           // resolve, connect, send and receive, each
    size_t maxBody = 1u << 20;         // a longer response fails with ERROR_FILE_TOO_LARGE
};

struct Result {
    DWORD error = 0;           // Win32 / WinHTTP error (0: a response arrived)
    DWORD status = 0;          // HTTP status
    std::string body;          // the response body (empty on error)
    std::wstring contentType;  // the Content-Type header ("" if none)
    std::wstring etag;         // the ETag header ("" if none; the calendar feeds revalidate with it)
    std::wstring lastModified; // the Last-Modified header ("" if none)
};

void Perform(const Request& request, Result& result);

// Splits an http(s) URL. False if it is not one (or has user info).
bool SplitUrl(const std::wstring& url, bool& secure, std::wstring& host, unsigned short& port, std::wstring& path);

}  // namespace Http
