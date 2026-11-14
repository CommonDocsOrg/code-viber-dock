#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

// One synchronous WinHTTP request (the system's automatic proxy settings), for worker threads:
// the weather service, the calendar feeds and the widgets' `http` API share it. TLS for https URLs;
// no WINHTTP_OPTION_DECOMPRESSION (on Windows 11 build 26300 it makes WinHttpQueryDataAvailable
// fail with E_ABORT in synchronous mode).
//
// Round 25 phase 25D, for every caller: Windows credentials are never sent
// (WINHTTP_OPTION_AUTOLOGON_POLICY = HIGH: the default, MEDIUM, logs on automatically to intranet
// and proxy servers, which a widget could aim at), and the whole request has a deadline (totalMs)
// on top of WinHTTP's per-operation timeouts, so a server trickling a byte at a time cannot hold a
// worker thread for ever. `strict` (the widgets' requests only; the weather and calendar callers
// keep WinHTTP's own redirect handling, which follows any host except https -> http):
//  - automatic redirects off; up to kMaxRedirects are followed here, each Location (resolved
//    against the URL it came from) checked with the same host allowlist (`allowedHosts`, HostMatches)
//    and scheme rules as the first URL, and never from https to http;
//  - an IP address as the host must be listed as itself (a wildcard never covers one);
//  - the address actually connected to (WINHTTP_OPTION_CONNECTION_INFO, read after the response
//    headers arrive and before anything of the response is read) must not be loopback, private,
//    link-local, unique-local or unspecified (IsLocalAddress), unless the URL's host is listed as
//    itself and is that literal address or `localhost`. Behind a proxy the address connected to is
//    the proxy's: that is detected by its port differing from the URL's (WinHTTP connects to the
//    URL's port otherwise) and then the check cannot apply (the proxy resolves the name).
//  A refusal fails the request with ERROR_ACCESS_DENIED and a message in Result::problem.
//  What this does not prevent: a request already sent to a name that resolved to a local address
//  (DNS rebinding) has reached that address (its side effect happened); only the response is
//  withheld from the script.
namespace Http {

constexpr int kMaxRedirects = 5;

struct Request {
    std::wstring method = L"GET";
    std::wstring url;      // http:// or https://
    std::wstring headers;  // extra request headers, "Name: value\r\n" each
    std::string body;      // sent as is (POST / PUT)
    DWORD timeoutMs = 10000;           // resolve, connect, send and receive, each
    // Round 25 phase 25D: the whole request (every hop, reading the body) at most this long; 0:
    // four times timeoutMs. Past it the request fails with ERROR_WINHTTP_TIMEOUT.
    DWORD totalMs = 0;
    size_t maxBody = 1u << 20;         // a longer response fails with ERROR_FILE_TOO_LARGE
    // Round 25 phase 25D: the widget rules above (redirects by hand, local addresses refused).
    bool strict = false;
    std::vector<std::wstring> allowedHosts;  // strict: lowercase, "*.example.com" any sub-domain
    // Round 29 phase 29C (strict): hosts the user typed into a widget's `url` setting and allowed in
    // the prompt (exact names, lowercase), reachable besides allowedHosts on every hop. A hop whose
    // host is allowed only through this list: https only, unless LocalNetworkHost(host); its
    // connection may then reach a private / link-local / unique-local address, never loopback; a
    // public-looking name must connect to a public address (see the attack note in HttpClient.cpp).
    std::vector<std::wstring> userHosts;
};

struct Result {
    DWORD error = 0;           // Win32 / WinHTTP error (0: a response arrived)
    DWORD status = 0;          // HTTP status
    std::string body;          // the response body (empty on error)
    std::wstring contentType;  // the Content-Type header ("" if none)
    std::wstring etag;         // the ETag header ("" if none; the calendar feeds revalidate with it)
    std::wstring lastModified; // the Last-Modified header ("" if none)
    std::wstring problem;      // round 25 phase 25D: why a strict request was refused ("" otherwise)
};

void Perform(const Request& request, Result& result);

// Splits an http(s) URL. False if it is not one (or has user info).
bool SplitUrl(const std::wstring& url, bool& secure, std::wstring& host, unsigned short& port, std::wstring& path);

// --- round 25 phase 25D: the widgets' host rules (also the consent dialog's wording) ---
// `host` (any case) is allowed by `allowed` (lowercase entries; "*.example.com": any sub-domain,
// not example.com itself; an IP address is only matched by an entry that is that address).
bool HostMatches(const std::vector<std::wstring>& allowed, const std::wstring& host);
// The host is an IP address literal (IPv6, or a last label that is a number: "10.0.0.1",
// "2130706433", "0x7f.1"; a DNS top-level label is never numeric).
bool IsIpLiteral(const std::wstring& host);
// What a listed host names, for the consent dialog: 0 a name or a public address, 1 this PC
// (localhost, loopback), 2 the local network (a private, link-local or unique-local address).
int LocalScope(const std::wstring& host);
// Round 29 phase 29C: a host the user may mean as "a device on my network" by its text alone: a
// private / link-local / unique-local address literal (LocalScope 2), a name under .local, .lan,
// .home, .home.arpa, .internal or .localdomain, or a single-label name ("homeassistant"). Never
// localhost or loopback (LocalScope 1).
bool LocalNetworkHost(const std::wstring& host);
// Validates request headers ("Name: value\r\n" lines): a token name, no CR / LF / NUL inside a
// line, not one of the headers WinHTTP or the host own (Host, Content-Length, Transfer-Encoding,
// Connection, Keep-Alive, Upgrade, TE, Trailer, Expect, Proxy-*), at most 16 KB. False with
// `error` naming the header.
bool ValidHeaders(const std::wstring& headers, std::wstring& error);

}  // namespace Http
