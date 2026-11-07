#pragma once

#include "core/NetAddress.hpp"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <cstdint>
#include <optional>
#include <vector>

namespace noctara::net {

enum class ProxyType : std::uint8_t { Http, Https, Socks4, Socks4a, Socks5, Socks5h };

[[nodiscard]] std::optional<ProxyType> proxyTypeFromScheme(QStringView scheme);
[[nodiscard]] QString proxyScheme(ProxyType type);

struct ProxyServer {
    ProxyType type = ProxyType::Http;
    QString host;
    std::uint16_t port = 0;
    QString username;  // secret: never logged, never written to settings
    QString password;  // secret

    // "socks5h://user:pass@host:1080", "http://proxy:3128", "proxy:3128" (HTTP). Ports default per scheme.
    [[nodiscard]] static std::optional<ProxyServer> fromUrl(QStringView text);

    [[nodiscard]] bool valid() const noexcept { return !host.isEmpty() && port != 0; }
    // True when the proxy, not Noctara, turns destination host names into addresses (HTTP CONNECT and
    // absolute-URI requests, SOCKS4a, SOCKS5h). Plain SOCKS4/SOCKS5 need locally resolved addresses.
    [[nodiscard]] bool resolvesRemotely() const noexcept;
    [[nodiscard]] bool hasCredentials() const noexcept { return !username.isEmpty() || !password.isEmpty(); }
    // For CURLOPT_PROXY: scheme, host and port only (credentials go through separate options).
    [[nodiscard]] QString curlUrl() const;
    // For the UI, diagnostics and logs: credentials replaced by <redacted>.
    [[nodiscard]] QString displayUrl() const;

    friend bool operator==(const ProxyServer&, const ProxyServer&) = default;
};

// Hosts that bypass the proxy. Rules: "example.com" and ".example.com" match the domain and its subdomains,
// "*.example.com" only subdomains, IP addresses and CIDR ranges match literal addresses, "<local>" matches
// names without a dot, "*" matches everything. `bypassLocal` adds localhost, loopback, private and
// link-local addresses.
class BypassList {
public:
    BypassList() = default;
    BypassList(const QStringList& rules, bool bypassLocal);

    // Comma- or space-separated NO_PROXY syntax.
    [[nodiscard]] static BypassList fromNoProxy(QStringView value);

    [[nodiscard]] bool matches(QStringView host) const;

private:
    struct Domain {
        QString suffix;  // lower case, no leading dot
        bool includeApex = true;
    };
    std::vector<Domain> m_domains;
    std::vector<Cidr> m_networks;
    bool m_all = false;
    bool m_plainNames = false;
    bool m_local = false;
};

enum class ProxySource : std::uint8_t { None, Manual, Environment, Desktop };

struct EnvironmentProxy {
    std::optional<ProxyServer> server;  // nullopt: direct
    QString variable;                   // e.g. "https_proxy", for the settings status line
};

// The proxy the conventional environment variables select for `url` (lower-case names win over upper case;
// HTTP_PROXY in upper case is ignored as in curl, see httpoxy). Returns an empty variable when none is set.
[[nodiscard]] EnvironmentProxy environmentProxy(const QHash<QString, QString>& environment, const QUrl& url);
[[nodiscard]] bool hasEnvironmentProxy(const QHash<QString, QString>& environment);

}  // namespace noctara::net
