#include "drm/SocksRelay.hpp"

#include "core/Handoff.hpp"
#include "core/Log.hpp"
#include "core/NetAddress.hpp"

#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace noctara::drm {

namespace {

using namespace std::chrono_literals;

constexpr qsizetype kMaxRequestHead = qsizetype{16} * 1024;
constexpr qint64 kChunk = qint64{64} * 1024;
constexpr qint64 kWriteHighWater = qint64{256} * 1024;  // past this, the sending side waits for the slower peer
constexpr auto kHandshakeTimeout = 30s;
constexpr int kMaxSocksField = 255;  // RFC 1929 username and password, SOCKS5 domain name

constexpr char kSocksVersion = 5;
constexpr char kNoAuth = 0;
constexpr char kPasswordAuth = 2;
constexpr char kLoginVersion = 1;
constexpr char kConnectCommand = 1;
constexpr char kAddressV4 = 1;
constexpr char kAddressName = 3;
constexpr char kAddressV6 = 4;

constexpr int kForbiddenStatus = 403;
constexpr int kAuthRequiredStatus = 407;
constexpr int kMethodNotAllowedStatus = 405;
constexpr int kBadRequestStatus = 400;
constexpr int kBadGatewayStatus = 502;
constexpr int kGatewayTimeoutStatus = 504;

struct Config {
    net::ProxyServer upstream;
    QByteArray expectedAuthorization;  // the whole "Basic …" value Chromium must send
};

QString randomToken()
{
    std::array<std::uint32_t, 4> words{};
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    const QByteArray bytes(
        reinterpret_cast<const char*>(words.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        static_cast<qsizetype>(sizeof(words)));
    return QString::fromLatin1(bytes.toHex());
}

bool sameSecret(QByteArrayView a, QByteArrayView b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    unsigned difference = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned>(static_cast<unsigned char>(a[i])) ^
                      static_cast<unsigned>(static_cast<unsigned char>(b[i]));
    }
    return difference == 0;
}

// "host:port" or "[v6]:port" from a CONNECT request line.
bool parseAuthority(QByteArrayView authority, QString& host, std::uint16_t& port)
{
    const qsizetype colon = authority.lastIndexOf(':');
    if (colon <= 0 || colon + 1 >= authority.size()) {
        return false;
    }
    bool isNumber = false;
    const uint number = QString::fromLatin1(authority.sliced(colon + 1)).toUInt(&isNumber);
    if (!isNumber || number == 0 || number > 65535U) {
        return false;
    }
    QByteArrayView name = authority.first(colon);
    if (name.startsWith('[') && name.endsWith(']')) {
        name = name.sliced(1, name.size() - 2);
    }
    host = QString::fromLatin1(name);
    port = static_cast<std::uint16_t>(number);
    return !host.isEmpty();
}

// ATYP, address and port of a SOCKS5 CONNECT; empty when the name does not fit.
QByteArray socksDestination(const QString& host, std::uint16_t port)
{
    QByteArray out;
    if (const auto literal = IpAddress::parse(host)) {
        out += literal->family() == AddressFamily::V4 ? kAddressV4 : kAddressV6;
        const auto bytes = literal->bytes();
        out.append(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                   static_cast<qsizetype>(bytes.size()));
    } else {
        const QByteArray name = host.toLatin1();
        if (name.size() > kMaxSocksField) {
            return {};
        }
        out += kAddressName;
        out += static_cast<char>(name.size());
        out += name;
    }
    out += static_cast<char>(port >> 8U);
    out += static_cast<char>(port & 0xFFU);
    return out;
}

// One CONNECT from the helper: reads the request, signs in to the real proxy, opens the tunnel, then copies bytes
// both ways. Lives on the relay's thread and deletes itself when both sides are closed.
class Tunnel final : public QObject {
public:
    Tunnel(QTcpSocket* client, std::shared_ptr<const Config> config, SocksRelay::Resolve resolve, QObject* parent)
        : QObject(parent)
        , m_client(client)
        , m_config(std::move(config))
        , m_resolve(std::move(resolve))
    {
        m_client->setParent(this);
        m_client->setReadBufferSize(kChunk);
        m_upstream.setReadBufferSize(kChunk);

        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { fail(kGatewayTimeoutStatus, "timed out"); });
        m_timer.start(kHandshakeTimeout);

