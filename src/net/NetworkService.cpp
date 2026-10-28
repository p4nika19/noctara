#include "net/NetworkService.hpp"

#include "core/Handoff.hpp"
#include "core/Log.hpp"
#include "net/CallbackChannel.hpp"
#include "net/CurlTransport.hpp"
#include "net/DnsResolver.hpp"
#include "net/SystemResolver.hpp"
#include "settings/SettingsStore.hpp"

#include <QPointer>

namespace noctara::net {

// Everything that lives on the network thread.
class NetworkCore : public QObject {
public:
    NetworkCore(QHash<QString, QString> environment, bool desktopProxy)
        : system(this)
        , resolver(system, this)
        , desktop(desktopProxy ? std::make_unique<LibproxyDesktopProxy>() : nullptr)
        , router(std::move(environment), desktop.get(), this)
        , transport(resolver, router, this)
    {
        resolver.setDohSender(transport.dohSender());
    }

    void apply(const NetworkConfig& config, bool metered, bool routingChanged)
    {
        resolver.setConfig(config.dns);
        router.setConfig(config.proxy);
        transport.setConfig(config.transport);
        transport.setMetered(metered);
        if (routingChanged) {
            if (desktop) {
                desktop->clearCache();
            }
            transport.drainConnections();
        }
    }

    SystemResolver system;
    DnsResolver resolver;
    std::unique_ptr<LibproxyDesktopProxy> desktop;
    ProxyRouter router;
    CurlTransport transport;
};

NetworkService::NetworkService(settings::SettingsStore& settings, QHash<QString, QString> environment,
                               bool desktopProxy, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_anchor(new QObject)  // NOLINT(cppcoreguidelines-owning-memory): deleted on the network thread
{
    qRegisterMetaType<RequestRecord>();
    m_thread.setObjectName(QStringLiteral("noctara-network"));
    m_thread.start();
    const bool envProxy = hasEnvironmentProxy(environment);
    m_systemProxySource = envProxy ? ProxySource::Environment : desktopProxy ? ProxySource::Desktop : ProxySource::None;
    // Everything on the network thread is created there, so its timers and socket notifiers belong to it.
    m_anchor->moveToThread(&m_thread);
    handoff::runAndWait(m_anchor, [&] {
        m_core = new NetworkCore(std::move(environment), desktopProxy);  // NOLINT(cppcoreguidelines-owning-memory)
    });
    connect(&m_core->transport, &CurlTransport::requestFinished, this, &NetworkService::onRecord);

    m_applyTimer.setSingleShot(true);
    m_applyTimer.setInterval(0);
    connect(&m_applyTimer, &QTimer::timeout, this, &NetworkService::apply);
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key.startsWith(u"network.")) {
            scheduleApply();
        }
    });
    apply();
}

NetworkService::~NetworkService()
{
    handoff::runAndWait(m_anchor, [this] {
        delete std::exchange(m_core, nullptr);    // NOLINT(cppcoreguidelines-owning-memory)
        delete std::exchange(m_anchor, nullptr);  // NOLINT(cppcoreguidelines-owning-memory)
    });
    m_thread.quit();
    m_thread.wait();
}

INetworkTransport& NetworkService::transport() noexcept
{
    return m_core->transport;  // send() is thread-safe
}

void NetworkService::setProxyCredentials(const QString& username, const QString& password)
{
    m_proxyUser = username;
    m_proxyPassword = password;
    scheduleApply();
}

void NetworkService::setSystemMetered(bool metered)
{
    if (metered != m_systemMetered) {
        m_systemMetered = metered;
        scheduleApply();
    }
}

bool NetworkService::meteredActive() const noexcept
{
    switch (m_config.transport.metered) {
    case MeteredMode::On: return true;
    case MeteredMode::Off: return false;
    case MeteredMode::Auto: return m_systemMetered;
    }
    return false;
}

void NetworkService::scheduleApply()
{
    m_applyTimer.start();
}

void NetworkService::apply()
{
    NetworkConfig config = configFromSettings(m_settings);
    config.proxy.manual.username = m_proxyUser;
    config.proxy.manual.password = m_proxyPassword;
    const bool routingChanged = config.dns != m_config.dns || config.proxy != m_config.proxy;
    const bool changed = !(config == m_config);
    m_config = config;
    const bool metered = meteredActive();
    handoff::post(m_core,
                  [core = m_core, config, metered, routingChanged] { core->apply(config, metered, routingChanged); });
    if (changed) {
        Q_EMIT configChanged();
    }
}

