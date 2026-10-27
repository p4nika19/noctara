#pragma once

#include "net/Diagnostics.hpp"
#include "net/DnsUpstream.hpp"
#include "net/NetworkConfig.hpp"
#include "net/ProxyRouter.hpp"
#include "net/Resolver.hpp"
#include "net/TokenBucket.hpp"
#include "net/Transport.hpp"

#include <QObject>
#include <QTimer>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace noctara::net {

class CurlSession;
struct Transfer;

// INetworkTransport on libcurl's multi interface, driven by the Qt event loop of the thread this object lives
// in (QSocketNotifier per socket, one QTimer). No thread per request.
//
// Every host curl connects to is resolved by IDnsResolver first and pinned with CURLOPT_RESOLVE; a
// resolver-start guard fails any transfer for which curl would resolve a name itself, so DNS settings and
// strict mode cannot be bypassed. Behind a proxy that resolves remotely (HTTP, HTTPS, SOCKS4a, SOCKS5h) only
// the proxy's own name is resolved locally. Redirects are followed here, not by curl, so each new host goes
// through the same resolver and proxy rules.
class CurlTransport : public QObject, public INetworkTransport {
    Q_OBJECT

public:
    CurlTransport(IDnsResolver& resolver, IProxyRouter& router, QObject* parent = nullptr);
    ~CurlTransport() override;
    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;
    CurlTransport(CurlTransport&&) = delete;
    CurlTransport& operator=(CurlTransport&&) = delete;

    // On the owning thread. Changes take effect for new transfers; going offline aborts running ones.
    void setConfig(const TransportConfig& config);
    void setMetered(bool metered);
    // New transfers use fresh connections from now on; existing connections close once their transfers end.
    void drainConnections();

    RequestHandle send(Request request, QObject* context, Callbacks callbacks) override;

    // DoH queries: POSTed directly (never through a proxy) to the pinned addresses, without retries.
    void sendDoh(const QUrl& url, std::vector<IpAddress> pinned, QByteArray query,
                 std::function<void(Result<QByteArray>)> callback);
    [[nodiscard]] DohSender dohSender();

    [[nodiscard]] int activeTransfers() const noexcept { return static_cast<int>(m_transfers.size()); }
    // Times curl tried to resolve a name on its own. Always 0 unless there is a bug; checked by tests.
    [[nodiscard]] int unexpectedCurlResolves() const noexcept { return m_unexpectedResolves; }

Q_SIGNALS:
    void requestFinished(const noctara::net::RequestRecord& record);

private:
    friend class CurlSession;

    void begin(std::unique_ptr<Transfer> transfer);
    void startAttempt(std::uint64_t id);
    void onRouted(std::uint64_t id, Result<ProxyRoute> route);
    void launch(std::uint64_t id, const std::vector<QByteArray>& resolveEntries);
    void onTransferDone(Transfer& transfer, int code);
    void retryLater(Transfer& transfer, std::optional<std::chrono::seconds> retryAfter);
    void followRedirect(Transfer& transfer, const QUrl& target);
    void finish(std::uint64_t id, Result<Response> result);
    void record(const Transfer& transfer, const Result<Response>& result);
    void detach(Transfer& transfer);
    void pauseForBandwidth(Transfer& transfer);
    void resumePaused();
    [[nodiscard]] Transfer* find(std::uint64_t id);
    [[nodiscard]] bool retryAllowed(const Transfer& transfer) const;

    static std::size_t onWrite(char* data, std::size_t size, std::size_t count, void* user);
    static std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* user);
    static int onProgress(void* user, std::int64_t, std::int64_t, std::int64_t, std::int64_t);
    static int onResolverStart(void* resolverState, void* reserved, void* user);

    IDnsResolver& m_resolver;
    IProxyRouter& m_router;
    TransportConfig m_config;
    bool m_metered = false;
    std::unordered_map<std::uint64_t, std::unique_ptr<Transfer>> m_transfers;
    std::uint64_t m_nextId = 1;
    std::unique_ptr<CurlSession> m_session;
    std::vector<std::unique_ptr<CurlSession>> m_draining;
    TokenBucket m_bandwidth;
    QTimer m_refill;
    int m_unexpectedResolves = 0;
};

}  // namespace noctara::net