        connect(m_client, &QTcpSocket::readyRead, this, [this] {
            if (m_stage == Stage::Request) {
                readRequest();
            } else if (m_stage == Stage::Piping) {
                pump(*m_client, m_upstream);
            }
        });
        connect(m_client, &QTcpSocket::bytesWritten, this, [this] {
            if (m_stage == Stage::Piping) {
                pump(m_upstream, *m_client);
            }
        });
        connect(m_client, &QTcpSocket::disconnected, this, [this] { clientClosed(); });

        connect(&m_upstream, &QTcpSocket::connected, this, [this] { sendGreeting(); });
        connect(&m_upstream, &QTcpSocket::readyRead, this, [this] {
            if (m_stage == Stage::Piping) {
                pump(m_upstream, *m_client);
            } else {
                readSocksReply();
            }
        });
        connect(&m_upstream, &QTcpSocket::bytesWritten, this, [this] {
            if (m_stage == Stage::Piping) {
                pump(*m_client, m_upstream);
            }
        });
        connect(&m_upstream, &QTcpSocket::disconnected, this, [this] { upstreamClosed(); });
        connect(&m_upstream, &QTcpSocket::errorOccurred, this, [this] {
            if (m_stage == Stage::Connecting) {
                // Not from inside this signal: the socket may not be ready for another attempt yet.
                QTimer::singleShot(0, this, [this] { connectNextAddress(); });
            }
        });

        if (m_client->bytesAvailable() > 0) {
            readRequest();
        }
    }

private:
    enum class Stage : std::uint8_t { Request, Resolving, Connecting, Greeting, Login, Opening, Piping, Closing };

