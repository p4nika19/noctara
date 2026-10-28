#pragma once

#include "cache/DiskLru.hpp"
#include "cache/Mailbox.hpp"
#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QObject>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace noctara::cache {

class FileOps;

// --- RFC 9111 rules (pure) ---

struct CacheControl {
    bool noStore = false;
    bool noCache = false;
    bool mustRevalidate = false;
    bool immutable = false;
    std::optional<std::int64_t> maxAge;  // seconds
    std::optional<std::int64_t> staleIfError;
};

[[nodiscard]] CacheControl parseCacheControl(QByteArrayView value);

// A response as stored in the HTTP cache.
struct StoredResponse {
    int status = 0;
    QUrl url;
    net::Headers headers;
    QByteArray body;
    std::int64_t storedAt = 0;     // unix seconds when received or last revalidated
    std::int64_t ttlFallback = 0;  // seconds; the request's cacheTtl when the server gave no freshness
    net::Headers varyValues;       // request header values named by Vary

    [[nodiscard]] QByteArray header(QByteArrayView name) const;
    [[nodiscard]] QByteArray serialize() const;
    [[nodiscard]] static Result<StoredResponse> parse(const QByteArray& data);
};

// Whether a response may be stored. This is a private (single-user) cache, so responses to authorised requests
// are stored too (the key includes the credentials); no-store never is.
[[nodiscard]] bool isStorable(const net::Request& request, const net::Response& response);
// Seconds the response stays fresh: max-age, else Expires - Date, else the request's fallback TTL.
[[nodiscard]] std::int64_t freshnessLifetime(const StoredResponse& stored);
[[nodiscard]] bool isFresh(const StoredResponse& stored, std::int64_t now);
// Cache key: method + URL + a digest of the credentials, so one account never sees another's cached data.
[[nodiscard]] QByteArray httpCacheKey(const net::Request& request);
[[nodiscard]] bool varyMatches(const StoredResponse& stored, const net::Request& request);

// --- The cache ---

// INetworkTransport decorator that serves GET requests from <cache>/http per RFC 9111: fresh responses
// without contacting the server, stale ones after revalidation (If-None-Match / If-Modified-Since; a 304
// refreshes the stored copy), and stale ones when the network fails or Offline mode is on (Response::stale).
// Streaming and range requests, other methods and CacheMode::Bypass go straight to the inner transport;
// successful unsafe methods invalidate the URL. Lives on the cache thread; send() is thread-safe.
class CachingTransport : public QObject, public net::INetworkTransport {
    Q_OBJECT

public:
    CachingTransport(net::INetworkTransport& inner, FileOps& ops, QObject* parent = nullptr);
    ~CachingTransport() override;
    CachingTransport(const CachingTransport&) = delete;
    CachingTransport& operator=(const CachingTransport&) = delete;
    CachingTransport(CachingTransport&&) = delete;
    CachingTransport& operator=(CachingTransport&&) = delete;

    // Cache thread. An empty directory disables storing (everything passes through).
    void setDisk(const QString& directory, std::int64_t budget);
    void clear();
    void flush();
    [[nodiscard]] std::int64_t diskBytes() const;

    net::RequestHandle send(net::Request request, QObject* context, net::Callbacks callbacks) override;

    static constexpr qsizetype kMaxStoredBody = 4 << 20;

private:
    struct Pending;

    void handle(const std::shared_ptr<Pending>& pending);
    void forward(const std::shared_ptr<Pending>& pending, std::optional<StoredResponse> stored);
    void complete(const std::shared_ptr<Pending>& pending, Result<net::Response> result);

    net::INetworkTransport& m_inner;
    FileOps& m_ops;
    MailboxPtr m_mailbox;
    std::unique_ptr<DiskLru> m_disk;
};

}  // namespace noctara::cache
