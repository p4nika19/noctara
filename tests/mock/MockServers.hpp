#pragma once

// Local stand-ins for the network: HTTP(S) origin, DNS (UDP+TCP), DoH, HTTP/CONNECT proxy and SOCKS4/5
// proxy. Each runs blocking I/O on its own threads, bound to 127.0.0.1 on a random port, and records what
// it saw so tests can prove where names were resolved.

#include "core/NetAddress.hpp"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QTemporaryDir>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace noctara::test {

// A connected byte stream: plain socket or TLS.
class Stream {
public:
    Stream() = default;
    virtual ~Stream() = default;
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) = delete;
    Stream& operator=(Stream&&) = delete;

    virtual long read(char* buffer, std::size_t size) = 0;  // <= 0: closed or failed
    virtual bool write(const char* data, std::size_t size) = 0;
    [[nodiscard]] virtual int fd() const = 0;

    bool write(const QByteArray& data) { return write(data.constData(), static_cast<std::size_t>(data.size())); }
    // Reads until "\r\n\r\n"; returns everything read (may include the start of the body).
    QByteArray readHead(QByteArray& rest);
};

// Accepts TCP connections and runs `handler` for each on its own thread.
class TcpServer {
public:
    using Handler = std::function<void(int fd)>;
    // Listens on 127.0.0.1, or on ::1 when `ipv6` is set. When `port` cannot be bound, port() is 0 and nothing
    // is accepted.
    explicit TcpServer(Handler handler, std::uint16_t port = 0, bool ipv6 = false);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] int connections() const noexcept { return m_connections.load(); }

private:
    Handler m_handler;
    int m_listen = -1;
    int m_wake = -1;
    std::uint16_t m_port = 0;
    std::atomic<int> m_connections{0};
    std::mutex m_mutex;
    std::vector<std::jthread> m_workers;
    std::jthread m_acceptor;
};

// Self-signed certificate for "app.test", "localhost" and 127.0.0.1, usable as its own CA bundle.
class TestCertificate {
public:
    TestCertificate();
    ~TestCertificate();
    TestCertificate(const TestCertificate&) = delete;
    TestCertificate& operator=(const TestCertificate&) = delete;
    TestCertificate(TestCertificate&&) = delete;
    TestCertificate& operator=(TestCertificate&&) = delete;

    [[nodiscard]] const QString& pemPath() const noexcept { return m_pemPath; }
    [[nodiscard]] void* context() const noexcept { return m_context; }  // SSL_CTX*

private:
    void* m_context = nullptr;
    QTemporaryDir m_dir;
    QString m_pemPath;
};

struct HttpRequest {
    QByteArray method;
    QByteArray target;                         // as sent: origin-form "/path" or absolute-form "http://host/path"
    std::map<QByteArray, QByteArray> headers;  // lower-case names
    QByteArray body;
};

struct HttpReply {
    int status = 200;
    std::vector<std::pair<QByteArray, QByteArray>> headers;
    QByteArray body;
};

// HTTP/1.1 origin server (keep-alive), optionally over TLS.
class HttpServer {
public:
    using Handler = std::function<HttpReply(const HttpRequest&)>;
    explicit HttpServer(Handler handler, const TestCertificate* tls = nullptr, bool ipv6 = false);

    [[nodiscard]] std::uint16_t port() const noexcept { return m_server.port(); }
    [[nodiscard]] std::vector<HttpRequest> requests() const;
    [[nodiscard]] int connections() const noexcept { return m_server.connections(); }

private:
    void serve(Stream& stream);

    Handler m_handler;
    mutable std::mutex m_mutex;
    std::vector<HttpRequest> m_requests;
    const TestCertificate* m_tls;
    TcpServer m_server;
};

// Answers A/AAAA from a table over UDP and TCP on the same port. NXDOMAIN for unknown names.
class DnsServer {
public:
    DnsServer();
    ~DnsServer();
    DnsServer(const DnsServer&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;
    DnsServer(DnsServer&&) = delete;
    DnsServer& operator=(DnsServer&&) = delete;

    void add(const QString& name, const QString& address, std::uint32_t ttl = 300);
    void setTruncateUdp(bool truncate) { m_truncate = truncate; }
    // Answers TCP queries with an id that does not match the query's.
    void setWrongTcpId(bool wrong) { m_wrongTcpId = wrong; }
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] std::uint16_t tcpPort() const noexcept { return m_tcp->port(); }
    [[nodiscard]] int udpQueries() const noexcept { return m_udpQueries.load(); }
    [[nodiscard]] int tcpQueries() const noexcept { return m_tcpQueries.load(); }

    // Builds the answer to a raw query (shared with the DoH server).
    [[nodiscard]] QByteArray answer(const QByteArray& query, bool truncate) const;

private:
    struct Record {
        QString address;
        std::uint32_t ttl = 0;
    };
    mutable std::mutex m_mutex;
    std::multimap<QString, Record> m_records;
    std::atomic<bool> m_truncate{false};
    std::atomic<bool> m_wrongTcpId{false};
    std::atomic<int> m_udpQueries{0};
    std::atomic<int> m_tcpQueries{0};
    int m_udp = -1;
    int m_wake = -1;
    std::uint16_t m_port = 0;
    std::jthread m_udpThread;
    std::unique_ptr<TcpServer> m_tcp;
};

// RFC 8484 POST endpoint at /dns-query backed by a DnsServer table.
class DohServer {
public:
    explicit DohServer(const DnsServer& table, bool ipv6 = false);
    [[nodiscard]] std::uint16_t port() const noexcept { return m_http.port(); }
    [[nodiscard]] QString url() const
    {
        return QStringLiteral("http://%1:%2/dns-query").arg(m_ipv6 ? u"[::1]" : u"127.0.0.1").arg(m_http.port());
    }
    [[nodiscard]] int queries() const noexcept { return static_cast<int>(m_http.requests().size()); }

private:
    bool m_ipv6;
    HttpServer m_http;
};

// HTTP proxy: absolute-form requests are answered by the proxy itself; CONNECT is tunnelled to the target
// port on 127.0.0.1 (the proxy "resolves" every name to loopback). Optionally demands Basic credentials.
// Also speaks SOCKS4/4a/5/5h on the same kind of server (see SocksProxy).
class HttpProxy {
public:
    explicit HttpProxy(QByteArray requiredAuth = {});  // "user:pass" or empty

    [[nodiscard]] std::uint16_t port() const noexcept { return m_server.port(); }
    [[nodiscard]] std::vector<QByteArray> requestLines() const;

private:
    void serve(int fd);

    QByteArray m_auth;
    mutable std::mutex m_mutex;
    std::vector<QByteArray> m_lines;
    TcpServer m_server;
};

class SocksProxy {
public:
    struct Connect {
        int version = 5;
        int addressType = 0;  // SOCKS5: 1 IPv4, 3 domain name, 4 IPv6; SOCKS4: 1, SOCKS4a: 3
        QByteArray host;
        std::uint16_t port = 0;
    };

    explicit SocksProxy(QByteArray requiredAuth = {});

    [[nodiscard]] std::uint16_t port() const noexcept { return m_server.port(); }
    [[nodiscard]] std::vector<Connect> connects() const;

private:
    void serve(int fd);

    QByteArray m_auth;
    mutable std::mutex m_mutex;
    std::vector<Connect> m_connects;
    TcpServer m_server;
};

}  // namespace noctara::test
