#include "HttpClient.h"

// winsock2.h before winhttp.h: WINHTTP_CONNECTION_INFO (SOCKADDR_STORAGE) is declared only then
// (round 25 phase 25D; only the structures are used, no Winsock function: nothing more to link).
#include <winsock2.h>
#include <ws2ipdef.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <string_view>
#include <vector>

namespace Http {

namespace {
constexpr wchar_t kUserAgent[] = L"ViberDock/0.1";

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// "[::1]" -> "::1" (WinHttpCrackUrl may keep the brackets of an IPv6 literal).
std::wstring Bare(const std::wstring& host) {
    if (host.size() >= 2 && host.front() == L'[' && host.back() == L']') return host.substr(1, host.size() - 2);
    return host;
}

// IPv4 (network order bytes) in a local range: 0/8 (this network / unspecified), 127/8 loopback,
// 10/8, 172.16/12, 192.168/16 private, 169.254/16 link-local, 255.255.255.255 broadcast.
// 1 loopback, 2 another local range, 0 public.
int ScopeV4(const unsigned char a[4]) {
    if (a[0] == 127) return 1;
    if (a[0] == 0 || a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
        (a[0] == 169 && a[1] == 254) || (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255)) {
        return 2;
    }
    return 0;
}

// IPv6: :: unspecified, ::1 loopback, fe80::/10 link-local, fec0::/10 site-local (deprecated),
// fc00::/7 unique-local, and the IPv4-mapped (::ffff:a.b.c.d) and IPv4-compatible (::a.b.c.d)
// forms of a local IPv4 address.
int ScopeV6(const unsigned char a[16]) {
    bool zero10 = true;
    for (int i = 0; i < 10; ++i) zero10 = zero10 && a[i] == 0;
    if (zero10 && a[10] == 0 && a[11] == 0) {
        bool zero = true;
        for (int i = 12; i < 15; ++i) zero = zero && a[i] == 0;
        if (zero && a[15] == 1) return 1;  // ::1
        if (zero && a[15] == 0) return 2;  // ::
        return ScopeV4(a + 12);            // ::a.b.c.d
    }
    if (zero10 && a[10] == 0xFF && a[11] == 0xFF) return ScopeV4(a + 12);  // ::ffff:a.b.c.d
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return 2;  // fe80::/10
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) return 2;  // fec0::/10
    if ((a[0] & 0xFE) == 0xFC) return 2;                  // fc00::/7
    return 0;
}

// inet_aton's forms, which the resolver also accepts for a URL's host: 1 - 4 parts, each
// decimal, octal (leading 0) or hex (0x); the last part fills the remaining bytes.
bool ParseV4(const std::wstring& s, unsigned char out[4]) {
    unsigned long long parts[4]{};
    int n = 0;
    size_t i = 0;
    if (s.empty()) return false;
    while (i <= s.size()) {
        if (n == 4) return false;
        size_t j = i;
        int base = 10;
        if (j + 1 < s.size() && s[j] == L'0' && (s[j + 1] == L'x' || s[j + 1] == L'X')) {
            base = 16;
            j += 2;
        } else if (j + 1 < s.size() && s[j] == L'0' && s[j + 1] != L'.') {
            base = 8;
            ++j;
        }
        unsigned long long v = 0;
        size_t digits = 0;
        for (; j < s.size() && s[j] != L'.'; ++j) {
            const wchar_t c = static_cast<wchar_t>(std::towlower(s[j]));
            int d = -1;
            if (c >= L'0' && c <= L'9') d = c - L'0';
            if (c >= L'a' && c <= L'f') d = c - L'a' + 10;
            if (d < 0 || d >= base) return false;
            v = v * base + static_cast<unsigned>(d);
            if (v > 0xFFFFFFFFull) return false;
            ++digits;
        }
        if (digits == 0 && base != 8) return false;
        parts[n++] = v;
        if (j >= s.size()) break;
        i = j + 1;
    }
    unsigned long long value = 0;
    for (int k = 0; k < n - 1; ++k) {
        if (parts[k] > 255) return false;
        value = (value << 8) | parts[k];
    }
    const int restBits = 8 * (4 - (n - 1));
    if (restBits < 64 && parts[n - 1] >= (1ull << restBits)) return false;
    value = (value << restBits) | parts[n - 1];
    for (int k = 0; k < 4; ++k) out[k] = static_cast<unsigned char>(value >> (24 - 8 * k));
    return true;
}

// The scope of the address the request is connected to (0 public; 1 loopback; 2 other local),
// and its port; false when it cannot be read.
bool ConnectedScope(HINTERNET request, int& scope, unsigned short& port) {
    WINHTTP_CONNECTION_INFO info{};
    info.cbSize = sizeof(info);
    DWORD size = sizeof(info);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_CONNECTION_INFO, &info, &size)) return false;
    const SOCKADDR_STORAGE& ss = info.RemoteAddress;
    if (ss.ss_family == AF_INET) {
        const auto* sa = reinterpret_cast<const sockaddr_in*>(&ss);
        scope = ScopeV4(reinterpret_cast<const unsigned char*>(&sa->sin_addr));
        port = static_cast<unsigned short>((sa->sin_port >> 8) | ((sa->sin_port & 0xFF) << 8));  // network order
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const auto* sa = reinterpret_cast<const sockaddr_in6*>(&ss);
        scope = ScopeV6(reinterpret_cast<const unsigned char*>(&sa->sin6_addr));
        port = static_cast<unsigned short>((sa->sin6_port >> 8) | ((sa->sin6_port & 0xFF) << 8));
        return true;
    }
    return false;
}

