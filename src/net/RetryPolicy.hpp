#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QDateTime>

#include <chrono>
#include <optional>

namespace noctara::net {

// When and how long to wait before repeating a failed request. Only idempotent requests are retried, and
// only for failures that a later attempt can plausibly fix (connection problems, 408/429/5xx gateways).
struct RetryPolicy {
    int maxRetries = 2;
    std::chrono::milliseconds baseDelay{500};
    std::chrono::milliseconds maxDelay{30'000};

    [[nodiscard]] static bool retryableError(ErrorCode code) noexcept;
    [[nodiscard]] static bool retryableStatus(int status) noexcept;

    // Exponential backoff (base · 2^retry) with ±20 % jitter (`jitter` in [0, 1)), capped; a server's
    // Retry-After wins when it is longer, up to the cap.
    [[nodiscard]] std::chrono::milliseconds delay(int retry, std::optional<std::chrono::seconds> retryAfter,
                                                  double jitter) const noexcept;
};

// Retry-After as delta-seconds or an HTTP date (RFC 9110 §10.2.3); nullopt when absent or unparsable.
[[nodiscard]] std::optional<std::chrono::seconds> parseRetryAfter(const QByteArray& value, const QDateTime& now);

}  // namespace noctara::net
