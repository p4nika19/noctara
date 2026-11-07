#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace noctara::net {

// Global download rate limit. A transfer may take a chunk while the bucket is not empty (the balance may go
// negative, so chunks larger than the burst still pass); otherwise it pauses until the bucket refills.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    void setRate(std::int64_t bytesPerSecond, Clock::time_point now) noexcept
    {
        m_rate = std::max<std::int64_t>(bytesPerSecond, 0);
        m_tokens = burst();
        m_last = now;
    }
    [[nodiscard]] bool limited() const noexcept { return m_rate > 0; }

    [[nodiscard]] bool tryConsume(std::int64_t bytes, Clock::time_point now) noexcept
    {
        if (!limited()) {
            return true;
        }
        refill(now);
        if (m_tokens <= 0) {
            return false;
        }
        m_tokens -= bytes;
        return true;
    }

    // How long until a paused transfer may try again.
    [[nodiscard]] std::chrono::milliseconds waitTime(Clock::time_point now) noexcept
    {
        refill(now);
        if (!limited() || m_tokens > 0) {
            return std::chrono::milliseconds(0);
        }
        return std::chrono::milliseconds(1 + (-m_tokens + 1) * 1000 / m_rate);
    }

private:
    // A quarter of a second of traffic: smooth enough for audio, small enough to keep the limit honest.
    [[nodiscard]] std::int64_t burst() const noexcept { return std::max<std::int64_t>(m_rate / 4, 16 * 1024); }

    void refill(Clock::time_point now) noexcept
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - m_last).count();
        if (elapsed <= 0) {
            return;
        }
        // Saturate first: after a long idle period m_rate * elapsed would overflow.
        const std::int64_t untilFull = m_rate > 0 ? (burst() - m_tokens) * 1'000'000 / m_rate + 1 : 0;
        if (m_rate > 0 && elapsed >= untilFull) {
            m_tokens = burst();
        } else {
            m_tokens = std::min(burst(), m_tokens + m_rate * elapsed / 1'000'000);
        }
        m_last = now;
    }

    std::int64_t m_rate = 0;
    std::int64_t m_tokens = 0;
    Clock::time_point m_last{};
};

}  // namespace noctara::net
