#include "net/DnsResolver.hpp"

#include "core/Log.hpp"

#include <QPointer>

#include <algorithm>
#include <array>

namespace noctara::net {

namespace {

constexpr std::chrono::milliseconds kServerTimeout{2000};

QString normalisedHost(const QString& host)
{
    QString name = host.trimmed().toLower();
    if (name.startsWith(u'[') && name.endsWith(u']')) {
        name = name.sliced(1, name.size() - 2);
    }
    if (name.endsWith(u'.')) {
        name.chop(1);
    }
    return name;
}

// Upstreams are QObjects whose sockets may still be signalling when the last reference goes away.
template <class T, class... Args> std::shared_ptr<IDnsUpstream> makeUpstream(Args&&... args)
{
    return std::shared_ptr<T>(new T(std::forward<Args>(args)...),  // NOLINT(cppcoreguidelines-owning-memory)
                              [](T* upstream) { upstream->deleteLater(); });
}

}  // namespace

const char* resolverSourceName(ResolverSource source) noexcept
{
    switch (source) {
    case ResolverSource::Literal: return "literal";
    case ResolverSource::Override: return "override";
    case ResolverSource::Cache: return "cache";
    case ResolverSource::System: return "system";
    case ResolverSource::SystemFallback: return "system-fallback";
    case ResolverSource::Servers: return "servers";
    case ResolverSource::Doh: return "doh";
    }
    return "unknown";
}

DnsResolver::DnsResolver(ISystemResolver& system, QObject* parent)
    : QObject(parent)
    , m_system(system)
{
}

DnsResolver::~DnsResolver() = default;

void DnsResolver::setDohSender(DohSender sender)
{
    m_dohSender = std::move(sender);
    rebuild();
}

void DnsResolver::setConfig(const DnsConfig& config)
{
    if (config == m_config) {
        return;
    }
    m_config = config;
    rebuild();
}

void DnsResolver::rebuild()
{
    const DnsConfig& config = m_config;
    ++m_generation;
    m_cache.clear();
    m_cache.setLimits({config.minTtl, config.maxTtl, config.negativeTtl});
    m_upstream.reset();
    m_dohAddresses.clear();
    if (config.mode == DnsMode::Custom) {
        m_upstream = makeUpstream<ServerUpstream>(config.servers, kServerTimeout);
    } else if (config.mode == DnsMode::Doh && m_dohSender) {
        std::vector<IpAddress> addresses = config.doh.bootstrap;
        if (const auto literal = IpAddress::parse(config.doh.url.host())) {
            addresses = {*literal};
        }
        if (!addresses.empty()) {
            m_upstream = makeUpstream<DohUpstream>(config.doh.url, addresses, m_dohSender);
        }
    }
    // Lookups in flight belong to the old settings, and the old upstream drops its queries without answering:
    // ask again under the new settings, or their callers (and every later caller of the host) would wait forever.
    QHash<QString, Pending> pending = std::exchange(m_pending, {});
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        for (ResolveCallback& callback : it->callbacks) {
            start(it.key(), std::move(callback), true);
        }
    }
}

void DnsResolver::flush()
{
    m_cache.clear();
    ++m_generation;
}

void DnsResolver::resolve(const QString& host, ResolveCallback callback)
{
    start(host, std::move(callback), true);
}

void DnsResolver::resolveFresh(const QString& host, ResolveCallback callback)
{
    start(host, std::move(callback), false);
}

void DnsResolver::deliverLater(ResolveCallback callback, Result<Resolution> result)
{
    if (!result) {
        ++m_stats.failures;
    }
    QMetaObject::invokeMethod(
        this, [callback = std::move(callback), result = std::move(result)] { callback(result); }, Qt::QueuedConnection);
}

