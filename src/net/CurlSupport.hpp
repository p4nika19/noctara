#pragma once

#include "core/Error.hpp"

#include <curl/curl.h>
#include <memory>

namespace noctara::net {

struct CurlEasyDeleter {
    void operator()(CURL* easy) const noexcept { curl_easy_cleanup(easy); }
};
struct CurlSlistDeleter {
    void operator()(curl_slist* list) const noexcept { curl_slist_free_all(list); }
};
using CurlEasy = std::unique_ptr<CURL, CurlEasyDeleter>;
using CurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

struct CurlFeatures {
    bool http2 = false;
    bool http3 = false;
    bool httpsProxy = false;
    const char* version = "";
};

// Initialises libcurl once and checks the features the transport relies on (TLS, HTTP/2, IPv6). Failure
// means a broken installation: the app refuses to start with a clear message instead of misbehaving.
[[nodiscard]] Result<CurlFeatures> initialiseCurl();

struct CurlFailure {
    CURLcode code = CURLE_OK;
    long osErrno = 0;        // CURLINFO_OS_ERRNO
    long connectStatus = 0;  // CURLINFO_HTTP_CONNECTCODE: the proxy's answer to CONNECT
    long proxyError = 0;     // CURLINFO_PROXY_ERROR (CURLproxycode)
    bool viaProxy = false;
    bool proxyTlsPending = false;  // an HTTPS proxy accepted the connection, but its TLS handshake never finished
};

// Maps a failed transfer to a typed error. Proxy problems are reported as such, not as server problems.
[[nodiscard]] ErrorCode errorFromCurl(const CurlFailure& failure) noexcept;

}  // namespace noctara::net
