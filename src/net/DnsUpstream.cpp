#include "net/DnsUpstream.hpp"

#include "core/Log.hpp"
#include "core/UniqueFd.hpp"

#include <QRandomGenerator>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <sys/socket.h>

namespace noctara::net {

namespace {

struct SocketAddress {
    sockaddr_storage storage{};
    socklen_t length = 0;
};

SocketAddress toSocketAddress(const Endpoint& endpoint)
{
    SocketAddress out;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): sockaddr_storage is designed to be reinterpreted
    if (endpoint.address.family() == AddressFamily::V4) {
        auto* in = reinterpret_cast<sockaddr_in*>(&out.storage);
        in->sin_family = AF_INET;
        in->sin_port = htons(endpoint.port);
        std::memcpy(&in->sin_addr, endpoint.address.bytes().data(), 4);
        out.length = sizeof(sockaddr_in);
    } else {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&out.storage);
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(endpoint.port);
        std::memcpy(&in6->sin6_addr, endpoint.address.bytes().data(), 16);
        out.length = sizeof(sockaddr_in6);
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    return out;
}

std::uint16_t messageId(QByteArrayView message)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(static_cast<std::uint8_t>(message[0])) << 8U) |
                                      static_cast<std::uint8_t>(message[1]));
}

// One request/response with one server over UDP or TCP. Deletes itself after reporting exactly once.
class Exchange : public QObject {
public:
    using Done = std::function<void(Result<QByteArray>)>;

    Exchange(const Endpoint& server, bool tcp, QByteArray query, std::chrono::milliseconds timeout, QObject* parent,
             Done done)
        : QObject(parent)
        , m_tcp(tcp)
        , m_query(std::move(query))
        , m_id(messageId(m_query))
        , m_done(std::move(done))
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { finish(fail(ErrorCode::Timeout, "no answer")); });
        m_timer.start(timeout);
        // Start from the event loop so a synchronous failure is still reported asynchronously.
        QMetaObject::invokeMethod(this, [this, server] { start(server); }, Qt::QueuedConnection);
    }

private:
    void start(const Endpoint& server)
    {
        const SocketAddress address = toSocketAddress(server);
        const int family = server.address.family() == AddressFamily::V4 ? AF_INET : AF_INET6;
        const auto type = static_cast<unsigned>(m_tcp ? SOCK_STREAM : SOCK_DGRAM) |
                          static_cast<unsigned>(SOCK_NONBLOCK) | static_cast<unsigned>(SOCK_CLOEXEC);
        m_fd.reset(::socket(family, static_cast<int>(type), 0));
        if (!m_fd) {
            finish(socketError("socket"));
            return;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): POSIX socket API
        const int rc = ::connect(m_fd.get(), reinterpret_cast<const sockaddr*>(&address.storage), address.length);
        if (rc != 0 && errno != EINPROGRESS) {
            finish(socketError("connect"));
            return;
        }
        m_read = std::make_unique<QSocketNotifier>(m_fd.get(), QSocketNotifier::Read);
        connect(m_read.get(), &QSocketNotifier::activated, this, [this] { onReadable(); });
        if (m_tcp) {
            QByteArray framed;
            const auto size = static_cast<std::uint16_t>(m_query.size());
            framed.append(static_cast<char>(size >> 8U));
            framed.append(static_cast<char>(size & 0xffU));
            m_pending = framed + m_query;
            m_write = std::make_unique<QSocketNotifier>(m_fd.get(), QSocketNotifier::Write);
            connect(m_write.get(), &QSocketNotifier::activated, this, [this] { onWritable(); });
        } else if (::send(m_fd.get(), m_query.constData(), static_cast<std::size_t>(m_query.size()), 0) < 0) {
            finish(socketError("send"));
        }
    }

    void onWritable()
    {
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(m_fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            errno = error;
            finish(socketError("connect"));
            return;
        }
        const ssize_t sent =
            ::send(m_fd.get(), m_pending.constData(), static_cast<std::size_t>(m_pending.size()), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno != EAGAIN) {
                finish(socketError("send"));
            }
            return;
        }
        m_pending.remove(0, sent);
        if (m_pending.isEmpty()) {
            m_write->setEnabled(false);
        }
    }

    void onReadable()
    {
        std::array<char, 4096> buffer{};
        const ssize_t received = ::recv(m_fd.get(), buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno != EAGAIN) {
                finish(socketError("recv"));
            }
            return;
        }
        if (!m_tcp) {
            // Ignore anything that is not a response to this query (the socket is connected, so it came from
            // the server's address; the random id guards against blind spoofing).
            const QByteArrayView message(buffer.data(), received);
            if (message.size() >= 12 && messageId(message) == m_id &&
                (static_cast<std::uint8_t>(message[2]) & 0x80U) != 0) {
                finish(message.toByteArray());
            }
            return;
        }
        if (received == 0) {
            finish(fail(ErrorCode::ConnectionFailed, "TCP connection closed early"));
            return;
        }
        m_received.append(buffer.data(), received);
        if (m_received.size() >= 2) {
            const auto length = messageId(m_received);  // same big-endian 16-bit layout
            if (m_received.size() >= 2 + static_cast<qsizetype>(length)) {
                finish(m_received.mid(2, length));
            }
        }
    }

    [[nodiscard]] static std::unexpected<Error> socketError(const char* call)
    {
        const int err = errno;
        return fail(err == ECONNREFUSED ? ErrorCode::ConnectionRefused : ErrorCode::ConnectionFailed,
                    std::format("{}: {}", call, std::strerror(err)));  // NOLINT(concurrency-mt-unsafe)
    }

    void finish(Result<QByteArray> result)
    {
        if (!m_done) {
            return;
        }
        // Only disable: this may run inside a notifier's own signal, and `done` may release the upstream
        // that parents this exchange. Everything is torn down later through deleteLater.
        m_timer.stop();
        for (QSocketNotifier* notifier : {m_read.get(), m_write.get()}) {
            if (notifier != nullptr) {
                notifier->setEnabled(false);
            }
        }
        const Done done = std::exchange(m_done, nullptr);
        deleteLater();
        done(std::move(result));
    }

    bool m_tcp;
    QByteArray m_query;
    std::uint16_t m_id;
    Done m_done;
    UniqueFd m_fd;
    std::unique_ptr<QSocketNotifier> m_read;
    std::unique_ptr<QSocketNotifier> m_write;
    QTimer m_timer;
    QByteArray m_pending;
    QByteArray m_received;
};