// Location resolved against the URL it came from (absolute, scheme-relative "//host/...",
// absolute-path "/...", query-only "?..." or a relative path). "" when it cannot be.
std::wstring Resolve(const std::wstring& base, const std::wstring& location) {
    if (location.empty() || location.size() > 4096) return {};
    const std::wstring lower = Lower(location.substr(0, 8));
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0) return location;
    if (location.find(L':') != std::wstring::npos && location.find(L':') < location.find_first_of(L"/?#")) {
        return {};  // another scheme ("file:", "javascript:"): never followed
    }
    const size_t schemeEnd = base.find(L"://");
    if (schemeEnd == std::wstring::npos) return {};
    if (location.rfind(L"//", 0) == 0) return base.substr(0, schemeEnd + 1) + location;
    const size_t pathStart = base.find(L'/', schemeEnd + 3);
    const std::wstring origin = pathStart == std::wstring::npos ? base : base.substr(0, pathStart);
    if (location.front() == L'/') return origin + location;
    std::wstring path = pathStart == std::wstring::npos ? L"/" : base.substr(pathStart);
    if (const size_t q = path.find_first_of(L"?#"); q != std::wstring::npos) path.resize(q);
    if (location.front() == L'?') return origin + path + location;
    const size_t slash = path.find_last_of(L'/');
    return origin + path.substr(0, slash + 1) + location;
}

std::wstring Header(HINTERNET request, DWORD query, size_t maxChars) {
    std::vector<wchar_t> value(maxChars + 1, L'\0');
    DWORD size = static_cast<DWORD>(maxChars * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    return std::wstring(value.data(), size / sizeof(wchar_t));
}

// Drops "Authorization:" lines (a redirect to another host: as fetch() does).
std::wstring WithoutAuthorization(const std::wstring& headers) {
    std::wstring out;
    size_t at = 0;
    while (at < headers.size()) {
        size_t end = headers.find(L"\r\n", at);
        if (end == std::wstring::npos) end = headers.size();
        const std::wstring line = headers.substr(at, end - at);
        if (Lower(line.substr(0, 14)) != L"authorization:") out += line + L"\r\n";
        at = end + 2;
    }
    return out;
}

}  // namespace

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

