#include "net/DnsCache.hpp"

#include <algorithm>
#include <utility>

namespace noctara::net {

DnsCache::DnsCache(std::size_t capacity)
    : m_capacity(std::max<std::size_t>(capacity, 1))
{
}

std::optional<DnsCache::Entry> DnsCache::lookup(const QString& host, Clock::time_point now)
{
    const auto it = m_entries.find(host.toLower());
    if (it == m_entries.end()) {
        return std::nullopt;
    }
    if (it->expires <= now) {
        m_entries.erase(it);
        return std::nullopt;
    }
    return *it;
}

void DnsCache::storeAddresses(const QString& host, std::vector<IpAddress> addresses, std::chrono::seconds ttl,
                              Clock::time_point now)
{
    if (addresses.empty()) {
        storeMissing(host, std::nullopt, now);
        return;
    }
    const auto lifetime = std::clamp(ttl, m_limits.minTtl, std::max(m_limits.minTtl, m_limits.maxTtl));
    if (lifetime.count() <= 0) {
        return;
    }
    insert(host, Entry{std::move(addresses), now + lifetime}, now);
}

void DnsCache::storeMissing(const QString& host, std::optional<std::chrono::seconds> serverTtl, Clock::time_point now)
{
    const auto lifetime = serverTtl ? std::min(*serverTtl, m_limits.negativeTtl) : m_limits.negativeTtl;
    if (lifetime.count() <= 0) {
        return;
    }
    insert(host, Entry{{}, now + lifetime}, now);
}

void DnsCache::insert(const QString& host, Entry entry, Clock::time_point now)
{
    if (std::cmp_greater_equal(m_entries.size(), m_capacity)) {
        // Drop expired entries first; if the cache is still full, the one that expires soonest.
        m_entries.removeIf([now](const auto& item) { return item.value().expires <= now; });
        if (std::cmp_greater_equal(m_entries.size(), m_capacity)) {
            const auto victim = std::ranges::min_element(m_entries, {}, &Entry::expires);
            m_entries.erase(victim);
        }
    }
    m_entries.insert(host.toLower(), std::move(entry));
}

}  // namespace noctara::net
