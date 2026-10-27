#pragma once

#include "core/NetAddress.hpp"

#include <QHash>
#include <QString>

#include <chrono>
#include <optional>
#include <vector>

namespace noctara::net {

// In-process DNS cache keyed by lower-case host name. Positive answers live for their TTL clamped to
// [minTtl, maxTtl]; "no such name" answers for negativeTtl (or the server's SOA value, if shorter).
// Time is passed in, so expiry is testable without sleeping. Not thread-safe: owned by the resolver.
class DnsCache {
public:
    using Clock = std::chrono::steady_clock;

    struct Limits {
        std::chrono::seconds minTtl{60};
        std::chrono::seconds maxTtl{3600};
        std::chrono::seconds negativeTtl{30};
    };

    struct Entry {
        std::vector<IpAddress> addresses;  // empty: the name does not exist (negative entry)
        Clock::time_point expires;
    };

    explicit DnsCache(std::size_t capacity = 1024);

    void setLimits(const Limits& limits) noexcept { m_limits = limits; }
    [[nodiscard]] const Limits& limits() const noexcept { return m_limits; }

    [[nodiscard]] std::optional<Entry> lookup(const QString& host, Clock::time_point now);
    void storeAddresses(const QString& host, std::vector<IpAddress> addresses, std::chrono::seconds ttl,
                        Clock::time_point now);
    void storeMissing(const QString& host, std::optional<std::chrono::seconds> serverTtl, Clock::time_point now);
    void clear() noexcept { m_entries.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return static_cast<std::size_t>(m_entries.size()); }

private:
    void insert(const QString& host, Entry entry, Clock::time_point now);

    std::size_t m_capacity;
    Limits m_limits;
    QHash<QString, Entry> m_entries;
};

}  // namespace noctara::net