bool IsIpLiteral(const std::wstring& host) {
    const std::wstring h = Bare(host);
    if (h.find(L':') != std::wstring::npos) return true;
    std::wstring last = h.substr(h.find_last_of(L'.') == std::wstring::npos ? 0 : h.find_last_of(L'.') + 1);
    if (last.empty()) return false;
    const std::wstring l = Lower(last);
    if (l.rfind(L"0x", 0) == 0) return std::all_of(l.begin() + 2, l.end(), [](wchar_t c) { return std::iswxdigit(c) != 0; });
    return std::all_of(l.begin(), l.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

bool HostMatches(const std::vector<std::wstring>& allowed, const std::wstring& host) {
    const std::wstring h = Lower(Bare(host));
    const bool literal = IsIpLiteral(h);
    for (const std::wstring& a : allowed) {
        if (Lower(Bare(a)) == h) return true;
        // "*.example.com": any sub-domain (not example.com itself); never an address.
        if (!literal && a.size() > 2 && a.rfind(L"*.", 0) == 0) {
            const std::wstring suffix = a.substr(1);  // ".example.com"
            if (h.size() > suffix.size() && h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
        }
    }
    return false;
}

int LocalScope(const std::wstring& host) {
    const std::wstring h = Lower(Bare(host));
    if (h == L"localhost" || (h.size() > 10 && h.compare(h.size() - 10, 10, L".localhost") == 0)) return 1;
    if (!IsIpLiteral(h)) return 0;
    unsigned char v4[4]{};
    if (h.find(L':') == std::wstring::npos) return ParseV4(h, v4) ? ScopeV4(v4) : 0;
    // IPv6 text: the forms the consent dialog needs to name (a full parser is not needed here:
    // the connection's own address is what the request is checked against).
    if (h == L"::1") return 1;
    if (h == L"::" || h.rfind(L"fe8", 0) == 0 || h.rfind(L"fe9", 0) == 0 || h.rfind(L"fea", 0) == 0 ||
        h.rfind(L"feb", 0) == 0 || h.rfind(L"fc", 0) == 0 || h.rfind(L"fd", 0) == 0) {
        return 2;
    }
    if (h.rfind(L"::ffff:", 0) == 0 && ParseV4(h.substr(7), v4)) return ScopeV4(v4);
    return 0;
}

bool LocalNetworkHost(const std::wstring& host) {
    const std::wstring h = Lower(Bare(host));
    if (h.empty()) return false;
    const int scope = LocalScope(h);
    if (scope == 1) return false;  // this PC: never
    if (scope == 2) return true;
    if (IsIpLiteral(h)) return false;  // a public address
    auto ends = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return h.size() > n && h.compare(h.size() - n, n, suffix) == 0;
    };
    return h.find(L'.') == std::wstring::npos || ends(L".local") || ends(L".lan") || ends(L".home") || ends(L".home.arpa") ||
           ends(L".internal") || ends(L".localdomain");
}

bool ValidHeaders(const std::wstring& headers, std::wstring& error) {
    if (headers.size() > 16 * 1024) {
        error = L"http: the request headers are larger than 16 KB";
        return false;
    }
    static const wchar_t* const kForbidden[] = {L"host",    L"content-length", L"transfer-encoding", L"connection",
                                                L"keep-alive", L"upgrade",     L"te",                L"trailer",
                                                L"expect"};
    size_t at = 0;
    while (at < headers.size()) {
        size_t end = headers.find(L"\r\n", at);
        if (end == std::wstring::npos) end = headers.size();
        const std::wstring line = headers.substr(at, end - at);
        at = end + 2;
        if (line.empty()) continue;
        const size_t colon = line.find(L':');
        const std::wstring name = colon == std::wstring::npos ? line : line.substr(0, colon);
        const bool token = !name.empty() && std::all_of(name.begin(), name.end(), [](wchar_t c) {
            return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
                   std::wstring_view(L"!#$%&'*+.^_`|~-").find(c) != std::wstring_view::npos;
        });
        if (colon == std::wstring::npos || !token || line.find_first_of(L"\r\n", 0) != std::wstring::npos ||
            line.find(L'\0') != std::wstring::npos) {
            error = L"http: bad header " + name.substr(0, 64);
            return false;
        }
        const std::wstring lower = Lower(name);
        if (lower.rfind(L"proxy-", 0) == 0 ||
            std::any_of(std::begin(kForbidden), std::end(kForbidden), [&](const wchar_t* f) { return lower == f; })) {
            error = L"http: the header " + name + L" is set by ViberDock, not by a widget";
            return false;
        }
    }
    return true;
}

void Perform(const Request& rq, Result& r) {
    r = Result{};
    const ULONGLONG start = GetTickCount64();
    const ULONGLONG total = rq.totalMs ? rq.totalMs : 4ull * rq.timeoutMs;
    auto remaining = [&]() -> DWORD {
        const ULONGLONG used = GetTickCount64() - start;
        return used >= total ? 0 : static_cast<DWORD>(total - used);
    };
    auto refuse = [&](const std::wstring& why) {
        r.error = ERROR_ACCESS_DENIED;
        r.problem = why;
    };
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        r.error = GetLastError();
        return;
    }
    std::wstring url = rq.url, method = rq.method, headers = rq.headers;
    std::string body = rq.body;
    bool wasSecure = true;
    for (int hop = 0;; ++hop) {
        bool secure = true;
        std::wstring host, path;
        unsigned short port = 0;
        if (!SplitUrl(url, secure, host, port, path)) {
            if (hop == 0) {
                r.error = ERROR_INVALID_PARAMETER;
            } else {
                refuse(L"http: the redirect to " + url.substr(0, 200) + L" is not an http(s) URL");
            }
            break;
        }
        // The connected address may be: 0 public only; 1 also this PC and the local network (a
        // manifest host listed as itself that names one); 2 also the local network, never this PC
        // (a host the user typed and allowed that names a device on the local network).
        int allowScope = 0;
        if (rq.strict) {
            if (hop > 0 && wasSecure && !secure) {
                refuse(L"http: a redirect from https to http was refused (" + url.substr(0, 200) + L")");
                break;
            }
            const std::wstring lower = Lower(Bare(host));
            const bool manifest = HostMatches(rq.allowedHosts, host);
            // Round 29 phase 29C. The attack this guards against: a widget steering itself to an
            // address inside the user's network (a router's admin page, a NAS, a printer, a service on
            // this PC) through a host the user allowed. It cannot get there by choosing the URL:
            // settings.set on a url setting grants nothing (only a value typed in the settings page
            // and allowed in the prompt is in userHosts, and only its exact host). It cannot get
            // there by a redirect: every hop is checked against the same lists, and an https -> http
            // hop is refused. It cannot get there by DNS: a user host that looks public must connect
            // to a public address (a name that resolves, or is rebound, to 192.168.1.1 is refused
            // after connect, before the script sees anything), and only a host whose text itself says
            // "local network" (LocalNetworkHost: a private literal, .local / .lan / .home.arpa /
            // .internal, one label), which the prompt names as such, may reach a private address;
            // even then never loopback. Plain http is allowed only for such a host. What is left: the
            // user typing an internal address on purpose, after a prompt that says it is on the local
            // network, and a request already sent to a rebound address (its side effect, not its
            // answer) - the 25D note above.
            const bool user = !manifest && std::any_of(rq.userHosts.begin(), rq.userHosts.end(),
                                                       [&](const std::wstring& u) { return Lower(Bare(u)) == lower; });
            if (!manifest && !user) {
                refuse(hop == 0 ? L"http: host \"" + host + L"\" is not in the widget's http permission"
                                : L"http: the redirect to host \"" + host + L"\" is not in the widget's http permission");
                break;
            }
            if (manifest) {
                // A local address only when the host is listed as itself and names one.
                const bool exact = std::any_of(rq.allowedHosts.begin(), rq.allowedHosts.end(),
                                               [&](const std::wstring& a) { return Lower(Bare(a)) == lower; });
                allowScope = exact && LocalScope(lower) != 0 ? 1 : 0;
                if (!allowScope && IsIpLiteral(lower) && LocalScope(lower) != 0) {
                    refuse(L"http: " + host + L" is an address on this PC or the local network");
                    break;
                }
            } else {
                const bool local = LocalNetworkHost(lower);
                if (!secure && !local) {
                    refuse(L"http: plain http is allowed only for a host on your local network (" + host + L"): use https");
                    break;
                }
                if (LocalScope(lower) == 1) {
                    refuse(L"http: " + host + L" is this PC (refused)");
                    break;
                }
                allowScope = local ? 2 : 0;
                if (!local && IsIpLiteral(lower) && LocalScope(lower) != 0) {
                    refuse(L"http: " + host + L" is an address on this PC or the local network");
                    break;
                }
            }
        }
        wasSecure = secure;
        const DWORD left = remaining();
        if (left == 0) {
            r.error = ERROR_WINHTTP_TIMEOUT;
            break;
        }
        const int t = static_cast<int>(std::min<DWORD>(rq.timeoutMs, left));
        WinHttpSetTimeouts(session, t, t, t, t);
        HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                         WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                                    : nullptr;
        if (request) {
            // Never Windows credentials, to any server or proxy (the default logs on to intranet ones).
            DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
            WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
            if (rq.strict) {
                DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
                WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
            }
        }
        bool ok = request &&
                  WinHttpSendRequest(request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                     headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                     body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                                     static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
                  WinHttpReceiveResponse(request, nullptr);
        std::wstring next;
        if (ok && rq.strict) {
            // Before anything of the response is read: where did the connection go?
            int scope = 0;
            unsigned short connectedPort = 0;
            if (!ConnectedScope(request, scope, connectedPort)) {
                refuse(L"http: the address of " + host + L" could not be checked");
                ok = false;
            } else if (connectedPort == port && ((scope == 1 && allowScope != 1) || (scope == 2 && allowScope == 0))) {
                refuse(L"http: " + host + L" resolved to an address on this PC or the local network (refused)");
                ok = false;
            }
        }
        if (ok) {
            DWORD status = 0, size = sizeof(status);
            ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE;
            r.status = status;
            if (ok && rq.strict && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
                const std::wstring location = Header(request, WINHTTP_QUERY_LOCATION, 4096);
                if (!location.empty()) {
                    if (hop >= kMaxRedirects) {
                        refuse(L"http: more than " + std::to_wstring(kMaxRedirects) + L" redirects");
                        ok = false;
                    } else {
                        next = Resolve(url, location);
                        if (next.empty()) {
                            refuse(L"http: the redirect to \"" + location.substr(0, 200) + L"\" was refused");
                            ok = false;
                        }
                    }
                }
            }
            if (ok && next.empty()) {
                r.contentType = Header(request, WINHTTP_QUERY_CONTENT_TYPE, 255);
                r.etag = Header(request, WINHTTP_QUERY_ETAG, 511);
                r.lastModified = Header(request, WINHTTP_QUERY_LAST_MODIFIED, 511);
            }
        }
        while (ok && next.empty()) {
            if (remaining() == 0) {
                r.error = ERROR_WINHTTP_TIMEOUT;  // the total deadline (a trickling server)
                break;
            }
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
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (r.error || next.empty()) break;
        // The next hop: 303 (and 301 / 302 after a POST, as browsers do) becomes a GET without a
        // body; 307 / 308 keep the method and the body. Another host does not get Authorization.
        if (r.status == 303 || ((r.status == 301 || r.status == 302) && method == L"POST")) {
            method = L"GET";
            body.clear();
        }
        bool s2 = true;
        std::wstring h2, p2;
        unsigned short port2 = 0;
        if (SplitUrl(next, s2, h2, port2, p2) && Lower(h2) != Lower(host)) headers = WithoutAuthorization(headers);
        url = std::move(next);
        r.status = 0;
    }
    if (r.error) r.body.clear();
    WinHttpCloseHandle(session);
}

}  // namespace Http