std::uint16_t randomId()
{
    return static_cast<std::uint16_t>(QRandomGenerator::system()->bounded(65536U));
}

}  // namespace

ServerUpstream::ServerUpstream(std::vector<Endpoint> servers, std::chrono::milliseconds timeout, QObject* parent)
    : QObject(parent)
    , m_servers(std::move(servers))
    , m_timeout(timeout)
{
}

void ServerUpstream::query(const QByteArray& name, dns::RecordType type, UpstreamCallback callback)
{
    if (m_servers.empty()) {
        QMetaObject::invokeMethod(
            this, [callback] { callback(fail(ErrorCode::DnsServerUnreachable, "no DNS servers configured")); },
            Qt::QueuedConnection);
        return;
    }
    attempt(0, false, name, type, callback);
}

void ServerUpstream::attempt(std::size_t serverIndex, bool tcp, const QByteArray& name, dns::RecordType type,
                             const UpstreamCallback& callback)
{
    if (serverIndex >= m_servers.size()) {
        callback(fail(ErrorCode::DnsServerUnreachable, "no DNS server answered"));
        return;
    }
    const Endpoint server = m_servers[serverIndex];
    auto query = dns::encodeQuery(name, type, randomId(), false);
    if (!query) {
        QMetaObject::invokeMethod(
            this, [callback, error = query.error()] { callback(std::unexpected(error)); }, Qt::QueuedConnection);
        return;
    }
    auto onDone = [this, serverIndex, tcp, name, type, server, callback](Result<QByteArray> raw) {
        const QString serverName = server.toString(53);
        if (raw) {
            auto response = dns::decodeResponse(*raw, name, type);
            if (response && response->truncated && !tcp) {
                attempt(serverIndex, true, name, type, callback);
                return;
            }
            const bool serverFailure =
                response && (response->rcode == dns::Rcode::ServFail || response->rcode == dns::Rcode::Refused);
            if (response && !serverFailure && !response->truncated) {
                callback(UpstreamAnswer{std::move(*response), serverName});
                return;
            }
            qCInfo(lcNet).noquote() << "DNS server" << serverName << "gave no usable answer"
                                    << (response ? QString() : QString::fromStdString(response.error().describe()));
        } else {
            qCInfo(lcNet).noquote() << "DNS server" << serverName
                                    << "failed:" << QString::fromStdString(raw.error().describe());
        }
        attempt(serverIndex + 1, false, name, type, callback);
    };
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): parented to this, deletes itself when done
    new Exchange(server, tcp, std::move(*query), m_timeout, this, std::move(onDone));
}

bool dohUrlAllowed(const QUrl& url, const std::vector<IpAddress>& bootstrap, bool allowInsecure)
{
    if (url.scheme() == u"https") {
        return true;
    }
    if (url.scheme() != u"http" || !allowInsecure) {
        return false;
    }
    if (const auto literal = IpAddress::parse(url.host())) {
        return literal->isLocal();
    }
    if (url.host().compare(u"localhost", Qt::CaseInsensitive) == 0) {
        return true;
    }
    return !bootstrap.empty() && std::ranges::all_of(bootstrap, &IpAddress::isLocal);
}

DohUpstream::DohUpstream(QUrl url, std::vector<IpAddress> addresses, DohSender sender, QObject* parent)
    : QObject(parent)
    , m_url(std::move(url))
    , m_addresses(std::move(addresses))
    , m_sender(std::move(sender))
{
}

void DohUpstream::query(const QByteArray& name, dns::RecordType type, UpstreamCallback callback)
{
    // RFC 8484 §4.1: the id should be 0 so identical queries are cacheable; padding hides the name's length.
    auto query = dns::encodeQuery(name, type, 0, true);
    if (!query) {
        QMetaObject::invokeMethod(
            this, [callback, error = query.error()] { callback(std::unexpected(error)); }, Qt::QueuedConnection);
        return;
    }
    m_sender(
        m_url, m_addresses, std::move(*query), [name, type, server = m_url.host(), callback](Result<QByteArray> raw) {
            if (!raw) {
                callback(std::unexpected(raw.error()));
                return;
            }
            auto response = dns::decodeResponse(*raw, name, type);
            if (!response || response->truncated) {
                callback(fail(ErrorCode::DohInvalidResponse, response ? "truncated" : response.error().describe()));
                return;
            }
            callback(UpstreamAnswer{std::move(*response), server});
        });
}

}  // namespace noctara::net
