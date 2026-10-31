#pragma once

#include "core/Error.hpp"
#include "net/NetworkConfig.hpp"

#include <QJsonObject>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <vector>

namespace noctara::drm {

// Network settings the helper (Chromium) cannot follow; the UI warns about each.
enum class NetworkLimit : std::uint8_t {
    CustomDns,          // plain DNS servers: Chromium has no way to use them, so the system resolver answers
    ProxyResolvesHere,  // SOCKS4a: Chromium speaks SOCKS4 and resolves destination names itself
    SocksCredentials,   // Chromium cannot authenticate to SOCKS proxies
    CaBundle,           // Chromium trusts the system certificate store, not a PEM bundle
    BandwidthLimit,     // the helper's traffic is not throttled
    IpFamily,           // IPv4-only / IPv6-only is not enforced
};

// How the helper follows Noctara's network settings: switches given when it starts (proxy, host mappings, DNS over
// HTTPS, TLS and HTTP versions; Chromium's own first requests happen before any message), the "configure" message
// (proxy credentials, and the proxy and DNS again for the player's session), what it cannot follow, and whether
// protected playback must not start at all (it would leak or fail). A change of the switches restarts the helper.
struct HelperNetwork {
    QStringList switches;
    QJsonObject configure;  // a complete "configure" message; it holds the proxy credentials: never log it
    std::vector<NetworkLimit> limits;
    std::optional<Error> blocked;
};

// Pure: `config` as NetworkService has it (manual proxy credentials filled in).
[[nodiscard]] HelperNetwork planNetwork(const net::NetworkConfig& config);

}  // namespace noctara::drm