void NetworkService::onRecord(const RequestRecord& record)
{
    handoff::barrier();
    m_recent.push_front(record);
    if (m_recent.size() > kRecentRequests) {
        m_recent.pop_back();
    }
    Q_EMIT requestRecorded();
}

void NetworkService::testDns(QObject* context, std::function<void(std::vector<DnsTestResult>)> callback)
{
    auto* channel = new CallbackChannel(context);  // NOLINT(cppcoreguidelines-owning-memory): deleted when done
    channel->moveToThread(&m_thread);
    handoff::post(m_core, [core = m_core, channel, callback = std::move(callback)] {
        const auto hosts = serviceHosts();
        const auto results = std::make_shared<std::vector<DnsTestResult>>(hosts.size());
        const auto remaining = std::make_shared<std::size_t>(hosts.size());
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            const QString host = QString::fromLatin1(hosts[i]);
            (*results)[i].host = host;
            (*results)[i].result = fail(ErrorCode::Unknown);
            core->resolver.resolveFresh(host, [=](Result<Resolution> result) {
                (*results)[i].result = std::move(result);
                if (--*remaining == 0) {
                    channel->post([callback, results] { callback(*results); });
                    channel->deleteLater();
                }
            });
        }
    });
}

void NetworkService::testConnection(QObject* context,
                                    const std::function<void(std::vector<ConnectionTestResult>)>& callback)
{
    const std::vector<QUrl> targets{QUrl(QStringLiteral("https://api-v2.soundcloud.com/")),
                                    QUrl(QStringLiteral("https://cf-media.sndcdn.com/")),
                                    QUrl(QStringLiteral("https://i1.sndcdn.com/"))};
    const auto results = std::make_shared<std::vector<ConnectionTestResult>>(targets.size());
    const auto remaining = std::make_shared<std::size_t>(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        (*results)[i].url = targets[i];
        Request request;
        request.method = Method::Head;
        request.url = targets[i];
        request.retry = false;
        request.maxRedirects = 0;
        request.timeout = std::chrono::milliseconds(m_config.transport.connectTimeout) + std::chrono::seconds(10);
        transport().send(std::move(request), context,
                         {.onResponse = {}, .onData = {}, .onFinished = [=](Result<Response> result) {
                              (*results)[i].result = std::move(result);
                              if (--*remaining == 0) {
                                  callback(*results);
                              }
                          }});
    }
}

void NetworkService::route(const QUrl& url, QObject* context, std::function<void(Result<ProxyRoute>)> callback)
{
    auto* channel = new CallbackChannel(context);  // NOLINT(cppcoreguidelines-owning-memory): deleted when done
    channel->moveToThread(&m_thread);
    handoff::post(m_core, [core = m_core, url, channel, callback = std::move(callback)] {
        core->router.route(url, [channel, callback](Result<ProxyRoute> route) {
            channel->post([callback, route] { callback(route); });
            channel->deleteLater();
        });
    });
}

void NetworkService::flushDnsCache()
{
    handoff::post(m_core, [core = m_core] { core->resolver.flush(); });
}

void NetworkService::snapshot(QObject* context, std::function<void(ResolverSnapshot)> callback)
{
    auto* channel = new CallbackChannel(context);  // NOLINT(cppcoreguidelines-owning-memory): deleted when done
    channel->moveToThread(&m_thread);
    handoff::post(m_core, [core = m_core, channel, callback = std::move(callback)] {
        const ResolverStats& stats = core->resolver.stats();
        const ResolverSnapshot snapshot{.lookups = stats.lookups,
                                        .cacheHits = stats.cacheHits,
                                        .systemLookups = stats.systemLookups,
                                        .fallbacks = stats.fallbacks,
                                        .failures = stats.failures,
                                        .cacheSize = static_cast<int>(core->resolver.cacheSize()),
                                        .activeTransfers = core->transport.activeTransfers()};
        channel->post([callback, snapshot] { callback(snapshot); });
        channel->deleteLater();
    });
}

}  // namespace noctara::net
