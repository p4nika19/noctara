#pragma once

#include "drm/HelperNetwork.hpp"
#include "net/ProxyRules.hpp"
#include "net/Resolver.hpp"

#include <QThread>

#include <functional>
#include <optional>

class QObject;

namespace noctara::drm {

// Lets the helper use a SOCKS5 proxy that needs a username and password, which Chromium cannot sign in to. The relay
// is an HTTP CONNECT proxy on 127.0.0.1 for the helper alone (Chromium answers its random per-run credentials through
// the same "login" path as any HTTP proxy), and opens each tunnel through the real proxy with RFC 1929 sign-in.
// Destination names go to the proxy, as Chromium's own SOCKS5 does. Only CONNECT is accepted: the helper's traffic is
// HTTPS. Sockets run on the relay's own thread; start() and stop() are for one thread (the GUI's).
class SocksRelay {
public:
    // Looks up the proxy server's own name with Noctara's DNS settings; `done` runs on `context`'s thread.
    using Resolve = std::function<void(const QString& host, QObject* context, net::ResolveCallback done)>;

    explicit SocksRelay(Resolve resolve);
    ~SocksRelay();
    SocksRelay(const SocksRelay&) = delete;
    SocksRelay& operator=(const SocksRelay&) = delete;
    SocksRelay(SocksRelay&&) = delete;
    SocksRelay& operator=(SocksRelay&&) = delete;

    // Listens if it is not listening yet and sends new connections through `upstream`, closing tunnels through a
    // previous one. The port and credentials stay the same until stop(), so the helper needs no restart for a new
    // upstream. nullopt when no local port could be opened.
    [[nodiscard]] std::optional<LocalProxy> start(const net::ProxyServer& upstream);
    void stop();

private:
    class Worker;

    Resolve m_resolve;
    QThread m_thread;
    QObject* m_anchor = nullptr;  // lives on m_thread: target for calls that must run there
    Worker* m_worker = nullptr;   // lives on m_thread; created and destroyed there
    std::optional<LocalProxy> m_endpoint;
};

}  // namespace noctara::drm
