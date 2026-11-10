#include "drm/HelperNetwork.hpp"

#include "net/ProxyRules.hpp"

#include <QUrl>

#include <algorithm>

namespace noctara::drm {

namespace {

using net::ProxyType;

// Chromium's scheme for a proxy type. Its "socks5" always resolves destination names on the proxy (SOCKS5h);
// "socks4" resolves them locally.
QString chromiumScheme(ProxyType type)
{
    switch (type) {
    case ProxyType::Http: return QStringLiteral("http");
    case ProxyType::Https: return QStringLiteral("https");
    case ProxyType::Socks4:
    case ProxyType::Socks4a: return QStringLiteral("socks4");
    case ProxyType::Socks5:
    case ProxyType::Socks5h: return QStringLiteral("socks5");
    }
    return QStringLiteral("http");
}

bool isSocks(ProxyType type)
{
    return type != ProxyType::Http && type != ProxyType::Https;
}

void addLimit(HelperNetwork& plan, NetworkLimit limit)
{
    if (std::ranges::find(plan.limits, limit) == plan.limits.end()) {
        plan.limits.push_back(limit);
    }
}

}  // namespace

bool needsSocksRelay(const net::NetworkConfig& config)
{
    const net::ProxyServer& manual = config.proxy.manual;
    return config.proxy.mode == net::ProxyMode::Manual && manual.valid() && manual.hasCredentials() &&
           (manual.type == ProxyType::Socks5 || manual.type == ProxyType::Socks5h);
}

HelperNetwork planNetwork(const net::NetworkConfig& config, const LocalProxy* relay)
{
    HelperNetwork plan;
    const bool relayed = relay != nullptr && needsSocksRelay(config);
    if (config.transport.offline) {
        plan.blocked = Error{ErrorCode::Offline, "offline mode"};
    }

    // --- Proxy: a startup switch (it covers Chromium's own requests too) and the live session proxy.
    QJsonObject proxy;
    bool namesResolvedByProxy = false;
    const net::ProxyServer& manual = config.proxy.manual;
    switch (config.proxy.mode) {
    case net::ProxyMode::None:
        proxy.insert(u"mode", QStringLiteral("direct"));
        plan.switches << QStringLiteral("--no-proxy-server");
        break;
    case net::ProxyMode::System:
        // Chromium reads the same sources (environment, desktop settings) on its own.
        proxy.insert(u"mode", QStringLiteral("system"));
        break;
    case net::ProxyMode::Manual: {
        if (!manual.valid()) {
            plan.blocked = Error{ErrorCode::ProxyNotConfigured, "manual proxy without a host"};
            proxy.insert(u"mode", QStringLiteral("direct"));
            break;
        }
        const auto literal = IpAddress::parse(manual.host);  // IPv6 needs brackets
        const QString rules = relayed
                                  ? QStringLiteral("http://127.0.0.1:%1").arg(relay->port)
                                  : QStringLiteral("%1://%2:%3")
                                        .arg(chromiumScheme(manual.type), literal ? literal->toUrlHost() : manual.host)
                                        .arg(manual.port);
        QStringList bypass = config.proxy.bypass;
        if (config.proxy.bypassLocal) {
            bypass << QStringLiteral("<local>");
        }
        proxy.insert(u"mode", QStringLiteral("manual"));
        proxy.insert(u"rules", rules);
        proxy.insert(u"bypass", bypass.join(u';'));
        proxy.insert(u"display", manual.displayUrl());
        if (relayed) {
            proxy.insert(u"username", relay->username);
            proxy.insert(u"password", relay->password);
        } else if (manual.hasCredentials()) {
            if (isSocks(manual.type)) {
                addLimit(plan, NetworkLimit::SocksCredentials);
            } else {
                proxy.insert(u"username", manual.username);
                proxy.insert(u"password", manual.password);
            }
        }
        plan.switches << QStringLiteral("--proxy-server=%1").arg(rules);
        if (!bypass.isEmpty()) {
            plan.switches << QStringLiteral("--proxy-bypass-list=%1").arg(bypass.join(u';'));
        }
        if (manual.type == ProxyType::Socks4a) {
            addLimit(plan, NetworkLimit::ProxyResolvesHere);
        }
        namesResolvedByProxy = manual.type != ProxyType::Socks4 && manual.type != ProxyType::Socks4a;
        break;
    }
    }

    // --- DNS: DNS over HTTPS is applied live; host mappings (overrides, DoH bootstrap) are startup switches.
    const net::DnsConfig& dns = config.dns;
    QJsonObject dnsMessage;
    QStringList mappings;
    for (const auto& [host, address] : dns.overrides) {
        mappings << QStringLiteral("MAP %1 %2").arg(host, address.toUrlHost());
    }
    switch (dns.mode) {
    case net::DnsMode::System: dnsMessage.insert(u"mode", QStringLiteral("system")); break;
    case net::DnsMode::Doh: {
        dnsMessage.insert(u"mode", QStringLiteral("doh"));
        dnsMessage.insert(u"template", dns.doh.url.toString(QUrl::FullyEncoded));
        dnsMessage.insert(u"strict", dns.strict());
        // Also at startup: Chromium's first requests (the CDM component check) precede "configure".
        plan.switches << QStringLiteral("--noctara-doh=%1").arg(dns.doh.url.toString(QUrl::FullyEncoded));
        if (dns.strict()) {
            plan.switches << QStringLiteral("--noctara-doh-strict");
        }
        // The DoH server's own name is never looked up: it maps to its bootstrap address, as in Noctara.
        const QString dohHost = dns.doh.url.host();
        if (!dns.doh.bootstrap.empty() && !IpAddress::parse(dohHost)) {
            mappings << QStringLiteral("MAP %1 %2").arg(dohHost, dns.doh.bootstrap.front().toUrlHost());
        }
        break;
    }
    case net::DnsMode::Custom:
        dnsMessage.insert(u"mode", QStringLiteral("system"));
        if (!namesResolvedByProxy) {
            addLimit(plan, NetworkLimit::CustomDns);
        }
        // Strict: the system resolver must never see a name. Through a name-resolving proxy only the proxy's
        // own host name would need one, and only when it is not an address (the relay looks it up with Noctara's
        // own resolver).
        if (dns.strict() && !relayed && (!namesResolvedByProxy || !IpAddress::parse(manual.host))) {
            plan.blocked = Error{ErrorCode::ProtectedNetworkUnsupported, "strict custom DNS"};
        }
        break;
    }
    if (!mappings.isEmpty()) {
        plan.switches << QStringLiteral("--host-resolver-rules=%1").arg(mappings.join(u','));
    }
    if (dns.family == net::IpFamilyPreference::V4Only || dns.family == net::IpFamilyPreference::V6Only) {
        addLimit(plan, NetworkLimit::IpFamily);
    }

    // --- Transport.
    const net::TransportConfig& transport = config.transport;
    if (transport.tls13Only) {
        plan.switches << QStringLiteral("--ssl-version-min=tls1.3");
    }
    switch (transport.httpVersion) {
    case net::HttpVersion::Http1_1:
        plan.switches << QStringLiteral("--disable-http2") << QStringLiteral("--disable-quic");
        break;
    case net::HttpVersion::Http2: plan.switches << QStringLiteral("--disable-quic"); break;
    case net::HttpVersion::Http3: plan.switches << QStringLiteral("--enable-quic"); break;
    case net::HttpVersion::Auto: break;
    }
    if (!transport.caBundle.isEmpty()) {
        addLimit(plan, NetworkLimit::CaBundle);
    }
    if (transport.bandwidthBytesPerSecond > 0) {
        addLimit(plan, NetworkLimit::BandwidthLimit);
    }

    plan.configure = QJsonObject{{QStringLiteral("type"), QStringLiteral("configure")},
                                 {QStringLiteral("proxy"), proxy},
                                 {QStringLiteral("dns"), dnsMessage}};
    return plan;
}

}  // namespace noctara::drm
