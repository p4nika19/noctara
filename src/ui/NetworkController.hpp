#pragma once

#include "net/NetworkService.hpp"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <optional>
#include <vector>

namespace noctara::net {
class NetworkMonitor;
}
namespace noctara::auth {
class ISecretStore;
}
namespace noctara::i18n {
class LanguageManager;
}

namespace noctara::ui {

class SettingsController;

// QML "Network" singleton: what the network stack is doing (connectivity, DNS and proxy in effect, the
// DNS <-> proxy interaction), the Test DNS / Test connection / Flush actions, proxy credentials in the secret
// store, recent requests, and a redacted, copyable report. Also keeps the status lines of network rows current.
class NetworkController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString connectivity READ connectivity NOTIFY statusChanged)  // unknown|none|portal|limited|full
    Q_PROPERTY(bool metered READ metered NOTIFY statusChanged)
    Q_PROPERTY(bool offline READ offline NOTIFY statusChanged)
    Q_PROPERTY(QString dnsSummary READ dnsSummary NOTIFY statusChanged)
    Q_PROPERTY(QString proxySummary READ proxySummary NOTIFY statusChanged)
    Q_PROPERTY(QString dnsProxyHint READ dnsProxyHint NOTIFY statusChanged)
    Q_PROPERTY(bool hasProxyCredentials READ hasProxyCredentials NOTIFY credentialsChanged)
    Q_PROPERTY(QString proxyUsername READ proxyUsername NOTIFY credentialsChanged)
    Q_PROPERTY(bool keyringAvailable READ keyringAvailable NOTIFY credentialsChanged)
    Q_PROPERTY(bool dnsTesting READ dnsTesting NOTIFY dnsTestChanged)
    Q_PROPERTY(QVariantList dnsResults READ dnsResults NOTIFY dnsTestChanged)
    Q_PROPERTY(bool connectionTesting READ connectionTesting NOTIFY connectionTestChanged)
    Q_PROPERTY(QVariantList connectionResults READ connectionResults NOTIFY connectionTestChanged)
    Q_PROPERTY(QVariantList recentRequests READ recentRequests NOTIFY recentRequestsChanged)

public:
    NetworkController(net::NetworkService& service, net::NetworkMonitor* monitor, auth::ISecretStore& secrets,
                      i18n::LanguageManager& languages, SettingsController& settingsUi, QObject* parent = nullptr);

    [[nodiscard]] QString connectivity() const;
    [[nodiscard]] bool metered() const;
    [[nodiscard]] bool offline() const;
    [[nodiscard]] QString dnsSummary() const;
    [[nodiscard]] QString proxySummary() const;
    [[nodiscard]] QString dnsProxyHint() const;
    [[nodiscard]] bool hasProxyCredentials() const noexcept { return !m_username.isEmpty() || !m_password.isEmpty(); }
    [[nodiscard]] const QString& proxyUsername() const noexcept { return m_username; }
    [[nodiscard]] bool keyringAvailable() const;
    [[nodiscard]] bool dnsTesting() const noexcept { return m_dnsTesting; }
    [[nodiscard]] QVariantList dnsResults() const;
    [[nodiscard]] bool connectionTesting() const noexcept { return m_connectionTesting; }
    [[nodiscard]] QVariantList connectionResults() const;
    [[nodiscard]] QVariantList recentRequests() const;

    Q_INVOKABLE void testDns();
    Q_INVOKABLE void testConnection();
    Q_INVOKABLE void flushDnsCache();
    // Empty password and username forget the credentials.
    Q_INVOKABLE void saveProxyCredentials(const QString& username, const QString& password);
    Q_INVOKABLE void forgetProxyCredentials();
    // Copies the redacted diagnostics report to the clipboard.
    Q_INVOKABLE void copyReport();
    [[nodiscard]] QString report() const;

    static constexpr auto kCredentialsKey = "network.proxy.credentials";

Q_SIGNALS:
    void statusChanged();
    void credentialsChanged();
    void dnsTestChanged();
    void connectionTestChanged();
    void recentRequestsChanged();
    // kind: "success" | "warning" | "error"; for the page's toasts.
    void notice(const QString& kind, const QString& message);

private:
    void loadCredentials();
    void refresh();
    void updateRowStatuses();

    net::NetworkService& m_service;
    net::NetworkMonitor* m_monitor;
    auth::ISecretStore& m_secrets;
    SettingsController& m_settingsUi;
    std::optional<net::ProxyServer> m_systemProxy;  // what System mode routes the API through (nullopt: direct)
    QString m_username;
    QString m_password;
    bool m_dnsTesting = false;
    bool m_connectionTesting = false;
    // Kept raw and translated when read, so a language switch needs no new test.
    std::vector<net::DnsTestResult> m_dnsResults;
    std::vector<net::ConnectionTestResult> m_connectionResults;
    QTimer m_recentThrottle;  // the request list updates at most twice a second
};

}  // namespace noctara::ui
