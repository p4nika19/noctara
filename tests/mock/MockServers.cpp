#include "MockServers.hpp"

#include <QList>

#include <arpa/inet.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <set>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

// Test scaffolding: raw POSIX and OpenSSL calls are the point here.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-cstyle-cast)

namespace noctara::test {

namespace {

constexpr int kIoTimeoutSeconds = 10;

class FdStream : public Stream {
public:
    explicit FdStream(int fd)
        : m_fd(fd)
    {
    }
    long read(char* buffer, std::size_t size) override { return ::recv(m_fd, buffer, size, 0); }
    bool write(const char* data, std::size_t size) override
    {
        while (size > 0) {
            const ssize_t sent = ::send(m_fd, data, size, MSG_NOSIGNAL);
            if (sent <= 0) {
                return false;
            }
            data += sent;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }
    [[nodiscard]] int fd() const override { return m_fd; }
    using Stream::write;

private:
    int m_fd;
};

class TlsStream : public Stream {
public:
    TlsStream(int fd, SSL_CTX* context)
        : m_fd(fd)
        , m_ssl(SSL_new(context))
    {
        SSL_set_fd(m_ssl, fd);
        m_ok = SSL_accept(m_ssl) == 1;
    }
    ~TlsStream() override
    {
        if (m_ok) {
            SSL_shutdown(m_ssl);
        }
        SSL_free(m_ssl);
    }
    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    TlsStream(TlsStream&&) = delete;
    TlsStream& operator=(TlsStream&&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    long read(char* buffer, std::size_t size) override { return SSL_read(m_ssl, buffer, static_cast<int>(size)); }
    bool write(const char* data, std::size_t size) override
    {
        return SSL_write(m_ssl, data, static_cast<int>(size)) == static_cast<int>(size);
    }
    [[nodiscard]] int fd() const override { return m_fd; }

private:
    int m_fd;
    SSL* m_ssl;
    bool m_ok = false;
};

bool readExact(int fd, char* buffer, std::size_t size)
{
    while (size > 0) {
        const ssize_t got = ::recv(fd, buffer, size, 0);
        if (got <= 0) {
            return false;
        }
        buffer += got;  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        size -= static_cast<std::size_t>(got);
    }
    return true;
}

int connectLoopback(std::uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Copies bytes both ways until either side closes.
void tunnel(int client, std::uint16_t port, const QByteArray& early)
{
    const int upstream = connectLoopback(port);
    if (upstream < 0) {
        return;
    }
    FdStream(upstream).write(early);
    std::array<pollfd, 2> fds{pollfd{client, POLLIN, 0}, pollfd{upstream, POLLIN, 0}};
    std::array<char, 16384> buffer{};
    while (::poll(fds.data(), fds.size(), kIoTimeoutSeconds * 1000) > 0) {
        bool closed = false;
        for (std::size_t i = 0; i < 2; ++i) {
            if ((fds.at(i).revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            const ssize_t got = ::recv(fds.at(i).fd, buffer.data(), buffer.size(), 0);
            if (got <= 0 || !FdStream(fds.at(1 - i).fd).write(buffer.data(), static_cast<std::size_t>(got))) {
                closed = true;
            }
        }
        if (closed) {
            break;
        }
    }
    ::close(upstream);
}

QByteArray reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 302: return "Found";
    case 404: return "Not Found";
    case 407: return "Proxy Authentication Required";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

void put16(QByteArray& out, std::uint16_t value)
{
    out.append(static_cast<char>(value >> 8U));
    out.append(static_cast<char>(value & 0xffU));
}

void put32(QByteArray& out, std::uint32_t value)
{
    put16(out, static_cast<std::uint16_t>(value >> 16U));
    put16(out, static_cast<std::uint16_t>(value & 0xffffU));
}

}  // namespace

QByteArray Stream::readHead(QByteArray& rest)
{
    QByteArray data = std::exchange(rest, {});
    std::array<char, 4096> buffer{};
    while (!data.contains("\r\n\r\n")) {
        const long got = read(buffer.data(), buffer.size());
        if (got <= 0) {
            return {};
        }
        data.append(buffer.data(), got);
    }
    const qsizetype end = data.indexOf("\r\n\r\n") + 4;
    rest = data.mid(end);
    return data.left(end);
}

TcpServer::TcpServer(Handler handler, std::uint16_t port)
    : m_handler(std::move(handler))
{
    m_listen = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const int one = 1;
    ::setsockopt(m_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(m_listen, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::listen(m_listen, 64);
    socklen_t length = sizeof(address);
    ::getsockname(m_listen, reinterpret_cast<sockaddr*>(&address), &length);
    m_port = ntohs(address.sin_port);
    m_wake = ::eventfd(0, EFD_CLOEXEC);
    m_acceptor = std::jthread([this](const std::stop_token& stop) {
        std::set<int> open;
        std::array<pollfd, 2> fds{pollfd{m_listen, POLLIN, 0}, pollfd{m_wake, POLLIN, 0}};
        while (!stop.stop_requested() && ::poll(fds.data(), fds.size(), -1) >= 0) {
            if ((fds[1].revents & POLLIN) != 0) {
                break;
            }
            const int client = ::accept4(m_listen, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                continue;
            }
            timeval timeout{kIoTimeoutSeconds, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ++m_connections;
            open.insert(client);
            const std::scoped_lock lock(m_mutex);
            m_workers.emplace_back([this, client] {
                m_handler(client);
                ::shutdown(client, SHUT_RDWR);
            });
        }
        // Unblock handlers still waiting on keep-alive connections.
        for (const int fd : open) {
            ::shutdown(fd, SHUT_RDWR);
        }
        const std::scoped_lock lock(m_mutex);
        m_workers.clear();  // joins
        for (const int fd : open) {
            ::close(fd);
        }
    });
}

TcpServer::~TcpServer()
{
    m_acceptor.request_stop();
    const std::uint64_t one = 1;
    (void)::write(m_wake, &one, sizeof(one));
    m_acceptor.join();
    ::close(m_listen);
    ::close(m_wake);
}

TestCertificate::TestCertificate()
{
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("app.test"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509V3_CTX v3{};
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
    for (const auto& [nid, value] : std::array<std::pair<int, const char*>, 4>{{
             {NID_subject_alt_name, "DNS:app.test,DNS:localhost,IP:127.0.0.1"},
             {NID_basic_constraints, "critical,CA:TRUE"},
             {NID_key_usage, "critical,digitalSignature,keyCertSign"},
             {NID_subject_key_identifier, "hash"},
         }}) {
        X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &v3, nid, value);
        X509_add_ext(cert, extension, -1);
        X509_EXTENSION_free(extension);
    }
    X509_sign(cert, key, EVP_sha256());

    SSL_CTX* context = SSL_CTX_new(TLS_server_method());
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    SSL_CTX_use_certificate(context, cert);
    SSL_CTX_use_PrivateKey(context, key);
    m_context = context;

    m_pemPath = m_dir.filePath(QStringLiteral("test-ca.pem"));
    if (FILE* file = std::fopen(m_pemPath.toLocal8Bit().constData(), "w")) {
        PEM_write_X509(file, cert);
        std::fclose(file);
    }
    X509_free(cert);
    EVP_PKEY_free(key);
}

TestCertificate::~TestCertificate()
{
    SSL_CTX_free(static_cast<SSL_CTX*>(m_context));
}

HttpServer::HttpServer(Handler handler, const TestCertificate* tls)
    : m_handler(std::move(handler))
    , m_tls(tls)
    , m_server([this](int fd) {
        if (m_tls != nullptr) {
            TlsStream stream(fd, static_cast<SSL_CTX*>(m_tls->context()));
            if (stream.ok()) {
                serve(stream);
            }
        } else {
            FdStream stream(fd);
            serve(stream);
        }
    })
{
}

std::vector<HttpRequest> HttpServer::requests() const
{
    const std::scoped_lock lock(m_mutex);
    return m_requests;
}

void HttpServer::serve(Stream& stream)
{
    QByteArray rest;
    while (true) {
        const QByteArray head = stream.readHead(rest);
        if (head.isEmpty()) {
            return;
        }
        HttpRequest request;
        const QList<QByteArray> lines = head.trimmed().split('\n');
        const QList<QByteArray> requestLine = lines.front().trimmed().split(' ');
        request.method = requestLine.value(0);
        request.target = requestLine.value(1);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines.at(i).trimmed();
            const qsizetype colon = line.indexOf(':');
            if (colon > 0) {
                request.headers[line.left(colon).trimmed().toLower()] = line.mid(colon + 1).trimmed();
            }
        }
        const qsizetype length =
            request.headers.contains("content-length") ? request.headers["content-length"].toLongLong() : 0;
        std::array<char, 4096> buffer{};
        while (rest.size() < length) {
            const long got = stream.read(buffer.data(), buffer.size());
            if (got <= 0) {
                return;
            }
            rest.append(buffer.data(), got);
        }
        request.body = rest.left(length);
        rest.remove(0, length);
        {
            const std::scoped_lock lock(m_mutex);
            m_requests.push_back(request);
        }
        const HttpReply reply = m_handler(request);
        QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + ' ' + reason(reply.status) + "\r\n";
        out += "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\n";
        for (const auto& [name, value] : reply.headers) {
            out += name + ": " + value + "\r\n";
        }
        out += "\r\n";
        if (request.method != "HEAD") {
            out += reply.body;
        }
        if (!stream.write(out)) {
            return;
        }
    }
}

DnsServer::DnsServer()
{
    m_udp = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(m_udp, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(m_udp, reinterpret_cast<sockaddr*>(&address), &length);
    m_port = ntohs(address.sin_port);
    m_wake = ::eventfd(0, EFD_CLOEXEC);
    m_udpThread = std::jthread([this] {
        std::array<pollfd, 2> fds{pollfd{m_udp, POLLIN, 0}, pollfd{m_wake, POLLIN, 0}};
        std::array<char, 4096> buffer{};
        while (::poll(fds.data(), fds.size(), -1) >= 0 && (fds[1].revents & POLLIN) == 0) {
            sockaddr_storage peer{};
            socklen_t peerLength = sizeof(peer);
            const ssize_t got =
                ::recvfrom(m_udp, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (got <= 0) {
                continue;
            }
            ++m_udpQueries;
            const QByteArray reply = answer(QByteArray(buffer.data(), got), m_truncate.load());
            ::sendto(m_udp, reply.constData(), static_cast<std::size_t>(reply.size()), 0,
                     reinterpret_cast<sockaddr*>(&peer), peerLength);
        }
    });
    m_tcp = std::make_unique<TcpServer>(
        [this](int fd) {
            std::array<char, 2> prefix{};
            while (readExact(fd, prefix.data(), 2)) {
                const auto size = static_cast<std::size_t>((static_cast<std::uint8_t>(prefix[0]) << 8U) |
                                                           static_cast<std::uint8_t>(prefix[1]));
                QByteArray query(static_cast<qsizetype>(size), '\0');
                if (!readExact(fd, query.data(), size)) {
                    return;
                }
                ++m_tcpQueries;
                const QByteArray reply = answer(query, false);
                QByteArray framed;
                put16(framed, static_cast<std::uint16_t>(reply.size()));
                FdStream(fd).write(framed + reply);
            }
        },
        m_port);
}

DnsServer::~DnsServer()
{
    m_tcp.reset();
    const std::uint64_t one = 1;
    (void)::write(m_wake, &one, sizeof(one));
    m_udpThread.join();
    ::close(m_udp);
    ::close(m_wake);
}

void DnsServer::add(const QString& name, const QString& address, std::uint32_t ttl)
{
    const std::scoped_lock lock(m_mutex);
    m_records.emplace(name.toLower(), Record{address, ttl});
}

QByteArray DnsServer::answer(const QByteArray& query, bool truncate) const
{
    if (query.size() < 17) {
        return {};
    }
    // Question name (uncompressed in queries).
    qsizetype offset = 12;
    QByteArray name;
    while (offset < query.size() && query[offset] != 0) {
        const auto length = static_cast<std::uint8_t>(query[offset]);
        name += (name.isEmpty() ? "" : ".") + query.mid(offset + 1, length).toLower();
        offset += 1 + length;
    }
    const qsizetype questionEnd = offset + 5;
    const auto type = static_cast<std::uint16_t>((static_cast<std::uint8_t>(query[offset + 1]) << 8U) |
                                                 static_cast<std::uint8_t>(query[offset + 2]));

    std::vector<QByteArray> rdata;
    std::vector<std::uint32_t> ttls;
    bool known = false;
    {
        const std::scoped_lock lock(m_mutex);
        const auto [first, last] = m_records.equal_range(QString::fromLatin1(name));
        for (auto it = first; it != last; ++it) {
            known = true;
            const auto address = IpAddress::parse(it->second.address);
            const bool v4 = address->family() == AddressFamily::V4;
            if ((type == 1 && v4) || (type == 28 && !v4)) {
                const auto bytes = address->bytes();
                rdata.emplace_back(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
                ttls.push_back(it->second.ttl);
            }
        }
    }
    QByteArray out = query.left(2);  // id
    const std::uint16_t rcode = known ? 0 : 3;
    put16(out, static_cast<std::uint16_t>(0x8180U | (truncate ? 0x0200U : 0U) | rcode));
    put16(out, 1);
    put16(out, truncate ? 0 : static_cast<std::uint16_t>(rdata.size()));
    put16(out, !truncate && rdata.empty() ? 1 : 0);  // SOA for negative answers
    put16(out, 0);
    out += query.mid(12, questionEnd - 12);
    if (truncate) {
        return out;
    }
    for (std::size_t i = 0; i < rdata.size(); ++i) {
        put16(out, 0xc00c);
        put16(out, type);
        put16(out, 1);
        put32(out, ttls[i]);
        put16(out, static_cast<std::uint16_t>(rdata[i].size()));
        out += rdata[i];
    }
    if (rdata.empty()) {
        out.append('\0');  // root owner
        put16(out, 6);
        put16(out, 1);
        put32(out, 60);
        put16(out, 22);
        out.append('\0');   // MNAME
        out.append('\0');   // RNAME
        put32(out, 1);      // SERIAL
        put32(out, 3600);   // REFRESH
        put32(out, 600);    // RETRY
        put32(out, 86400);  // EXPIRE
        put32(out, 10);     // MINIMUM
    }
    return out;
}

DohServer::DohServer(const DnsServer& table)
    : m_http([&table](const HttpRequest& request) {
        if (request.method != "POST" || request.target != "/dns-query" ||
            request.headers.at("content-type") != "application/dns-message") {
            return HttpReply{.status = 404, .headers = {}, .body = {}};
        }
        return HttpReply{.status = 200,
                         .headers = {{"Content-Type", "application/dns-message"}},
                         .body = table.answer(request.body, false)};
    })
{
}

HttpProxy::HttpProxy(QByteArray requiredAuth)
    : m_auth(std::move(requiredAuth))
    , m_server([this](int fd) { serve(fd); })
{
}

std::vector<QByteArray> HttpProxy::requestLines() const
{
    const std::scoped_lock lock(m_mutex);
    return m_lines;
}

void HttpProxy::serve(int fd)
{
    FdStream stream(fd);
    QByteArray rest;
    while (true) {
        const QByteArray head = stream.readHead(rest);
        if (head.isEmpty()) {
            return;
        }
        const QByteArray line = head.left(head.indexOf("\r\n"));
        {
            const std::scoped_lock lock(m_mutex);
            m_lines.push_back(line);
        }
        if (!m_auth.isEmpty() && !head.contains(QByteArray("Proxy-Authorization: Basic " + m_auth.toBase64()))) {
            stream.write(QByteArray("HTTP/1.1 407 Proxy Authentication Required\r\n"
                                    "Proxy-Authenticate: Basic realm=\"test\"\r\nContent-Length: 0\r\n\r\n"));
            continue;
        }
        if (line.startsWith("CONNECT ")) {
            const QByteArray authority = line.split(' ').value(1);
            const auto port = static_cast<std::uint16_t>(authority.mid(authority.lastIndexOf(':') + 1).toUInt());
            stream.write(QByteArray("HTTP/1.1 200 Connection established\r\n\r\n"));
            tunnel(fd, port, rest);
            return;
        }
        stream.write(QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nvia-proxy"));
    }
}

SocksProxy::SocksProxy(QByteArray requiredAuth)
    : m_auth(std::move(requiredAuth))
    , m_server([this](int fd) { serve(fd); })
{
}

std::vector<SocksProxy::Connect> SocksProxy::connects() const
{
    const std::scoped_lock lock(m_mutex);
    return m_connects;
}

void SocksProxy::serve(int fd)
{
    FdStream stream(fd);
    std::array<char, 512> buffer{};
    if (!readExact(fd, buffer.data(), 1)) {
        return;
    }
    Connect connect;
    connect.version = buffer[0];
    if (connect.version == 4) {
        // VN CD DSTPORT DSTIP USERID\0 [HOST\0 for SOCKS4a]
        if (!readExact(fd, buffer.data(), 7)) {
            return;
        }
        connect.port = static_cast<std::uint16_t>((static_cast<std::uint8_t>(buffer[1]) << 8U) |
                                                  static_cast<std::uint8_t>(buffer[2]));
        const QByteArray ip(buffer.data() + 3, 4);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto readString = [&] {
            QByteArray text;
            char c = 0;
            while (readExact(fd, &c, 1) && c != 0) {
                text.append(c);
            }
            return text;
        };
        (void)readString();  // user id
        const bool socks4a = ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] != 0;
        connect.addressType = socks4a ? 3 : 1;
        connect.host = socks4a ? readString()
                               : QByteArray::number(static_cast<std::uint8_t>(ip[0])) + '.' +
                                     QByteArray::number(static_cast<std::uint8_t>(ip[1])) + '.' +
                                     QByteArray::number(static_cast<std::uint8_t>(ip[2])) + '.' +
                                     QByteArray::number(static_cast<std::uint8_t>(ip[3]));
        {
            const std::scoped_lock lock(m_mutex);
            m_connects.push_back(connect);
        }
        stream.write(QByteArray("\x00\x5a\x00\x00\x00\x00\x00\x00", 8));
        tunnel(fd, connect.port, {});
        return;
    }
    if (connect.version != 5 || !readExact(fd, buffer.data(), 1)) {
        return;
    }
    const auto methods = static_cast<std::uint8_t>(buffer[0]);
    if (!readExact(fd, buffer.data(), methods)) {
        return;
    }
    const bool offersPassword = std::memchr(buffer.data(), 2, methods) != nullptr;
    if (!m_auth.isEmpty()) {
        if (!offersPassword) {
            stream.write(QByteArray("\x05\xff", 2));
            return;
        }
        stream.write(QByteArray("\x05\x02", 2));
        // RFC 1929: VER ULEN UNAME PLEN PASSWD
        std::array<char, 2> header{};
        if (!readExact(fd, header.data(), 2) || !readExact(fd, buffer.data(), static_cast<std::uint8_t>(header[1]))) {
            return;
        }
        QByteArray credentials(buffer.data(), static_cast<std::uint8_t>(header[1]));
        char passwordLength = 0;
        if (!readExact(fd, &passwordLength, 1) ||
            !readExact(fd, buffer.data(), static_cast<std::uint8_t>(passwordLength))) {
            return;
        }
        credentials += ':' + QByteArray(buffer.data(), static_cast<std::uint8_t>(passwordLength));
        const bool accepted = credentials == m_auth;
        stream.write(QByteArray(accepted ? "\x01\x00" : "\x01\x01", 2));
        if (!accepted) {
            return;
        }
    } else {
        stream.write(QByteArray("\x05\x00", 2));
    }
    // VER CMD RSV ATYP DST.ADDR DST.PORT
    if (!readExact(fd, buffer.data(), 4)) {
        return;
    }
    connect.addressType = buffer[3];
    std::size_t length = 0;
    if (connect.addressType == 1) {
        length = 4;
    } else if (connect.addressType == 4) {
        length = 16;
    } else if (connect.addressType == 3) {
        char size = 0;
        if (!readExact(fd, &size, 1)) {
            return;
        }
        length = static_cast<std::uint8_t>(size);
    }
    if (!readExact(fd, buffer.data(), length + 2)) {
        return;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(buffer.data());
    if (connect.addressType == 3) {
        connect.host = QByteArray(buffer.data(), static_cast<qsizetype>(length));
    } else if (connect.addressType == 1) {
        connect.host = IpAddress::fromV4(std::span<const std::uint8_t, 4>(raw, 4)).toString().toLatin1();
    } else {
        connect.host = IpAddress::fromV6(std::span<const std::uint8_t, 16>(raw, 16)).toString().toLatin1();
    }
    connect.port = static_cast<std::uint16_t>((raw[length] << 8U) | raw[length + 1]);  // NOLINT
    {
        const std::scoped_lock lock(m_mutex);
        m_connects.push_back(connect);
    }
    stream.write(QByteArray("\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10));
    tunnel(fd, connect.port, {});
}

}  // namespace noctara::test

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-type-cstyle-cast)
