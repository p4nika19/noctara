#pragma once

#include "net/DnsCache.hpp"
#include "net/DnsUpstream.hpp"
#include "net/NetworkConfig.hpp"
#include "net/Resolver.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>

#include <memory>

namespace noctara::net {

struct ResolverStats {
    int lookups = 0;
    int cacheHits = 0;
    int systemLookups = 0;  // calls into the OS resolver, including fallbacks
    int fallbacks = 0;
    int failures = 0;
};

// The DNS stack: host overrides, then the cache, then the configured mode (system / custom servers / DoH)
// with optional fallback to the system resolver. In strict mode (custom or DoH without fallback) the system
// resolver is never called, not even for the DoH server's own name. Concurrent lookups of one name share
// one query; A and AAAA are asked in parallel and ordered by the IP family preference.
class DnsResolver : public QObject, public IDnsResolver {
    Q_OBJECT

public:
    explicit DnsResolver(ISystemResolver& system, QObject* parent = nullptr);
    ~DnsResolver() override;
    DnsResolver(const DnsResolver&) = delete;
    DnsResolver& operator=(const DnsResolver&) = delete;
    DnsResolver(DnsResolver&&) = delete;
    DnsResolver& operator=(DnsResolver&&) = delete;

    void setDohSender(DohSender sender);
    void setConfig(const DnsConfig& config);  // flushes the cache when anything DNS-related changed
    [[nodiscard]] const DnsConfig& config() const noexcept { return m_config; }

    void resolve(const QString& host, ResolveCallback callback) override;
    // Skips the cache (but stores the answer): used by "Test DNS".
    void resolveFresh(const QString& host, ResolveCallback callback);
    void flush();

    [[nodiscard]] const ResolverStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] std::size_t cacheSize() const noexcept { return m_cache.size(); }

private:
    struct Pending {
        std::vector<ResolveCallback> callbacks;
        QElapsedTimer clock;
    };

    void rebuild();
    void start(const QString& host, ResolveCallback callback, bool useCache);
    void lookupUpstream(const QString& host, const std::shared_ptr<IDnsUpstream>& upstream, ResolverSource source);
    void lookupSystem(const QString& host, ResolverSource source);
    void complete(const QString& host, Result<Resolution> result);
    void deliverLater(ResolveCallback callback, Result<Resolution> result);
    void withUpstream(const std::function<void(Result<std::shared_ptr<IDnsUpstream>>)>& next);
    [[nodiscard]] std::vector<IpAddress> ordered(std::vector<IpAddress> addresses) const;

    ISystemResolver& m_system;
    DohSender m_dohSender;
    DnsConfig m_config;
    DnsCache m_cache;
    QHash<QString, Pending> m_pending;
    std::shared_ptr<IDnsUpstream> m_upstream;  // for custom servers / DoH; rebuilt on config changes
    std::vector<IpAddress> m_dohAddresses;     // resolved DoH host when no bootstrap addresses are given
    std::uint64_t m_generation = 0;            // answers from before a config change are not cached
    ResolverStats m_stats;
};

}  // namespace noctara::net
