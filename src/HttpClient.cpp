#include "HttpClient.h"

#include <winhttp.h>

#include <vector>

namespace Http {

namespace {
constexpr wchar_t kUserAgent[] = L"WinDock/0.1";
}

bool SplitUrl(const std::wstring& url, bool& secure, std::wstring& host, unsigned short& port, std::wstring& path) {
    if (url.size() > 4096) return false;
    URL_COMPONENTS c{sizeof(c)};
    c.dwSchemeLength = c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = c.dwUserNameLength = 1;
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &c)) return false;
    if (c.nScheme != INTERNET_SCHEME_HTTPS && c.nScheme != INTERNET_SCHEME_HTTP) return false;
    if (c.dwUserNameLength > 0 || !c.lpszHostName || c.dwHostNameLength == 0) return false;
    secure = c.nScheme == INTERNET_SCHEME_HTTPS;
    host.assign(c.lpszHostName, c.dwHostNameLength);
    port = c.nPort;
    path.assign(c.lpszUrlPath ? c.lpszUrlPath : L"", c.dwUrlPathLength);
    if (c.lpszExtraInfo) path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    return true;
}

void Perform(const Request& rq, Result& r) {
    r = Result{};
    bool secure = true;
    std::wstring host, path;
    unsigned short port = 0;
    if (!SplitUrl(rq.url, secure, host, port, path)) {
        r.error = ERROR_INVALID_PARAMETER;
        return;
    }
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.error = GetLastError();
        return;
    }
    const int t = static_cast<int>(rq.timeoutMs);
    WinHttpSetTimeouts(session, t, t, t, t);
    HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, rq.method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    bool ok = request &&
              WinHttpSendRequest(request, rq.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : rq.headers.c_str(),
                                 rq.headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                 rq.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(rq.body.data()),
                                 static_cast<DWORD>(rq.body.size()), static_cast<DWORD>(rq.body.size()), 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        DWORD status = 0, size = sizeof(status);
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
        r.status = status;
        wchar_t type[256]{};
        DWORD typeSize = sizeof(type);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, type, &typeSize,
                                WINHTTP_NO_HEADER_INDEX)) {
            r.contentType = type;
        }
    }
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            ok = false;
            break;
        }
        if (available == 0) break;
        if (r.body.size() + available > rq.maxBody) {
            r.error = ERROR_FILE_TOO_LARGE;
            r.body.clear();
            break;
        }
        const size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, r.body.data() + at, available, &read)) {
            ok = false;
            break;
        }
        r.body.resize(at + read);
    }
    if (!ok && !r.error) r.error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    if (r.error) r.body.clear();
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
}

}  // namespace Http
