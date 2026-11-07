#include "net/ProxyRouter.hpp"

#include "core/Log.hpp"

#include <QProcessEnvironment>
#include <QtConcurrent/QtConcurrentRun>

#include <proxy.h>

namespace noctara::net {

namespace {

constexpr std::chrono::seconds kDesktopCacheLifetime{60};

pxProxyFactory* factoryOf(const std::shared_ptr<void>& holder)
{
    return static_cast<pxProxyFactory*>(holder.get());
}

// libproxy answers with a list such as {"http://proxy:3128", "direct://"}; the first usable entry wins.
std::optional<ProxyServer> askLibproxy(pxProxyFactory* factory, const QByteArray& url)
{
    char** proxies = px_proxy_factory_get_proxies(factory, url.constData());
    if (proxies == nullptr) {
        return std::nullopt;
    }
    std::optional<ProxyServer> chosen;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): NULL-terminated C array from libproxy
    for (char* const* entry = proxies; *entry != nullptr; ++entry) {
        const QString value = QString::fromUtf8(*entry);
        if (value.startsWith(u"direct://")) {
            break;
        }
        if (auto server = ProxyServer::fromUrl(value)) {
            chosen = std::move(server);
            break;
        }
    }
    px_proxy_factory_free_proxies(proxies);
    return chosen;
}

}  // namespace

LibproxyDesktopProxy::LibproxyDesktopProxy(QObject* parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
    m_pool.setExpiryTimeout(-1);  // keep the worker (and libproxy's per-factory state) alive
}

LibproxyDesktopProxy::~LibproxyDesktopProxy()
{
    m_pool.clear();
    m_pool.waitForDone();
    if (m_factory) {
        // Freed on the worker that created it.
        QtConcurrent::run(&m_pool, [factory = std::move(m_factory)] mutable { factory.reset(); }).waitForFinished();
    }
}

void LibproxyDesktopProxy::query(const QUrl& url, std::function<void(std::optional<ProxyServer>)> callback)
{
    const QString key = url.scheme() + u"://" + url.host();
    const auto now = std::chrono::steady_clock::now();
    if (const auto it = m_cache.constFind(key); it != m_cache.constEnd() && it->expires > now) {
        QMetaObject::invokeMethod(this, [callback, server = it->server] { callback(server); }, Qt::QueuedConnection);
        return;
    }
    // Query with scheme and host only: libproxy may hand the URL to a PAC script, which needs no path or query.
    const QByteArray target = key.toUtf8() + '/';
    QtConcurrent::run(&m_pool, [this, target] {
        if (!m_factory) {
            m_factory.reset(px_proxy_factory_new(),
                            [](void* f) { px_proxy_factory_free(static_cast<pxProxyFactory*>(f)); });
        }
        return askLibproxy(factoryOf(m_factory), target);
    }).then(this, [this, key, callback](const std::optional<ProxyServer>& server) {
        m_cache.insert(key, {server, std::chrono::steady_clock::now() + kDesktopCacheLifetime});
        callback(server);
    });
}

ProxyRouter::ProxyRouter(QHash<QString, QString> environment, IDesktopProxy* desktop, QObject* parent)
    : QObject(parent)
    , m_environment(std::move(environment))
    , m_desktop(desktop)
{
}

void ProxyRouter::setConfig(const ProxyConfig& config)
{
    m_config = config;
    m_bypass = BypassList(config.bypass, config.bypassLocal);
}

void ProxyRouter::route(const QUrl& url, std::function<void(Result<ProxyRoute>)> callback)
{
    const auto later = [this, callback](Result<ProxyRoute> result) {
        QMetaObject::invokeMethod(this, [callback, result] { callback(result); }, Qt::QueuedConnection);
    };
    switch (m_config.mode) {
    case ProxyMode::None: later(ProxyRoute{}); return;
    case ProxyMode::Manual:
        if (!m_config.manual.valid()) {
            later(fail(ErrorCode::ProxyNotConfigured, "manual proxy without host"));
        } else if (m_bypass.matches(url.host())) {
            later(ProxyRoute{std::nullopt, ProxySource::Manual, {}});
        } else {
            later(ProxyRoute{m_config.manual, ProxySource::Manual, {}});
        }
        return;
    case ProxyMode::System: break;
    }
    if (hasEnvironmentProxy(m_environment)) {
        EnvironmentProxy env = environmentProxy(m_environment, url);
        later(ProxyRoute{std::move(env.server), ProxySource::Environment, env.variable});
        return;
    }
    if (m_desktop == nullptr) {
        later(ProxyRoute{});
        return;
    }
    m_desktop->query(url, [callback](std::optional<ProxyServer> server) {
        callback(ProxyRoute{std::move(server), ProxySource::Desktop, QStringLiteral("libproxy")});
    });
}

QHash<QString, QString> processEnvironment()
{
    QHash<QString, QString> out;
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString& key : environment.keys()) {
        out.insert(key, environment.value(key));
    }
    return out;
}

}  // namespace noctara::net
