#pragma once

#include "core/NetAddress.hpp"
#include "net/DnsMessage.hpp"

#include <QObject>
#include <QString>
#include <QUrl>

#include <chrono>
#include <functional>
#include <vector>

namespace noctara::net {

struct UpstreamAnswer {
    dns::Response response;
    QString server;  // which server answered, for diagnostics
};
using UpstreamCallback = std::function<void(Result<UpstreamAnswer>)>;

// Asks DNS servers one question. Callbacks run on the owning thread, never inside query().
class IDnsUpstream {
public:
    IDnsUpstream() = default;
    virtual ~IDnsUpstream() = default;
    IDnsUpstream(const IDnsUpstream&) = delete;
    IDnsUpstream& operator=(const IDnsUpstream&) = delete;
    IDnsUpstream(IDnsUpstream&&) = delete;
    IDnsUpstream& operator=(IDnsUpstream&&) = delete;

    virtual void query(const QByteArray& name, dns::RecordType type, UpstreamCallback callback) = 0;
};

// Classic DNS: UDP first, the same server over TCP when the answer is truncated, then the next server on
// timeout, network error, SERVFAIL or REFUSED (ordered failover). Answers must come from the server asked,
// carry the query's random id and echo its question.
class ServerUpstream : public QObject, public IDnsUpstream {
    Q_OBJECT

public:
    ServerUpstream(std::vector<Endpoint> servers, std::chrono::milliseconds timeout, QObject* parent = nullptr);

    void query(const QByteArray& name, dns::RecordType type, UpstreamCallback callback) override;

private:
    void attempt(std::size_t serverIndex, bool tcp, const QByteArray& name, dns::RecordType type,
                 const UpstreamCallback& callback);

    std::vector<Endpoint> m_servers;
    std::chrono::milliseconds m_timeout;
};

// Sends one DoH request (RFC 8484 POST, application/dns-message) to the server at the pinned addresses;
// provided by the transport so DoH shares TLS, HTTP/2 and timeouts with everything else.
using DohSender = std::function<void(const QUrl& url, std::vector<IpAddress> pinned, QByteArray query,
                                     std::function<void(Result<QByteArray>)> callback)>;

// True when the DoH URL may be used: https, or http to a loopback/LAN server with "allow insecure" on.
[[nodiscard]] bool dohUrlAllowed(const QUrl& url, const std::vector<IpAddress>& bootstrap, bool allowInsecure);

// DNS over HTTPS. The DoH host's own addresses are supplied by the caller (bootstrap addresses, or the
// system resolver when fallback is allowed), so resolving the resolver is never circular.
class DohUpstream : public QObject, public IDnsUpstream {
    Q_OBJECT

public:
    DohUpstream(QUrl url, std::vector<IpAddress> addresses, DohSender sender, QObject* parent = nullptr);

    void query(const QByteArray& name, dns::RecordType type, UpstreamCallback callback) override;

private:
    QUrl m_url;
    std::vector<IpAddress> m_addresses;
    DohSender m_sender;
};

}  // namespace noctara::net