std::vector<IpAddress> DnsResolver::ordered(std::vector<IpAddress> addresses) const
{
    const auto isV4 = [](const IpAddress& a) {
        return a.family() == AddressFamily::V4;
    };
    switch (m_config.family) {
    case IpFamilyPreference::V4Only: std::erase_if(addresses, [&](const IpAddress& a) { return !isV4(a); }); break;
    case IpFamilyPreference::V6Only: std::erase_if(addresses, isV4); break;
    case IpFamilyPreference::PreferV4: std::ranges::stable_partition(addresses, isV4); break;
    case IpFamilyPreference::Auto:
    case IpFamilyPreference::PreferV6:
        // IPv6 first, as getaddrinfo does; the transport races both families (happy eyeballs).
        std::ranges::stable_partition(addresses, [&](const IpAddress& a) { return !isV4(a); });
        break;
    }
    return addresses;
}

void DnsResolver::start(const QString& rawHost, ResolveCallback callback, bool useCache)
{
    ++m_stats.lookups;
    const QString host = normalisedHost(rawHost);
    if (const auto literal = IpAddress::parse(host)) {
        deliverLater(std::move(callback), Resolution{{*literal}, ResolverSource::Literal, {}, {}});
        return;
    }
    if (!isValidHostname(host)) {
        deliverLater(std::move(callback), fail(ErrorCode::InvalidArgument, "invalid host name"));
        return;
    }
    // RFC 6761: localhost names never leave the machine.
    if (host == u"localhost" || host.endsWith(u".localhost")) {
        constexpr std::array<std::uint8_t, 4> v4{127, 0, 0, 1};
        constexpr std::array<std::uint8_t, 16> v6{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        auto loopback = ordered({IpAddress::fromV6(v6), IpAddress::fromV4(v4)});
        deliverLater(std::move(callback), Resolution{std::move(loopback), ResolverSource::Literal, {}, {}});
        return;
    }
    std::vector<IpAddress> overrides;
    for (const auto& [name, address] : m_config.overrides) {
        if (normalisedHost(name) == host) {  // an override may be written "example.com."
            overrides.push_back(address);
        }
    }
    if (!overrides.empty()) {
        auto addresses = ordered(std::move(overrides));
        if (addresses.empty()) {
            deliverLater(std::move(callback),
                         fail(ErrorCode::DnsResolutionFailed, "no address of the allowed IP version"));
        } else {
            deliverLater(std::move(callback), Resolution{std::move(addresses), ResolverSource::Override, {}, {}});
        }
        return;
    }
    if (useCache) {
        if (const auto cached = m_cache.lookup(host, DnsCache::Clock::now())) {
            ++m_stats.cacheHits;
            auto addresses = ordered(cached->addresses);
            if (addresses.empty()) {
                deliverLater(std::move(callback), fail(ErrorCode::DnsResolutionFailed, "no such host (cached)"));
            } else {
                deliverLater(std::move(callback), Resolution{std::move(addresses), ResolverSource::Cache, {}, {}});
            }
            return;
        }
    }

    if (const auto it = m_pending.find(host); it != m_pending.end()) {
        it->callbacks.push_back(std::move(callback));
        return;
    }
    Pending& pending = m_pending[host];
    pending.callbacks.push_back(std::move(callback));
    pending.clock.start();
    pending.lookup = ++m_lastLookup;
    const std::uint64_t lookup = pending.lookup;

    if (m_config.mode == DnsMode::System) {
        lookupSystem(host, ResolverSource::System, lookup);
        return;
    }
    const ResolverSource source = m_config.mode == DnsMode::Doh ? ResolverSource::Doh : ResolverSource::Servers;
    withUpstream([this, host, source, lookup](Result<std::shared_ptr<IDnsUpstream>> upstream) {
        if (!isCurrent(host, lookup)) {
            return;  // the settings changed in between and the host is being looked up again
        }
        if (upstream) {
            lookupUpstream(host, *upstream, source, lookup);
        } else if (m_config.fallbackToSystem) {
            ++m_stats.fallbacks;
            qCInfo(lcNet).noquote() << "DNS falls back to the system resolver:"
                                    << QString::fromStdString(upstream.error().describe());
            lookupSystem(host, ResolverSource::SystemFallback, lookup);
        } else {
            complete(host, lookup, std::unexpected(upstream.error()));
        }
    });
}

// Provides the upstream for custom servers or DoH. A DoH server given by name without bootstrap addresses is
// resolved once through the system resolver, which strict mode forbids.
void DnsResolver::withUpstream(const std::function<void(Result<std::shared_ptr<IDnsUpstream>>)>& next)
{
    const auto later = [this](const auto& function) {
        QMetaObject::invokeMethod(this, function, Qt::QueuedConnection);
    };
    if (m_config.mode == DnsMode::Doh) {
        const DohServer& doh = m_config.doh;
        if (doh.url.isEmpty() || !doh.url.isValid()) {
            later([next] { next(fail(ErrorCode::DohUnreachable, "no DoH address configured")); });
            return;
        }
        if (!dohUrlAllowed(doh.url, doh.bootstrap, doh.allowInsecure)) {
            later([next] { next(fail(ErrorCode::DohInsecureUrl, "plain http:// DoH address not allowed")); });
            return;
        }
    }
    if (m_upstream) {
        later([next, upstream = m_upstream] { next(upstream); });
        return;
    }
    if (m_config.mode != DnsMode::Doh || !m_dohSender) {
        later([next] { next(fail(ErrorCode::DohUnreachable, "DoH is not available")); });
        return;
    }
    if (m_config.strict()) {
        later([next] {
            next(fail(ErrorCode::DohUnreachable, "strict mode needs bootstrap addresses for the DoH server"));
        });
        return;
    }
    ++m_stats.systemLookups;
    const std::uint64_t generation = m_generation;
    m_system.lookup(m_config.doh.url.host(), [self = QPointer(this), generation, next](auto addresses) {
        if (self == nullptr) {
            return;
        }
        if (!addresses) {
            next(fail(ErrorCode::DohUnreachable, "DoH server name: " + addresses.error().describe()));
            return;
        }
        if (generation == self->m_generation && !self->m_upstream) {
            self->m_upstream = makeUpstream<DohUpstream>(self->m_config.doh.url, *addresses, self->m_dohSender);
        }
        next(self->m_upstream ? Result<std::shared_ptr<IDnsUpstream>>(self->m_upstream)
                              : fail(ErrorCode::DohUnreachable, "configuration changed"));
    });
}

void DnsResolver::lookupUpstream(const QString& host, const std::shared_ptr<IDnsUpstream>& upstream,
                                 ResolverSource source, std::uint64_t lookup)
{
    struct Gathered {
        int remaining = 0;
        std::vector<Result<UpstreamAnswer>> answers;
    };
    const auto gathered = std::make_shared<Gathered>();
    std::vector<dns::RecordType> types;
    if (m_config.family != IpFamilyPreference::V6Only) {
        types.push_back(dns::RecordType::A);
    }
    if (m_config.family != IpFamilyPreference::V4Only) {
        types.push_back(dns::RecordType::Aaaa);
    }
    gathered->remaining = static_cast<int>(types.size());
    const std::uint64_t generation = m_generation;
    const QByteArray name = host.toLatin1();
    for (const dns::RecordType type : types) {
        upstream->query(name, type, [self = QPointer(this), gathered, host, source, generation, lookup](auto answer) {
            gathered->answers.push_back(std::move(answer));
            if (--gathered->remaining > 0 || self == nullptr || !self->isCurrent(host, lookup)) {
                return;
            }
            std::vector<IpAddress> addresses;
            std::uint32_t ttl = UINT32_MAX;
            std::optional<std::uint32_t> negativeTtl;
            const Error* firstError = nullptr;
            QString server;
            for (const auto& result : gathered->answers) {
                if (!result) {
                    firstError = firstError != nullptr ? firstError : &result.error();
                    continue;
                }
                server = result->server;
                for (const dns::AddressRecord& record : result->response.addresses) {
                    addresses.push_back(record.address);
                    ttl = std::min(ttl, record.ttl);
                }
                if (result->response.negativeTtl) {
                    negativeTtl = std::min(negativeTtl.value_or(UINT32_MAX), *result->response.negativeTtl);
                }
            }
            const bool cacheable = generation == self->m_generation;
            const auto now = DnsCache::Clock::now();
            if (!addresses.empty()) {
                if (cacheable) {
                    self->m_cache.storeAddresses(host, addresses, std::chrono::seconds(ttl), now);
                }
                self->complete(host, lookup, Resolution{self->ordered(std::move(addresses)), source, server, {}});
                return;
            }
            if (firstError == nullptr) {
                // Every server answered authoritatively: the name has no addresses. Never a reason to fall back.
                if (cacheable) {
                    self->m_cache.storeMissing(
                        host, negativeTtl.transform([](std::uint32_t s) { return std::chrono::seconds(s); }), now);
                }
                self->complete(host, lookup, fail(ErrorCode::DnsResolutionFailed, "no such host"));
                return;
            }
            if (self->m_config.fallbackToSystem && generation == self->m_generation) {
                ++self->m_stats.fallbacks;
                qCInfo(lcNet).noquote() << "DNS falls back to the system resolver:"
                                        << QString::fromStdString(firstError->describe());
                self->lookupSystem(host, ResolverSource::SystemFallback, lookup);
                return;
            }
            self->complete(host, lookup, std::unexpected(*firstError));
        });
    }
}

void DnsResolver::lookupSystem(const QString& host, ResolverSource source, std::uint64_t lookup)
{
    ++m_stats.systemLookups;
    const std::uint64_t generation = m_generation;
    m_system.lookup(host, [self = QPointer(this), host, source, generation, lookup](auto addresses) {
        if (self == nullptr || !self->isCurrent(host, lookup)) {
            return;
        }
        const auto now = DnsCache::Clock::now();
        if (!addresses) {
            if (addresses.error().code == ErrorCode::NotFound) {
                if (generation == self->m_generation) {
                    self->m_cache.storeMissing(host, std::nullopt, now);
                }
                self->complete(host, lookup, fail(ErrorCode::DnsResolutionFailed, "no such host"));
            } else {
                self->complete(host, lookup, std::unexpected(addresses.error()));
            }
            return;
        }
        if (generation == self->m_generation) {
            // No TTL from the OS: the cache keeps it for its minimum lifetime.
            self->m_cache.storeAddresses(host, *addresses, std::chrono::seconds(0), now);
        }
        auto ordered = self->ordered(std::move(*addresses));
        if (ordered.empty()) {
            self->complete(host, lookup, fail(ErrorCode::DnsResolutionFailed, "no address of the allowed IP version"));
            return;
        }
        self->complete(host, lookup, Resolution{std::move(ordered), source, {}, {}});
    });
}

bool DnsResolver::isCurrent(const QString& host, std::uint64_t lookup) const
{
    const auto it = m_pending.constFind(host);
    return it != m_pending.cend() && it->lookup == lookup;
}

void DnsResolver::complete(const QString& host, std::uint64_t lookup, Result<Resolution> result)
{
    const auto it = m_pending.find(host);
    if (it == m_pending.end() || it->lookup != lookup) {
        return;
    }
    const Pending pending = std::move(*it);
    m_pending.erase(it);
    if (result) {
        result->elapsed = std::chrono::milliseconds(pending.clock.elapsed());
        if (result->addresses.empty()) {
            result = fail(ErrorCode::DnsResolutionFailed, "no address of the allowed IP version");
        }
    }
    if (!result) {
        ++m_stats.failures;
    }
    for (const ResolveCallback& callback : pending.callbacks) {
        callback(result);
    }
}

}  // namespace noctara::net