    void readRequest()
    {
        m_request += m_client->readAll();
        const qsizetype end = m_request.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_request.size() > kMaxRequestHead) {
                fail(kBadRequestStatus, "request too long");
            }
            return;
        }
        const QList<QByteArray> lines = m_request.first(end).split('\n');
        const QList<QByteArray> requestLine = lines.front().trimmed().split(' ');
        if (requestLine.size() != 3 || requestLine[0] != "CONNECT") {
            fail(kMethodNotAllowedStatus, "not a CONNECT request");
            return;
        }
        QByteArray authorization;
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0 && lines[i].first(colon).trimmed().compare("proxy-authorization", Qt::CaseInsensitive) == 0) {
                authorization = lines[i].sliced(colon + 1).trimmed();
            }
        }
        if (!sameSecret(authorization, m_config->expectedAuthorization)) {
            // The normal first answer: Chromium then asks for the credentials and sends the request again.
            reply(kAuthRequiredStatus, "Proxy Authentication Required",
                  "Proxy-Authenticate: Basic realm=\"Noctara\"\r\n");
            return;
        }
        QString host;
        std::uint16_t port = 0;
        if (!parseAuthority(requestLine[1], host, port)) {
            fail(kBadRequestStatus, "invalid destination");
            return;
        }
        m_destination = socksDestination(host, port);
        if (m_destination.isEmpty()) {
            fail(kBadRequestStatus, "destination name too long");
            return;
        }
        m_clientExtra = m_request.sliced(end + 4);
        m_request.clear();
        qCDebug(lcDrm).noquote() << "proxy relay: tunnel to" << host << port;

        const QString& proxyHost = m_config->upstream.host;
        if (const auto literal = IpAddress::parse(proxyHost)) {
            m_addresses = {*literal};
            startConnecting();
            return;
        }
        m_stage = Stage::Resolving;
        m_resolve(proxyHost, this, [this](Result<net::Resolution> result) {
            if (!result || result->addresses.empty()) {
                fail(kBadGatewayStatus, "could not look up the proxy server");
                return;
            }
            m_addresses = std::move(result->addresses);
            startConnecting();
        });
    }

    void startConnecting()
    {
        m_stage = Stage::Connecting;
        connectNextAddress();
    }

    void connectNextAddress()
    {
        if (m_stage != Stage::Connecting) {
            return;
        }
        if (m_nextAddress >= m_addresses.size()) {
            fail(kBadGatewayStatus, "could not connect to the proxy server");
            return;
        }
        m_upstream.abort();
        m_upstream.connectToHost(QHostAddress(m_addresses[m_nextAddress++].toString()), m_config->upstream.port);
    }

    void sendGreeting()
    {
        m_stage = Stage::Greeting;
        QByteArray greeting;
        greeting += kSocksVersion;
        greeting += char{2};  // two methods offered
        greeting += kNoAuth;
        greeting += kPasswordAuth;
        m_upstream.write(greeting);
    }

    void sendLogin()
    {
        const QByteArray username = m_config->upstream.username.toUtf8();
        const QByteArray password = m_config->upstream.password.toUtf8();
        if (username.size() > kMaxSocksField || password.size() > kMaxSocksField) {
            fail(kForbiddenStatus, "the proxy username or password is too long for SOCKS5");
            return;
        }
        QByteArray login;
        login += kLoginVersion;
        login += static_cast<char>(username.size());
        login += username;
        login += static_cast<char>(password.size());
        login += password;
        m_stage = Stage::Login;
        m_upstream.write(login);
    }

    void sendConnect()
    {
        m_stage = Stage::Opening;
        QByteArray request;
        request += kSocksVersion;
        request += kConnectCommand;
        request += char{0};  // reserved
        request += m_destination;
        m_upstream.write(request);
    }

    void readSocksReply()
    {
        m_reply += m_upstream.readAll();
        if (m_stage == Stage::Greeting && m_reply.size() >= 2) {
            const char method = m_reply[1];
            m_reply.remove(0, 2);
            if (method == kPasswordAuth) {
                sendLogin();
            } else if (method == kNoAuth) {
                sendConnect();
            } else {
                fail(kForbiddenStatus, "the proxy accepts no sign-in method Noctara offers");
            }
        } else if (m_stage == Stage::Login && m_reply.size() >= 2) {
            const char status = m_reply[1];
            m_reply.remove(0, 2);
            if (status != 0) {
                fail(kForbiddenStatus, "the proxy refused the username or password");
                return;
            }
            sendConnect();
        }
        if (m_stage != Stage::Opening || m_reply.size() < 5) {  // one byte past the fixed part, for a name's length
            return;
        }
        if (m_reply[1] != 0) {
            fail(kBadGatewayStatus, "the proxy could not reach the destination");
            return;
        }
        const char addressType = m_reply[3];
        const qsizetype addressSize = addressType == kAddressV4     ? 4
                                      : addressType == kAddressV6   ? 16
                                      : addressType == kAddressName ? 1 + static_cast<unsigned char>(m_reply[4])
                                                                    : -1;
        if (addressSize < 0) {
            fail(kBadGatewayStatus, "invalid answer from the proxy");
            return;
        }
        const qsizetype total = 4 + addressSize + 2;
        if (m_reply.size() < total) {
            return;
        }
        const QByteArray fromUpstream = m_reply.sliced(total);
        m_reply.clear();
        m_timer.stop();
        m_stage = Stage::Piping;
        m_client->write("HTTP/1.1 200 Connection established\r\n\r\n");
        if (!fromUpstream.isEmpty()) {
            m_client->write(fromUpstream);
        }
        if (!m_clientExtra.isEmpty()) {
            m_upstream.write(m_clientExtra);
            m_clientExtra.clear();
        }
        pump(*m_client, m_upstream);
        pump(m_upstream, *m_client);
    }

    static void pump(QTcpSocket& from, QTcpSocket& to)
    {
        while (from.bytesAvailable() > 0 && to.bytesToWrite() < kWriteHighWater) {
            to.write(from.read(kChunk));
        }
    }

    static void drain(QTcpSocket& from, QTcpSocket& to)
    {
        while (from.bytesAvailable() > 0) {
            to.write(from.read(kChunk));
        }
    }

    void clientClosed()
    {
        if (m_stage == Stage::Piping) {
            drain(*m_client, m_upstream);
            m_upstream.disconnectFromHost();
        } else {
            m_stage = Stage::Closing;
            m_timer.stop();
            m_upstream.abort();
        }
        finishIfDone();
    }

    void upstreamClosed()
    {
        if (m_stage == Stage::Piping) {
            drain(m_upstream, *m_client);
            m_client->disconnectFromHost();
        } else if (m_stage != Stage::Closing) {
            fail(kBadGatewayStatus, "the proxy closed the connection");
            return;
        }
        finishIfDone();
    }

    void finishIfDone()
    {
        if (m_client->state() == QAbstractSocket::UnconnectedState &&
            m_upstream.state() == QAbstractSocket::UnconnectedState) {
            deleteLater();
        }
    }

    void fail(int status, const char* reason)
    {
        if (m_stage == Stage::Closing) {
            return;
        }
        qCWarning(lcDrm).noquote() << "proxy relay:" << reason;
        const char* text = status == kGatewayTimeoutStatus ? "Gateway Timeout"
                           : status == kForbiddenStatus    ? "Forbidden"
                           : status == kBadRequestStatus   ? "Bad Request"
                           : status == kBadGatewayStatus   ? "Bad Gateway"
                                                           : "Method Not Allowed";
        reply(status, text, "");
    }

    void reply(int status, const char* text, const char* extraHeaders)
    {
        m_stage = Stage::Closing;
        m_timer.stop();
        m_upstream.abort();
        m_client->write(QByteArrayLiteral("HTTP/1.1 ") + QByteArray::number(status) + ' ' + text + "\r\n" +
                        extraHeaders + "Content-Length: 0\r\nConnection: close\r\n\r\n");
        m_client->disconnectFromHost();
        finishIfDone();
    }

    QTcpSocket* m_client;  // child of this
    QTcpSocket m_upstream;
    QTimer m_timer;
    std::shared_ptr<const Config> m_config;
    SocksRelay::Resolve m_resolve;
    Stage m_stage = Stage::Request;
    QByteArray m_request;
    QByteArray m_clientExtra;  // sent by the client right behind its request
    QByteArray m_reply;
    QByteArray m_destination;
    std::vector<IpAddress> m_addresses;
    std::size_t m_nextAddress = 0;
};

}  // namespace

