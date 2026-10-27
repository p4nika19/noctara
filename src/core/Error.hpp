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
    DohUnreachable,
    DohInvalidResponse,
    ProxyConnectFailed,
    ProxyAuthRequired,
    TlsHandshakeFailed,
    ConnectionFailed,
    ConnectionRefused,
    HttpError,
    Offline,
    RateLimited,
    Unauthorized,
    Forbidden,
};

// Stable English identifier, used in logs and diagnostics.
[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string detail;  // English, for logs and diagnostics only

    [[nodiscard]] std::string describe() const;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string detail = {})
{
    return std::unexpected(Error{code, std::move(detail)});
}

}  // namespace noctara
