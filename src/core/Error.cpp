#include "core/Error.hpp"

#include <format>

namespace noctara {

std::string_view errorCodeName(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::NoSpace: return "NoSpace";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::Corrupt: return "Corrupt";
    case ErrorCode::ParseError: return "ParseError";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::Timeout: return "Timeout";
    case ErrorCode::ConfigSyntaxError: return "ConfigSyntaxError";
    case ErrorCode::ConfigTypeMismatch: return "ConfigTypeMismatch";
    case ErrorCode::ConfigOutOfRange: return "ConfigOutOfRange";
    case ErrorCode::ConfigUnknownEnumValue: return "ConfigUnknownEnumValue";
    case ErrorCode::ConfigInvalidValue: return "ConfigInvalidValue";
    case ErrorCode::ConfigUnknownKey: return "ConfigUnknownKey";
    case ErrorCode::ConfigOverridden: return "ConfigOverridden";
    case ErrorCode::ConfigNewerVersion: return "ConfigNewerVersion";
    case ErrorCode::GraphicsInitFailed: return "GraphicsInitFailed";
    case ErrorCode::GraphicsDeviceLost: return "GraphicsDeviceLost";
    case ErrorCode::DnsResolutionFailed: return "DnsResolutionFailed";
    case ErrorCode::DnsServerUnreachable: return "DnsServerUnreachable";
    case ErrorCode::DohUnreachable: return "DohUnreachable";
    case ErrorCode::DohInvalidResponse: return "DohInvalidResponse";
    case ErrorCode::DohInsecureUrl: return "DohInsecureUrl";
    case ErrorCode::ProxyNotConfigured: return "ProxyNotConfigured";
    case ErrorCode::ProxyConnectFailed: return "ProxyConnectFailed";
    case ErrorCode::ProxyAuthRequired: return "ProxyAuthRequired";
    case ErrorCode::TlsHandshakeFailed: return "TlsHandshakeFailed";
    case ErrorCode::TlsCertificateInvalid: return "TlsCertificateInvalid";
    case ErrorCode::ConnectionFailed: return "ConnectionFailed";
    case ErrorCode::ConnectionRefused: return "ConnectionRefused";
    case ErrorCode::HttpError: return "HttpError";
    case ErrorCode::Offline: return "Offline";
    case ErrorCode::MeteredDeferred: return "MeteredDeferred";
    case ErrorCode::RateLimited: return "RateLimited";
    case ErrorCode::Unauthorized: return "Unauthorized";
    case ErrorCode::Forbidden: return "Forbidden";
    case ErrorCode::NotCacheable: return "NotCacheable";
    case ErrorCode::CacheLocked: return "CacheLocked";
    case ErrorCode::ContentChanged: return "ContentChanged";
    case ErrorCode::ClientIdUnavailable: return "ClientIdUnavailable";
    case ErrorCode::InvalidResponse: return "InvalidResponse";
    case ErrorCode::SignInRequired: return "SignInRequired";
    case ErrorCode::NotPlayable: return "NotPlayable";
    case ErrorCode::DecodeFailed: return "DecodeFailed";
    case ErrorCode::AudioOutputFailed: return "AudioOutputFailed";
    case ErrorCode::ProtectedPlayback: return "ProtectedPlayback";
    }
    return "Unknown";
}

std::string Error::describe() const
{
    if (detail.empty()) {
        return std::string(errorCodeName(code));
    }
    return std::format("{}: {}", errorCodeName(code), detail);
}

}  // namespace noctara
