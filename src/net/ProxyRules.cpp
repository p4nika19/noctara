#include "net/ProxyRules.hpp"

#include "core/Log.hpp"

#include <QRegularExpression>

#include <array>

namespace noctara::net {

namespace {

std::uint16_t defaultPort(ProxyType type)
{
    switch (type) {
    case ProxyType::Http: return 80;
    case ProxyType::Https: return 443;
    case ProxyType::Socks4:
    case ProxyType::Socks4a:
    case ProxyType::Socks5:
    case ProxyType::Socks5h: return 1080;
    }
    return 1080;
}

QString hostWithoutBrackets(QStringView host)
{
    if (host.size() >= 2 && host.front() == u'[' && host.back() == u']') {
        return host.sliced(1, host.size() - 2).toString();
    }
    return host.toString();
}

QString variable(const QHash<QString, QString>& environment, const QString& lower, bool upperAllowed)
{
    if (const QString value = environment.value(lower).trimmed(); !value.isEmpty()) {
        return value;
    }
    return upperAllowed ? environment.value(lower.toUpper()).trimmed() : QString();
}

}  // namespace

std::optional<ProxyType> proxyTypeFromScheme(QStringView scheme)
{
    const QString lower = scheme.toString().toLower();
    if (lower == u"http") {
        return ProxyType::Http;
    }
    if (lower == u"https") {
        return ProxyType::Https;
    }
    if (lower == u"socks4") {
        return ProxyType::Socks4;
    }
    if (lower == u"socks4a") {
        return ProxyType::Socks4a;
    }
    if (lower == u"socks5" || lower == u"socks") {
        return ProxyType::Socks5;
    }
    if (lower == u"socks5h") {
        return ProxyType::Socks5h;
    }
    return std::nullopt;
}

QString proxyScheme(ProxyType type)
{
    switch (type) {
    case ProxyType::Http: return QStringLiteral("http");
    case ProxyType::Https: return QStringLiteral("https");
    case ProxyType::Socks4: return QStringLiteral("socks4");
    case ProxyType::Socks4a: return QStringLiteral("socks4a");
    case ProxyType::Socks5: return QStringLiteral("socks5");
    case ProxyType::Socks5h: return QStringLiteral("socks5h");
    }
    return QStringLiteral("http");
}

std::optional<ProxyServer> ProxyServer::fromUrl(QStringView text)
{
    QString candidate = text.trimmed().toString();
    if (candidate.isEmpty()) {
        return std::nullopt;
    }
    if (!candidate.contains(u"://")) {
        candidate.prepend(u"http://");
    }
    const QUrl url(candidate, QUrl::StrictMode);
    const auto type = proxyTypeFromScheme(url.scheme());
    if (!url.isValid() || !type || url.host().isEmpty()) {
        return std::nullopt;
    }
    ProxyServer server;
    server.type = *type;
    server.host = url.host();
    const int port = url.port(defaultPort(*type));
    if (port <= 0 || port > 65535) {
        return std::nullopt;
    }
    server.port = static_cast<std::uint16_t>(port);
    server.username = url.userName(QUrl::FullyDecoded);
    server.password = url.password(QUrl::FullyDecoded);
    return server;
}

bool ProxyServer::resolvesRemotely() const noexcept
{
    return type != ProxyType::Socks4 && type != ProxyType::Socks5;
}

QString ProxyServer::curlUrl() const
{
    const QString hostPart = host.contains(u':') ? u'[' + host + u']' : host;
    return proxyScheme(type) + u"://" + hostPart + u':' + QString::number(port);
}

QString ProxyServer::displayUrl() const
{
    const QString hostPart = host.contains(u':') ? u'[' + host + u']' : host;
    const QString auth = hasCredentials() ? log::redacted() + u'@' : QString();
    return proxyScheme(type) + u"://" + auth + hostPart + u':' + QString::number(port);
}

BypassList::BypassList(const QStringList& rules, bool bypassLocal)
    : m_local(bypassLocal)
{
    for (const QString& raw : rules) {
        const QString rule = raw.trimmed().toLower();
        if (rule.isEmpty()) {
            continue;
        }
        if (rule == u"*") {
            m_all = true;
        } else if (rule == u"<local>") {
            m_plainNames = true;
        } else if (auto cidr = Cidr::parse(rule)) {
            m_networks.push_back(*cidr);
        } else if (rule.startsWith(u"*.")) {
            m_domains.push_back({rule.sliced(2), false});
        } else {
            // A leading dot is how NO_PROXY spells "this domain and its subdomains"; same as a bare name.
            m_domains.push_back({rule.startsWith(u'.') ? rule.sliced(1) : rule, true});
        }
    }
}

BypassList BypassList::fromNoProxy(QStringView value)
{
    static const QRegularExpression separators(QStringLiteral("[,\\s]+"));
    return {value.toString().split(separators, Qt::SkipEmptyParts), false};
}

bool BypassList::matches(QStringView host) const
{
    if (m_all) {
        return true;
    }
    const QString name = hostWithoutBrackets(host).toLower();
    if (const auto address = IpAddress::parse(name)) {
        return (m_local && address->isLocal()) ||
               std::ranges::any_of(m_networks, [&](const Cidr& cidr) { return cidr.contains(*address); });
    }
    if (m_local && (name == u"localhost" || name.endsWith(u".localhost") || !name.contains(u'.'))) {
        return true;
    }
    if (m_plainNames && !name.contains(u'.')) {
        return true;
    }
    return std::ranges::any_of(m_domains, [&](const Domain& domain) {
        if (name == domain.suffix) {
            return domain.includeApex;
        }
        return name.endsWith(domain.suffix) && name.at(name.size() - domain.suffix.size() - 1) == u'.';
    });
}

EnvironmentProxy environmentProxy(const QHash<QString, QString>& environment, const QUrl& url)
{
    const bool https = url.scheme() == u"https";
    const QString specificName = https ? QStringLiteral("https_proxy") : QStringLiteral("http_proxy");
    // Upper-case HTTP_PROXY may be set by CGI-style environments from a request header (httpoxy).
    QString value = variable(environment, specificName, https);
    QString source =
        value.isEmpty() ? QString() : (environment.contains(specificName) ? specificName : specificName.toUpper());
    if (value.isEmpty()) {
        value = variable(environment, QStringLiteral("all_proxy"), true);
        source = value.isEmpty() ? QString()
                                 : (environment.contains(QStringLiteral("all_proxy")) ? QStringLiteral("all_proxy")
                                                                                      : QStringLiteral("ALL_PROXY"));
    }
    if (value.isEmpty()) {
        return {};
    }
    const BypassList noProxy = BypassList::fromNoProxy(variable(environment, QStringLiteral("no_proxy"), true));
    if (noProxy.matches(url.host())) {
        return {std::nullopt, source};
    }
    const auto server = ProxyServer::fromUrl(value);
    if (!server) {
        qCWarning(lcNet).noquote() << "ignoring unparsable proxy variable" << source;
    }
    return {server, source};
}

bool hasEnvironmentProxy(const QHash<QString, QString>& environment)
{
    constexpr std::array names{"http_proxy", "https_proxy", "all_proxy", "HTTPS_PROXY", "ALL_PROXY"};
    return std::ranges::any_of(
        names, [&](const char* name) { return !environment.value(QString::fromLatin1(name)).trimmed().isEmpty(); });
}

}  // namespace noctara::net
