#include "ui/NetworkController.hpp"

#include "auth/SecretStore.hpp"
#include "core/Log.hpp"
#include "core/Strings.hpp"
#include "i18n/ErrorMessages.hpp"
#include "i18n/LanguageManager.hpp"
#include "net/DnsUpstream.hpp"
#include "net/NetworkMonitor.hpp"
#include "settings/Keys.hpp"
#include "ui/SettingsController.hpp"

#include <QClipboard>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>

namespace noctara::ui {

namespace {

namespace keys = settings::keys;

QString milliseconds(double value)
{
    //: Duration in milliseconds, e.g. "42 ms"
    return NetworkController::tr("%1 ms").arg(QLocale().toString(value, 'f', 0));
}

QString addressesText(const std::vector<IpAddress>& addresses)
{
    QStringList parts;
    for (const IpAddress& address : addresses) {
        parts.append(address.toString());
    }
    return parts.join(QStringLiteral(", "));
}

}  // namespace

NetworkController::NetworkController(net::NetworkService& service, net::NetworkMonitor* monitor,
                                     auth::ISecretStore& secrets, i18n::LanguageManager& languages,
                                     SettingsController& settingsUi, QObject* parent)
    : QObject(parent)
    , m_service(service)
    , m_monitor(monitor)
    , m_secrets(secrets)
    , m_settingsUi(settingsUi)
{
    connect(&m_service, &net::NetworkService::configChanged, this, &NetworkController::refresh);
    if (m_monitor != nullptr) {
        connect(m_monitor, &net::NetworkMonitor::changed, this, [this] {
            m_service.setSystemMetered(m_monitor->metered());
            refresh();
        });
        m_service.setSystemMetered(m_monitor->metered());
    }
    connect(&languages, &i18n::LanguageManager::languageChanged, this, [this] {
        refresh();
        Q_EMIT dnsTestChanged();
        Q_EMIT connectionTestChanged();
        Q_EMIT recentRequestsChanged();
    });
    m_recentThrottle.setSingleShot(true);
    m_recentThrottle.setInterval(500);
    connect(&m_recentThrottle, &QTimer::timeout, this, &NetworkController::recentRequestsChanged);
    connect(&m_service, &net::NetworkService::requestRecorded, this, [this] {
        if (!m_recentThrottle.isActive()) {
            m_recentThrottle.start();
        }
    });
    loadCredentials();
    refresh();
}

QString NetworkController::connectivity() const
{
    if (m_monitor == nullptr) {
        return QStringLiteral("unknown");
    }
    switch (m_monitor->connectivity()) {
    case net::Connectivity::None: return QStringLiteral("none");
    case net::Connectivity::Portal: return QStringLiteral("portal");
    case net::Connectivity::Limited: return QStringLiteral("limited");
    case net::Connectivity::Full: return QStringLiteral("full");
    case net::Connectivity::Unknown: break;
    }
    return QStringLiteral("unknown");
}

bool NetworkController::metered() const
{
    return m_service.meteredActive();
}

bool NetworkController::offline() const
{
    return m_service.config().transport.offline;
}

QString NetworkController::dnsSummary() const
{
    const net::DnsConfig& dns = m_service.config().dns;
    QString text;
    switch (dns.mode) {
    case net::DnsMode::System: return tr("System resolver");
    case net::DnsMode::Custom: {
        QStringList servers;
        for (const Endpoint& server : dns.servers) {
            servers.append(server.toString(53));
        }
        //: %1 = provider name or list of server addresses
        text = tr("Custom DNS servers: %1")
                   .arg(dns.providerName.isEmpty() ? servers.join(QStringLiteral(", ")) : dns.providerName);
        break;
    }
    case net::DnsMode::Doh:
        //: %1 = provider name or server host name
        text = tr("DNS over HTTPS: %1").arg(dns.providerName.isEmpty() ? dns.doh.url.host() : dns.providerName);
        break;
    }
    //: Appended to the DNS mode when the system resolver is never used
    return dns.strict() ? tr("%1 · strict").arg(text) : text;
}

QString NetworkController::proxySummary() const
{
    const net::ProxyConfig& proxy = m_service.config().proxy;
    switch (proxy.mode) {
    case net::ProxyMode::None: return tr("No proxy");
    case net::ProxyMode::Manual:
        return proxy.manual.valid() ? proxy.manual.displayUrl() : tr("Manual proxy without a host");
    case net::ProxyMode::System: break;
    }
    //: %1 = proxy address, or "direct"
    return tr("System: %1").arg(m_systemProxy ? m_systemProxy->displayUrl() : tr("direct"));
}

QString NetworkController::dnsProxyHint() const
{
    const net::ProxyConfig& proxy = m_service.config().proxy;
    std::optional<net::ProxyServer> active;
    if (proxy.mode == net::ProxyMode::Manual && proxy.manual.valid()) {
        active = proxy.manual;
    } else if (proxy.mode == net::ProxyMode::System) {
        active = m_systemProxy;
    }
    if (!active) {
        return {};
    }
    if (!active->resolvesRemotely()) {
        return tr("With SOCKS4 and SOCKS5 Noctara looks up server names itself, using the DNS settings above, and "
                  "sends only addresses to the proxy. Choose SOCKS5 with remote DNS to let the proxy look them up.");
    }
    return tr("The proxy looks up server names itself, so the DNS settings above apply only to direct "
              "connections and to finding the proxy server.");
}

bool NetworkController::keyringAvailable() const
{
    return m_secrets.keyringBacked();
}

bool NetworkController::secretsPersistent() const
{
    return m_secrets.persistent();
}

void NetworkController::refresh()
{
    updateRowStatuses();
    Q_EMIT statusChanged();
    if (m_service.config().proxy.mode == net::ProxyMode::System) {
        m_service.route(QUrl(QStringLiteral("https://api-v2.soundcloud.com/")), this,
                        [this](const Result<net::ProxyRoute>& route) {
                            m_systemProxy = route ? route->proxy : std::nullopt;
                            updateRowStatuses();
                            Q_EMIT statusChanged();
                        });
    }
}

void NetworkController::updateRowStatuses()
{
    const net::NetworkConfig& config = m_service.config();
    QString proxyStatus;
    if (config.proxy.mode == net::ProxyMode::System) {
        switch (m_service.systemProxySource()) {
        case net::ProxySource::Environment:
            proxyStatus = tr("Taken from the proxy environment variables (http_proxy, https_proxy, all_proxy).");
            break;
        case net::ProxySource::Desktop:
            proxyStatus = m_systemProxy ? tr("Taken from your desktop's proxy settings.")
                                        : tr("Your desktop has no proxy set, so Noctara connects directly.");
            break;
        default: proxyStatus = tr("No system proxy is configured; connecting directly."); break;
        }
    }
    m_settingsUi.setRowStatus(latin(keys::ProxyMode), proxyStatus);

    QString meteredStatus;
    if (m_monitor != nullptr && config.transport.metered == net::MeteredMode::Auto) {
        meteredStatus = m_monitor->metered() ? tr("The current connection is metered.")
                                             : tr("The current connection is not metered.");
    }
    m_settingsUi.setRowStatus(latin(keys::MeteredMode), meteredStatus);

    m_settingsUi.setRowStatus(latin(keys::DnsMode), config.dns.strict()
                                                        ? tr("Strict mode: when these servers fail, lookups fail. The "
                                                             "system resolver is never asked.")
                                                        : QString());

    const bool dohRejected =
        config.dns.mode == net::DnsMode::Doh && config.dns.providerName.isEmpty() && !config.dns.doh.url.isEmpty() &&
        !net::dohUrlAllowed(config.dns.doh.url, config.dns.doh.bootstrap, config.dns.doh.allowInsecure);
    m_settingsUi.setRowStatus(latin(keys::DohUrl),
                              dohRejected ? i18n::errorMessage(ErrorCode::DohInsecureUrl) : QString());

    const bool bundleMissing = !config.transport.caBundle.isEmpty() && !QFileInfo::exists(config.transport.caBundle);
    m_settingsUi.setRowStatus(latin(keys::CaBundle),
                              bundleMissing ? tr("This file does not exist, so secure connections will fail.")
                                            : QString());
}

void NetworkController::loadCredentials()
{
    m_secrets.read(QString::fromLatin1(kCredentialsKey), this,
                   [this, generation = m_credentialsGeneration](Result<std::optional<QByteArray>> secret) {
                       if (generation != m_credentialsGeneration || !secret || !*secret) {
                           Q_EMIT credentialsChanged();
                           return;
                       }
                       const QJsonObject object = QJsonDocument::fromJson(**secret).object();
                       m_username = object.value(QStringLiteral("username")).toString();
                       m_password = object.value(QStringLiteral("password")).toString();
                       m_service.setProxyCredentials(m_username, m_password);
                       Q_EMIT credentialsChanged();
                   });
}

void NetworkController::saveProxyCredentials(const QString& username, const QString& password)
{
    if (username.isEmpty() && password.isEmpty()) {
        forgetProxyCredentials();
        return;
    }
    ++m_credentialsGeneration;
    m_username = username;
    m_password = password;
    m_service.setProxyCredentials(username, password);
    const QByteArray secret =
        QJsonDocument(QJsonObject{{QStringLiteral("username"), username}, {QStringLiteral("password"), password}})
            .toJson(QJsonDocument::Compact);
    m_secrets.write(QString::fromLatin1(kCredentialsKey), tr("Noctara proxy credentials"), secret, this,
                    [this](const Result<void>& result) {
                        Q_EMIT credentialsChanged();
                        if (!result || !m_secrets.persistent()) {
                            Q_EMIT notice(QStringLiteral("warning"),
                                          tr("No keyring is available, so the proxy credentials are kept only until "
                                             "Noctara quits."));
                        } else if (m_secrets.keyringBacked()) {
                            Q_EMIT notice(QStringLiteral("success"), tr("Proxy credentials saved to the keyring."));
                        } else {
                            Q_EMIT notice(QStringLiteral("warning"),
                                          tr("Proxy credentials saved in Noctara's encrypted local store because no "
                                             "desktop keyring is available."));
                        }
                    });
}

void NetworkController::forgetProxyCredentials()
{
    ++m_credentialsGeneration;
    m_username.clear();
    m_password.clear();
    m_service.setProxyCredentials({}, {});
    m_secrets.remove(QString::fromLatin1(kCredentialsKey), this,
                     [this](const Result<void>&) { Q_EMIT credentialsChanged(); });
}

void NetworkController::testDns()
{
    if (m_dnsTesting) {
        return;
    }
    m_dnsTesting = true;
    Q_EMIT dnsTestChanged();
    m_service.testDns(this, [this](std::vector<net::DnsTestResult> results) {
        m_dnsResults = std::move(results);
        m_dnsTesting = false;
        Q_EMIT dnsTestChanged();
    });
}

void NetworkController::testConnection()
{
    if (m_connectionTesting) {
        return;
    }
    m_connectionTesting = true;
    Q_EMIT connectionTestChanged();
    m_service.testConnection(this, [this](std::vector<net::ConnectionTestResult> results) {
        m_connectionResults = std::move(results);
        m_connectionTesting = false;
        Q_EMIT connectionTestChanged();
    });
}

void NetworkController::flushDnsCache()
{
    m_service.flushDnsCache();
    Q_EMIT notice(QStringLiteral("success"), tr("DNS cache cleared."));
}

QVariantList NetworkController::dnsResults() const
{
    QVariantList list;
    for (const net::DnsTestResult& entry : m_dnsResults) {
        QVariantMap item{{QStringLiteral("host"), entry.host}, {QStringLiteral("ok"), entry.result.has_value()}};
        if (entry.result) {
            QString via;
            switch (entry.result->source) {
            case net::ResolverSource::Literal: via = tr("IP address"); break;
            case net::ResolverSource::Override: via = tr("host override"); break;
            case net::ResolverSource::Cache: via = tr("cache"); break;
            case net::ResolverSource::System: via = tr("system resolver"); break;
            case net::ResolverSource::SystemFallback: via = tr("system resolver (fallback)"); break;
            case net::ResolverSource::Servers: via = entry.result->server; break;
            case net::ResolverSource::Doh:
                //: %1 = DoH server host name
                via = tr("DNS over HTTPS (%1)").arg(entry.result->server);
                break;
            }
            item.insert(QStringLiteral("detail"), addressesText(entry.result->addresses));
            item.insert(QStringLiteral("via"), via);
            item.insert(QStringLiteral("latency"), milliseconds(static_cast<double>(entry.result->elapsed.count())));
        } else {
            item.insert(QStringLiteral("detail"), i18n::errorMessage(entry.result.error().code));
        }
        list.append(item);
    }
    return list;
}

QVariantList NetworkController::connectionResults() const
{
    QVariantList list;
    for (const net::ConnectionTestResult& entry : m_connectionResults) {
        QVariantMap item{{QStringLiteral("host"), entry.url.host()}, {QStringLiteral("ok"), entry.result.has_value()}};
        if (entry.result) {
            //: %1 = HTTP status code, %2 = protocol such as HTTP/2
            item.insert(QStringLiteral("detail"), tr("Reachable (HTTP %1, %2)")
                                                      .arg(entry.result->status)
                                                      .arg(QString::fromLatin1(entry.result->httpVersion)));
            item.insert(QStringLiteral("via"), entry.result->route == u"direct" ? tr("direct") : entry.result->route);
            item.insert(QStringLiteral("latency"), milliseconds(entry.result->timings.totalMs));
        } else {
            item.insert(QStringLiteral("detail"), i18n::errorMessage(entry.result.error().code));
        }
        list.append(item);
    }
    return list;
}

QVariantList NetworkController::recentRequests() const
{
    QVariantList list;
    constexpr qsizetype kShown = 20;
    for (const net::RequestRecord& record : m_service.recentRequests()) {
        if (list.size() == kShown) {
            break;
        }
        list.append(QVariantMap{
            {QStringLiteral("time"), QLocale().toString(record.finishedAt.time(), QStringLiteral("HH:mm:ss"))},
            {QStringLiteral("method"), QString::fromLatin1(record.method)},
            {QStringLiteral("url"), record.url},
            {QStringLiteral("ok"), !record.error && record.status < 400},
            {QStringLiteral("result"),
             record.error ? i18n::errorMessage(*record.error)
                          : QStringLiteral("%1 %2").arg(record.status).arg(QString::fromLatin1(record.httpVersion))},
            {QStringLiteral("latency"), milliseconds(record.timings.totalMs)},
            {QStringLiteral("via"), record.route == u"direct" ? tr("direct") : record.route},
        });
    }
    return list;
}

QString NetworkController::report() const
{
    const net::NetworkConfig& config = m_service.config();
    QStringList lines;
    //: Title of the copied network report; %1 = app version, %2 = date and time
    lines << tr("Noctara network report (%1, %2)")
                 .arg(QCoreApplication::applicationVersion(), QDateTime::currentDateTime().toString(Qt::ISODate));
    lines << tr("Connectivity: %1").arg(connectivity());
    lines << tr("Offline mode: %1").arg(offline() ? tr("on") : tr("off"));
    lines << tr("Metered: %1").arg(metered() ? tr("yes") : tr("no"));
    lines << tr("DNS: %1").arg(dnsSummary());
    lines << tr("Proxy: %1").arg(proxySummary());
    lines << tr("Minimum TLS version: %1")
                 .arg(config.transport.tls13Only ? QStringLiteral("1.3") : QStringLiteral("1.2"));
    lines << tr("Custom CA bundle: %1").arg(config.transport.caBundle.isEmpty() ? tr("no") : tr("yes"));
    const auto appendResults = [&lines](const QString& title, const QVariantList& results) {
        lines << QString() << title;
        for (const QVariant& item : results) {
            const QVariantMap map = item.toMap();
            lines << QStringLiteral("  %1: %2 %3 %4")
                         .arg(map.value(QStringLiteral("host")).toString(),
                              map.value(QStringLiteral("detail")).toString(),
                              map.value(QStringLiteral("via")).toString(),
                              map.value(QStringLiteral("latency")).toString());
        }
    };
    if (!m_dnsResults.empty()) {
        appendResults(tr("DNS test:"), dnsResults());
    }
    if (!m_connectionResults.empty()) {
        appendResults(tr("Connection test:"), connectionResults());
    }
    lines << QString() << tr("Recent requests:");
    for (const net::RequestRecord& record : m_service.recentRequests()) {
        const QString outcome = record.error ? latin(errorCodeName(*record.error)) : QString::number(record.status);
        lines << QStringLiteral("  %1 %2 %3 → %4 %5 %6 ms (dns %7, connect %8, tls %9) %10")
                     .arg(record.finishedAt.toString(QStringLiteral("HH:mm:ss")), QString::fromLatin1(record.method),
                          record.url, outcome, QString::fromLatin1(record.httpVersion))
                     .arg(record.timings.totalMs, 0, 'f', 0)
                     .arg(record.timings.dnsMs, 0, 'f', 0)
                     .arg(record.timings.connectMs, 0, 'f', 0)
                     .arg(record.timings.tlsMs, 0, 'f', 0)
                     .arg(record.route);
    }
    return lines.join(u'\n');
}

void NetworkController::copyReport()
{
    QGuiApplication::clipboard()->setText(report());
    Q_EMIT notice(QStringLiteral("success"), tr("Network report copied. Passwords and tokens are not included."));
}

}  // namespace noctara::ui
