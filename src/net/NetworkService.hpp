#pragma once

#include "net/Diagnostics.hpp"
#include "net/NetworkConfig.hpp"
#include "net/ProxyRouter.hpp"
#include "net/Resolver.hpp"
#include "net/Transport.hpp"

#include <QObject>
#include <QThread>
#include <QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::net {

class NetworkCore;

struct DnsTestResult {
    QString host;
    Result<Resolution> result;
};

struct ConnectionTestResult {
    QUrl url;
    Result<Response> result;
};

struct ResolverSnapshot {
    int lookups = 0;
    int cacheHits = 0;
    int systemLookups = 0;
    int fallbacks = 0;
    int failures = 0;
    int cacheSize = 0;
    int activeTransfers = 0;
};

// Owns the network stack on its own thread (resolver, proxy router, curl transport) and keeps it in step with
// the network.* settings: every change applies live, and changes to DNS, proxy or TLS make new requests use
// fresh connections while running ones finish on the old ones. Lives on the GUI thread; transport() is
// thread-safe and is what every other module uses for HTTP.
class NetworkService : public QObject {
    Q_OBJECT

public:
    // `environment` is the process environment (for System proxy mode); `desktopProxy` enables libproxy.
    NetworkService(settings::SettingsStore& settings, QHash<QString, QString> environment, bool desktopProxy,
                   QObject* parent = nullptr);
    ~NetworkService() override;
    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;
    NetworkService(NetworkService&&) = delete;
    NetworkService& operator=(NetworkService&&) = delete;

    [[nodiscard]] INetworkTransport& transport() noexcept;

    // Credentials for the manual proxy, from the secret store; never stored in settings.
    void setProxyCredentials(const QString& username, const QString& password);
    // NetworkManager's metered flag, used when the setting is Auto.
    void setSystemMetered(bool metered);

    [[nodiscard]] const NetworkConfig& config() const noexcept { return m_config; }
    [[nodiscard]] bool meteredActive() const noexcept;
    // Where System proxy mode takes its proxy from (environment, desktop settings or nowhere).
    [[nodiscard]] ProxySource systemProxySource() const noexcept { return m_systemProxySource; }
    [[nodiscard]] const std::deque<RequestRecord>& recentRequests() const noexcept { return m_recent; }

    // Resolves Noctara's service hosts with the current settings, bypassing the cache.
    void testDns(QObject* context, std::function<void(std::vector<DnsTestResult>)> callback);
    // Reaches the API and media hosts through the current proxy and DNS settings.
    void testConnection(QObject* context, const std::function<void(std::vector<ConnectionTestResult>)>& callback);
    // How a URL would be routed right now (direct or which proxy), e.g. to show what System mode found.
    void route(const QUrl& url, QObject* context, std::function<void(Result<ProxyRoute>)> callback);
    // Looks up a name with the current DNS settings. Callable from any thread; `callback` runs on `context`'s.
    void resolve(const QString& host, QObject* context, ResolveCallback callback);
    void flushDnsCache();
    void snapshot(QObject* context, std::function<void(ResolverSnapshot)> callback);

    static constexpr std::size_t kRecentRequests = 50;

Q_SIGNALS:
    void configChanged();
    void requestRecorded();

private:
    void scheduleApply();
    void apply();
    void onRecord(const RequestRecord& record);

    settings::SettingsStore& m_settings;
    QThread m_thread;
    QObject* m_anchor = nullptr;    // lives on m_thread: target for calls that must run there
    NetworkCore* m_core = nullptr;  // lives on m_thread; created and destroyed there
    NetworkConfig m_config;
    QString m_proxyUser;
    QString m_proxyPassword;
    bool m_systemMetered = false;
    ProxySource m_systemProxySource = ProxySource::None;
    QTimer m_applyTimer;
    std::deque<RequestRecord> m_recent;
};

}  // namespace noctara::net
