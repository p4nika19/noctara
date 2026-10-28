#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace noctara {

// Typed error codes shared by all layers. Lower layers never produce user-facing text;
// the i18n layer maps codes to translated messages (see i18n/ErrorMessages).
enum class ErrorCode : std::uint16_t {
    Unknown,
    Cancelled,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    NoSpace,
    IoError,
    Corrupt,
    ParseError,
    Unsupported,
    Timeout,

    // settings / configuration
    ConfigSyntaxError,
    ConfigTypeMismatch,
    ConfigOutOfRange,
    ConfigUnknownEnumValue,
    ConfigInvalidValue,
    ConfigUnknownKey,
    ConfigOverridden,
    ConfigNewerVersion,

    // graphics
    GraphicsInitFailed,
    GraphicsDeviceLost,

    // network
    DnsResolutionFailed,
    DnsServerUnreachable,
    DohUnreachable,
    DohInvalidResponse,
    DohInsecureUrl,
    ProxyNotConfigured,
    ProxyConnectFailed,
    ProxyAuthRequired,
    TlsHandshakeFailed,
    TlsCertificateInvalid,
    ConnectionFailed,
    ConnectionRefused,
    HttpError,
    Offline,
    MeteredDeferred,
    RateLimited,
    Unauthorized,
    Forbidden,

    // cache
    NotCacheable,    // protected (DRM) or encrypted media: never stored in any cache tier
    CacheLocked,     // another Noctara instance owns the cache directory
    ContentChanged,  // the source changed while an entry was being read; reopen it

    // SoundCloud API
    ClientIdUnavailable,  // the web client's client_id could not be found (the site changed, or it was rejected)
    InvalidResponse,      // the API answered with JSON Noctara does not understand
    SignInRequired,       // the endpoint needs an account and none is signed in

    // playback
    NotPlayable,        // SoundCloud offers no stream this account may play (blocked, removed, not streamable)
    DecodeFailed,       // the audio could not be demuxed or decoded
    AudioOutputFailed,  // no usable audio device, or the device failed
    ProtectedPlayback,  // a protected (DRM) track; it needs the protected player (DRM helper)
};

// Stable English identifier, used in logs and diagnostics.
[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string detail;  // English, for logs and diagnostics only

    [[nodiscard]] std::string describe() const;
};

template <class T> using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string detail = {})
{
    return std::unexpected(Error{code, std::move(detail)});
}

}  // namespace noctara
