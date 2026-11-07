#include "net/RetryPolicy.hpp"

#include <QLocale>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace noctara::net {

bool RetryPolicy::retryableError(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::ConnectionFailed:
    case ErrorCode::ConnectionRefused:
    case ErrorCode::Timeout:
    case ErrorCode::DnsServerUnreachable:
    case ErrorCode::DohUnreachable:
    case ErrorCode::ProxyConnectFailed: return true;
    default: return false;
    }
}

bool RetryPolicy::retryableStatus(int status) noexcept
{
    return status == 408 || status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

std::chrono::milliseconds RetryPolicy::delay(int retry, std::optional<std::chrono::seconds> retryAfter,
                                             double jitter) const noexcept
{
    const double exponent = std::ldexp(1.0, std::clamp(retry, 0, 20));
    const double factor = 0.8 + 0.4 * std::clamp(jitter, 0.0, 1.0);
    const auto backoff = std::chrono::milliseconds(static_cast<std::int64_t>(
        std::min(static_cast<double>(baseDelay.count()) * exponent * factor, static_cast<double>(maxDelay.count()))));
    if (retryAfter) {
        if (*retryAfter >= std::chrono::ceil<std::chrono::seconds>(maxDelay)) {
            return maxDelay;  // before the conversion: a huge server value would overflow milliseconds
        }
        return std::clamp(std::chrono::duration_cast<std::chrono::milliseconds>(*retryAfter), backoff, maxDelay);
    }
    return backoff;
}

std::optional<std::chrono::seconds> parseRetryAfter(const QByteArray& value, const QDateTime& now)
{
    const QByteArray trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return std::nullopt;
    }
    bool ok = false;
    const qint64 seconds = trimmed.toLongLong(&ok);
    if (ok) {
        return seconds >= 0 ? std::optional(std::chrono::seconds(seconds)) : std::nullopt;
    }
    // IMF-fixdate, e.g. "Wed, 21 Oct 2015 07:28:00 GMT".
    QDateTime date =
        QLocale::c().toDateTime(QString::fromLatin1(trimmed), QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'"));
    if (!date.isValid()) {
        return std::nullopt;
    }
    date.setTimeZone(QTimeZone::UTC);
    return std::chrono::seconds(std::max<qint64>(0, now.secsTo(date)));
}

}  // namespace noctara::net
