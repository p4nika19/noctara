#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace noctara::net {

enum class Method : std::uint8_t { Get, Head, Post, Put, Delete };

// How the HTTP cache (cache::CachingTransport) treats a request; the plain transport ignores it.
enum class CacheMode : std::uint8_t {
    Default,      // RFC 9111: fresh entries are served, stale ones revalidated; stale served when offline
    Refresh,      // always revalidate with the server (but still store the answer)
    Bypass,       // neither read nor write the cache
    OnlyIfCached  // never touch the network; fails with ErrorCode::Offline on a miss
};

using Headers = QList<QPair<QByteArray, QByteArray>>;  // names in lower case

struct ByteRange {
    std::int64_t first = 0;
    std::int64_t last = -1;  // inclusive; -1 = to the end
};

struct Request {
    Method method = Method::Get;
    QUrl url;
    Headers headers;
    QByteArray body;
    std::optional<ByteRange> range;
    std::chrono::milliseconds timeout{0};  // whole request; 0 = none (the stall timeout still applies)
    bool background = false;               // prefetch and similar: skipped on metered connections
    std::optional<bool> retry;             // default: retry idempotent methods (GET, HEAD, PUT, DELETE)
    int maxRedirects = 10;
    CacheMode cache = CacheMode::Default;
    // Freshness for responses that carry no Cache-Control/Expires: short for volatile endpoints (feed, likes),
    // long for immutable ones (track metadata, waveforms). 0 = revalidate every time.
    std::chrono::seconds cacheTtl{0};
};

// Phase durations of the last attempt, measured from its start.
struct Timings {
    double dnsMs = 0;        // our resolver (0 when the proxy resolves the destination)
    double connectMs = 0;    // TCP connect to the server or proxy done
    double tlsMs = 0;        // TLS handshake done (0 for plain HTTP)
    double firstByteMs = 0;  // first response byte
    double totalMs = 0;
};

struct Response {
    int status = 0;
    QUrl url;  // after redirects
    Headers headers;
    QByteArray body;         // empty when streamed through Callbacks::onData
    QByteArray httpVersion;  // "HTTP/1.1", "HTTP/2", "HTTP/3"
    QString remoteAddress;   // the server's or the proxy's address
    QString route;           // "direct" or the proxy's display URL (credentials redacted)
    Timings timings;
    std::int64_t bytesReceived = 0;
    int attempts = 1;
    bool fromCache = false;  // served by the HTTP cache (fresh, revalidated with 304, or stale while offline)
    bool stale = false;      // served from the cache past its freshness because the network was unavailable

    [[nodiscard]] QByteArray header(QByteArrayView name) const;
    [[nodiscard]] bool successful() const noexcept { return status >= 200 && status < 300; }
};

struct Callbacks {
    // Optional: status and headers of the final response, before its body.
    std::function<void(const Response&)> onResponse;
    // Optional: streams a successful (2xx) body instead of collecting it in Response::body.
    std::function<void(const QByteArray&)> onData;
    // Required. HTTP error statuses are responses, not errors; errors are transport failures.
    std::function<void(Result<Response>)> onFinished;
};

// Lets the caller cancel a request; copies share the request. Dropping the handle does not cancel.
class RequestHandle {
public:
    struct State {
        std::atomic<bool> cancelled{false};
    };

    RequestHandle() = default;
    explicit RequestHandle(std::shared_ptr<State> state)
        : m_state(std::move(state))
    {
    }

    void cancel() const noexcept
    {
        if (m_state) {
            m_state->cancelled.store(true, std::memory_order_relaxed);
        }
    }
    [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }

private:
    std::shared_ptr<State> m_state;
};

// The single gateway for all of Noctara's HTTP traffic (API, images, audio, client_id discovery), so DNS,
// proxy, TLS, limits and diagnostics are implemented once. Thread-safe: callbacks run on `context`'s
// thread, in order, and are dropped if `context` is destroyed first (which also cancels the request).
class INetworkTransport {
public:
    INetworkTransport() = default;
    virtual ~INetworkTransport() = default;
    INetworkTransport(const INetworkTransport&) = delete;
    INetworkTransport& operator=(const INetworkTransport&) = delete;
    INetworkTransport(INetworkTransport&&) = delete;
    INetworkTransport& operator=(INetworkTransport&&) = delete;

    virtual RequestHandle send(Request request, QObject* context, Callbacks callbacks) = 0;
};

[[nodiscard]] bool isIdempotent(Method method) noexcept;
[[nodiscard]] QByteArray methodName(Method method);

}  // namespace noctara::net