class SocksRelay::Worker final : public QObject {
public:
    explicit Worker(Resolve resolve)
        : m_resolve(std::move(resolve))
        , m_tunnels(std::make_unique<QObject>())
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                new Tunnel(socket, m_config, m_resolve, m_tunnels.get());  // NOLINT(cppcoreguidelines-owning-memory)
            }
        });
    }

    [[nodiscard]] std::uint16_t listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0) ? m_server.serverPort() : std::uint16_t{0};
    }

    void configure(std::shared_ptr<const Config> config)
    {
        m_config = std::move(config);
        m_tunnels = std::make_unique<QObject>();  // tunnels through the previous proxy end here
    }

private:
    Resolve m_resolve;
    QTcpServer m_server;
    std::unique_ptr<QObject> m_tunnels;  // parent of every open Tunnel
    std::shared_ptr<const Config> m_config;
};

SocksRelay::SocksRelay(Resolve resolve)
    : m_resolve(std::move(resolve))
{
    m_thread.setObjectName(QStringLiteral("noctara-socks-relay"));
}

SocksRelay::~SocksRelay()
{
    stop();
}

std::optional<LocalProxy> SocksRelay::start(const net::ProxyServer& upstream)
{
    if (m_worker == nullptr) {
        m_anchor = new QObject;  // NOLINT(cppcoreguidelines-owning-memory): deleted in stop()
        m_anchor->moveToThread(&m_thread);
        m_thread.start();
        std::uint16_t port = 0;
        handoff::runAndWait(m_anchor, [this, &port] {
            m_worker = new Worker(m_resolve);  // NOLINT(cppcoreguidelines-owning-memory): deleted in stop()
            port = m_worker->listen();
        });
        if (port == 0) {
            qCWarning(lcDrm) << "proxy relay: could not open a local port";
            stop();
            return std::nullopt;
        }
        m_endpoint = LocalProxy{.port = port, .username = randomToken(), .password = randomToken()};
    }
    if (!m_endpoint) {
        return std::nullopt;  // set together with the worker
    }
    auto config = std::make_shared<Config>();
    config->upstream = upstream;
    config->expectedAuthorization =
        "Basic " + (m_endpoint->username + u':' + m_endpoint->password).toLatin1().toBase64();
    handoff::post(m_anchor, [worker = m_worker, config = std::shared_ptr<const Config>(std::move(config))] {
        worker->configure(config);
    });
    return m_endpoint;
}

void SocksRelay::stop()
{
    if (m_anchor == nullptr) {
        return;
    }
    handoff::runAndWait(m_anchor, [this] {
        delete m_worker;  // NOLINT(cppcoreguidelines-owning-memory)
        m_worker = nullptr;
    });
    m_thread.quit();
    m_thread.wait();
    delete m_anchor;  // NOLINT(cppcoreguidelines-owning-memory): its thread has ended
    m_anchor = nullptr;
    m_endpoint.reset();
}

}  // namespace noctara::drm
