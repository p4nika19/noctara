#pragma once

#include "core/NetAddress.hpp"
#include "net/ProxyRules.hpp"

#include <QString>
#include <QStringList>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::net {

enum class DnsMode : std::uint8_t { System, Custom, Doh };
enum class IpFamilyPreference : std::uint8_t { Auto, PreferV4, PreferV6, V4Only, V6Only };
enum class ProxyMode : std::uint8_t { None, System, Manual };
enum class HttpVersion : std::uint8_t { Auto, Http1_1, Http2, Http3 };
enum class MeteredMode : std::uint8_t { Auto, On, Off };

struct DohServer {
    QUrl url;
    std::vector<IpAddress> bootstrap;
    bool allowInsecure = false;

    friend bool operator==(const DohServer&, const DohServer&) = default;
};

struct DnsConfig {
    DnsMode mode = DnsMode::System;
    QString providerName;  // "Cloudflare", ... or empty for custom entries
    std::vector<Endpoint> servers;
    DohServer doh;
    bool fallbackToSystem = true;
    IpFamilyPreference family = IpFamilyPreference::Auto;
    std::vector<std::pair<QString, IpAddress>> overrides;  // lower-case host -> address
    std::chrono::seconds minTtl{60};
    std::chrono::seconds maxTtl{3600};
    std::chrono::seconds negativeTtl{30};

    [[nodiscard]] bool strict() const noexcept { return mode != DnsMode::System && !fallbackToSystem; }
    friend bool operator==(const DnsConfig&, const DnsConfig&) = default;
};

struct ProxyConfig {
    ProxyMode mode = ProxyMode::System;
    ProxyServer manual;  // credentials are filled in from the secret store, never from settings
    QStringList bypass;
    bool bypassLocal = true;

    friend bool operator==(const ProxyConfig&, const ProxyConfig&) = default;
};

struct TransportConfig {
    HttpVersion httpVersion = HttpVersion::Auto;
    std::chrono::seconds connectTimeout{15};
    std::chrono::seconds readTimeout{30};
    int maxConnections = 16;
    int maxConnectionsPerHost = 6;
    int retries = 2;
    std::chrono::milliseconds retryBackoff{500};
    bool tls13Only = false;
    QString caBundle;
    std::int64_t bandwidthBytesPerSecond = 0;  // 0 = unlimited
    bool offline = false;
    MeteredMode metered = MeteredMode::Auto;

    friend bool operator==(const TransportConfig&, const TransportConfig&) = default;
};

struct NetworkConfig {
    DnsConfig dns;
    ProxyConfig proxy;
    TransportConfig transport;

    friend bool operator==(const NetworkConfig&, const NetworkConfig&) = default;
};

// Snapshot of the network.* settings (proxy credentials are not settings and stay empty).
[[nodiscard]] NetworkConfig configFromSettings(const settings::SettingsStore& store);

struct DnsPreset {
    const char* id;
    const char* name;
    std::span<const char* const> servers;
};
struct DohPreset {
    const char* id;
    const char* name;
    const char* url;
    std::span<const char* const> bootstrap;
};

[[nodiscard]] std::span<const DnsPreset> dnsPresets() noexcept;
[[nodiscard]] std::span<const DohPreset> dohPresets() noexcept;

// The hosts Noctara talks to; used by "Test DNS" and "Test connection".
[[nodiscard]] std::span<const char* const> serviceHosts() noexcept;

}  // namespace noctara::net
