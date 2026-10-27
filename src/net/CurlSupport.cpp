#include "net/CurlSupport.hpp"

#include <cerrno>
#include <format>
#include <mutex>

namespace noctara::net {

Result<CurlFeatures> initialiseCurl()
{
    static std::once_flag once;
    static CURLcode initResult = CURLE_OK;
    std::call_once(once, [] { initResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initResult != CURLE_OK) {
        return fail(ErrorCode::Unsupported, std::format("curl_global_init: {}", curl_easy_strerror(initResult)));
    }
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    const auto has = [info](int feature) {
        return (static_cast<unsigned>(info->features) & static_cast<unsigned>(feature)) != 0;
    };
    constexpr unsigned kMinimumVersion = 0x075500;  // 7.85.0: CURLOPT_PROTOCOLS_STR
    if (info->version_num < kMinimumVersion) {
        return fail(ErrorCode::Unsupported, std::format("libcurl {} is older than 7.85.0", info->version));
    }
    if (!has(CURL_VERSION_SSL) || !has(CURL_VERSION_HTTP2) || !has(CURL_VERSION_IPV6)) {
        return fail(ErrorCode::Unsupported, std::format("libcurl {} lacks TLS, HTTP/2 or IPv6 support", info->version));
    }
    return CurlFeatures{.http2 = true,
                        .http3 = has(CURL_VERSION_HTTP3),
                        .httpsProxy = has(CURL_VERSION_HTTPS_PROXY),
                        .version = info->version};
}

ErrorCode errorFromCurl(const CurlFailure& failure) noexcept
{
    switch (failure.code) {
    case CURLE_OK: return ErrorCode::Unknown;
    case CURLE_ABORTED_BY_CALLBACK:
    case CURLE_WRITE_ERROR: return ErrorCode::Cancelled;
    case CURLE_COULDNT_RESOLVE_HOST: return ErrorCode::DnsResolutionFailed;
    case CURLE_COULDNT_RESOLVE_PROXY: return ErrorCode::ProxyConnectFailed;
    case CURLE_OPERATION_TIMEDOUT: return ErrorCode::Timeout;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH: return ErrorCode::TlsCertificateInvalid;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_USE_SSL_FAILED: return ErrorCode::TlsHandshakeFailed;
    case CURLE_TOO_MANY_REDIRECTS: return ErrorCode::HttpError;
    case CURLE_PROXY:
        switch (failure.proxyError) {
        case CURLPX_NO_AUTH:
        case CURLPX_USER_REJECTED: return ErrorCode::ProxyAuthRequired;
        case CURLPX_REPLY_CONNECTION_REFUSED: return ErrorCode::ConnectionRefused;
        case CURLPX_REPLY_HOST_UNREACHABLE:
        case CURLPX_REPLY_NETWORK_UNREACHABLE: return ErrorCode::ConnectionFailed;
        default: return ErrorCode::ProxyConnectFailed;
        }
    case CURLE_COULDNT_CONNECT:
        if (failure.viaProxy) {
            return ErrorCode::ProxyConnectFailed;
        }
        return failure.osErrno == ECONNREFUSED ? ErrorCode::ConnectionRefused : ErrorCode::ConnectionFailed;
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
    case CURLE_HTTP2:
    case CURLE_HTTP2_STREAM:
    case CURLE_HTTP3:
    case CURLE_QUIC_CONNECT_ERROR:
        // A refused CONNECT surfaces as a receive error with the proxy's status code.
        if (failure.connectStatus == 407) {
            return ErrorCode::ProxyAuthRequired;
        }
        if (failure.connectStatus >= 300) {
            return ErrorCode::ProxyConnectFailed;
        }
        return ErrorCode::ConnectionFailed;
    default: return ErrorCode::Unknown;
    }
}

}  // namespace noctara::net
