#pragma once

#include "core/Error.hpp"
#include "net/NetworkConfig.hpp"
#include "net/ProxyRules.hpp"

#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace noctara::net {

struct ProxyRoute {
    std::optional<ProxyServer> proxy;  // nullopt: connect directly
    ProxySource source = ProxySource::None;
    QString detail;  // environment variable or "libproxy", for the UI
};

// Decides per URL whether and through which proxy to connect.
class IProxyRouter {
public:
    IProxyRouter() = default;
    virtual ~IProxyRouter() = default;
    IProxyRouter(const IProxyRouter&) = delete;
    IProxyRouter& operator=(const IProxyRouter&) = delete;
    IProxyRouter(IProxyRouter&&) = delete;
    IProxyRouter& operator=(IProxyRouter&&) = delete;

    virtual void route(const QUrl& url, std::function<void(Result<ProxyRoute>)> callback) = 0;
};

// The desktop's proxy settings (GNOME, KDE, PAC/WPAD), asked when no proxy variable is set.
class IDesktopProxy {
public:
    IDesktopProxy() = default;
    virtual ~IDesktopProxy() = default;
    IDesktopProxy(const IDesktopProxy&) = delete;
    IDesktopProxy& operator=(const IDesktopProxy&) = delete;
    IDesktopProxy(IDesktopProxy&&) = delete;
    IDesktopProxy& operator=(IDesktopProxy&&) = delete;

    virtual void query(const QUrl& url, std::function<void(std::optional<ProxyServer>)> callback) = 0;
};

// libproxy on a private worker thread (its lookups may block on PAC scripts). Answers are cached per scheme
// and host for a minute. PAC/WPAD evaluation inside libproxy uses the system resolver: a documented limit of
// strict DNS mode with the System proxy mode.
class LibproxyDesktopProxy : public QObject, public IDesktopProxy {
    Q_OBJECT

public:
    explicit LibproxyDesktopProxy(QObject* parent = nullptr);
    ~LibproxyDesktopProxy() override;
    LibproxyDesktopProxy(const LibproxyDesktopProxy&) = delete;
    LibproxyDesktopProxy& operator=(const LibproxyDesktopProxy&) = delete;
    LibproxyDesktopProxy(LibproxyDesktopProxy&&) = delete;
    LibproxyDesktopProxy& operator=(LibproxyDesktopProxy&&) = delete;

    void query(const QUrl& url, std::function<void(std::optional<ProxyServer>)> callback) override;
    void clearCache() { m_cache.clear(); }

private:
    struct Cached {
        std::optional<ProxyServer> server;
        std::chrono::steady_clock::time_point expires;
    };

    QThreadPool m_pool;
    std::shared_ptr<void> m_factory;  // pxProxyFactory*, created and used on the pool's single thread
    QHash<QString, Cached> m_cache;
};

// None: always direct. Manual: the configured server unless the host is bypassed. System: the proxy
// environment variables when any is set, otherwise the desktop settings.
class ProxyRouter : public QObject, public IProxyRouter {
    Q_OBJECT

public:
    ProxyRouter(QHash<QString, QString> environment, IDesktopProxy* desktop, QObject* parent = nullptr);

    void setConfig(const ProxyConfig& config);

    void route(const QUrl& url, std::function<void(Result<ProxyRoute>)> callback) override;

private:
    QHash<QString, QString> m_environment;
    IDesktopProxy* m_desktop;
    ProxyConfig m_config;
    BypassList m_bypass;
};

[[nodiscard]] QHash<QString, QString> processEnvironment();

}  // namespace noctara::net
