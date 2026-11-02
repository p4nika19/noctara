#include "net/NetworkConfig.hpp"

#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <array>

namespace noctara::net {

namespace {

namespace keys = settings::keys;

constexpr std::array kCloudflareDns{"1.1.1.1", "1.0.0.1", "2606:4700:4700::1111", "2606:4700:4700::1001"};
constexpr std::array kGoogleDns{"8.8.8.8", "8.8.4.4", "2001:4860:4860::8888", "2001:4860:4860::8844"};
constexpr std::array kQuad9Dns{"9.9.9.9", "149.112.112.112", "2620:fe::fe", "2620:fe::9"};
constexpr std::array kAdGuardDns{"94.140.14.14", "94.140.15.15", "2a10:50c0::ad1:ff", "2a10:50c0::ad2:ff"};
constexpr std::array kMullvadDns{"194.242.2.2", "2a07:e340::2"};

constexpr std::array kDnsPresets{
    DnsPreset{"cloudflare", "Cloudflare", kCloudflareDns},
    DnsPreset{"google", "Google", kGoogleDns},
    DnsPreset{"quad9", "Quad9", kQuad9Dns},
    DnsPreset{"adguard", "AdGuard", kAdGuardDns},
};

// The bootstrap addresses are the providers' anycast addresses, which are covered by their certificates.
constexpr std::array kDohPresets{
    DohPreset{"cloudflare", "Cloudflare", "https://cloudflare-dns.com/dns-query", kCloudflareDns},
    DohPreset{"google", "Google", "https://dns.google/dns-query", kGoogleDns},
    DohPreset{"quad9", "Quad9", "https://dns.quad9.net/dns-query", kQuad9Dns},
    DohPreset{"adguard", "AdGuard", "https://dns.adguard-dns.com/dns-query", kAdGuardDns},
    DohPreset{"mullvad", "Mullvad", "https://dns.mullvad.net/dns-query", kMullvadDns},
};

constexpr std::array kServiceHosts{"api-v2.soundcloud.com", "soundcloud.com", "cf-media.sndcdn.com", "i1.sndcdn.com"};

template <class T> std::vector<T> parseAll(std::span<const char* const> items, auto parse)
{
    std::vector<T> out;
    for (const char* item : items) {
        if (auto value = parse(QString::fromLatin1(item))) {
            out.push_back(*value);
        }
    }
    return out;
}

std::vector<Endpoint> parseServers(const QStringList& items)
{
    std::vector<Endpoint> out;
    for (const QString& item : items) {
        if (auto endpoint = Endpoint::parse(item, 53)) {
            out.push_back(*endpoint);
        }
    }
    return out;
}

DnsConfig dnsFromSettings(const settings::SettingsStore& store)
{
    DnsConfig dns;
    const QString mode = store.stringValue(keys::DnsMode);
    dns.mode = mode == u"custom" ? DnsMode::Custom : mode == u"doh" ? DnsMode::Doh : DnsMode::System;

    const QString serverPreset = store.stringValue(keys::DnsServerPreset);
    if (dns.mode == DnsMode::Custom) {
        const auto* preset = std::ranges::find_if(
            kDnsPresets, [&](const DnsPreset& p) { return serverPreset == QLatin1StringView(p.id); });
        if (preset != kDnsPresets.end()) {
            dns.providerName = QString::fromLatin1(preset->name);
            dns.servers = parseAll<Endpoint>(preset->servers, [](const QString& s) { return Endpoint::parse(s, 53); });
        } else {
            dns.servers = parseServers(store.listValue(keys::DnsServers));
        }
    }

    const QString dohProvider = store.stringValue(keys::DohProvider);
    if (dns.mode == DnsMode::Doh) {
        const auto* preset = std::ranges::find_if(
            kDohPresets, [&](const DohPreset& p) { return dohProvider == QLatin1StringView(p.id); });
        if (preset != kDohPresets.end()) {
            dns.providerName = QString::fromLatin1(preset->name);
            dns.doh.url = QUrl(QString::fromLatin1(preset->url));
            dns.doh.bootstrap =
                parseAll<IpAddress>(preset->bootstrap, [](const QString& s) { return IpAddress::parse(s); });
        } else {
            dns.doh.url = QUrl(store.stringValue(keys::DohUrl));
            for (const QString& item : store.listValue(keys::DohBootstrap)) {
                if (auto address = IpAddress::parse(item)) {
                    dns.doh.bootstrap.push_back(*address);
                }
            }
            dns.doh.allowInsecure = store.boolValue(keys::DohAllowInsecure);
        }
    }
    dns.fallbackToSystem = store.boolValue(keys::DnsFallbackToSystem);

    const QString family = store.stringValue(keys::IpFamily);
    dns.family = family == u"prefer_ipv4"   ? IpFamilyPreference::PreferV4
                 : family == u"prefer_ipv6" ? IpFamilyPreference::PreferV6
                 : family == u"ipv4"        ? IpFamilyPreference::V4Only
                 : family == u"ipv6"        ? IpFamilyPreference::V6Only
                                            : IpFamilyPreference::Auto;
    for (const QString& item : store.listValue(keys::HostOverrides)) {
        const qsizetype eq = item.indexOf(u'=');
        if (auto address = IpAddress::parse(QStringView(item).sliced(eq + 1)); eq > 0 && address) {
            dns.overrides.emplace_back(item.first(eq).trimmed().toLower(), *address);
        }
    }
    dns.minTtl = std::chrono::seconds(store.intValue(keys::DnsMinTtl));
    dns.maxTtl = std::max(dns.minTtl, std::chrono::seconds(store.intValue(keys::DnsMaxTtl)));
    dns.negativeTtl = std::chrono::seconds(store.intValue(keys::DnsNegativeTtl));
    return dns;
}

ProxyConfig proxyFromSettings(const settings::SettingsStore& store)
{
    ProxyConfig proxy;
    const QString mode = store.stringValue(keys::ProxyMode);
    proxy.mode = mode == u"none" ? ProxyMode::None : mode == u"manual" ? ProxyMode::Manual : ProxyMode::System;
    proxy.manual.type = proxyTypeFromScheme(store.stringValue(keys::ProxyType)).value_or(ProxyType::Http);
    proxy.manual.host = store.stringValue(keys::ProxyHost).trimmed();
    if (const auto literal = IpAddress::parse(proxy.manual.host)) {
        proxy.manual.host = literal->toString();  // "[2001:db8::1]" -> bare; the URLs bracket it once
    }
    proxy.manual.port = static_cast<std::uint16_t>(store.intValue(keys::ProxyPort));
    proxy.bypass = store.listValue(keys::ProxyBypass);
    proxy.bypassLocal = store.boolValue(keys::ProxyBypassLocal);
    return proxy;
}

TransportConfig transportFromSettings(const settings::SettingsStore& store)
{
    TransportConfig transport;
    const QString version = store.stringValue(keys::HttpVersion);
    transport.httpVersion = version == u"1.1" ? HttpVersion::Http1_1
                            : version == u"2" ? HttpVersion::Http2
                            : version == u"3" ? HttpVersion::Http3
                                              : HttpVersion::Auto;
    transport.connectTimeout = std::chrono::seconds(store.intValue(keys::ConnectTimeout));
    transport.readTimeout = std::chrono::seconds(store.intValue(keys::ReadTimeout));
    transport.maxConnections = static_cast<int>(store.intValue(keys::MaxConnections));
    transport.maxConnectionsPerHost = static_cast<int>(store.intValue(keys::MaxConnectionsPerHost));
    transport.retries = static_cast<int>(store.intValue(keys::Retries));
    transport.retryBackoff = std::chrono::milliseconds(store.intValue(keys::RetryBackoff));
    transport.tls13Only = store.stringValue(keys::TlsMinVersion) == u"1.3";
    transport.caBundle = store.stringValue(keys::CaBundle);
    transport.bandwidthBytesPerSecond = store.intValue(keys::BandwidthLimit) * 1'000'000 / 8;
    transport.offline = store.boolValue(keys::OfflineMode);
    const QString metered = store.stringValue(keys::MeteredMode);
    transport.metered = metered == u"on" ? MeteredMode::On : metered == u"off" ? MeteredMode::Off : MeteredMode::Auto;
    return transport;
}

}  // namespace

NetworkConfig configFromSettings(const settings::SettingsStore& store)
{
    return {dnsFromSettings(store), proxyFromSettings(store), transportFromSettings(store)};
}

std::span<const DnsPreset> dnsPresets() noexcept
{
    return kDnsPresets;
}

std::span<const DohPreset> dohPresets() noexcept
{
    return kDohPresets;
}

std::span<const char* const> serviceHosts() noexcept
{
    return kServiceHosts;
}

}  // namespace noctara::net
