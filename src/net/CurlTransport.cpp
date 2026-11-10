#include "net/CurlTransport.hpp"

#include "core/Handoff.hpp"
#include "core/Log.hpp"
#include "net/CallbackChannel.hpp"
#include "net/CurlSupport.hpp"
#include "net/Redaction.hpp"
#include "net/RetryPolicy.hpp"

#include <QElapsedTimer>
#include <QPointer>
#include <QRandomGenerator>
#include <QSocketNotifier>

#include <array>
#include <format>

namespace noctara::net {

namespace {

constexpr std::chrono::milliseconds kDohTimeout{5000};

bool isRedirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

QByteArray httpVersionName(long version)
{
    switch (version) {
    case CURL_HTTP_VERSION_1_0: return QByteArrayLiteral("HTTP/1.0");
    case CURL_HTTP_VERSION_1_1: return QByteArrayLiteral("HTTP/1.1");
    case CURL_HTTP_VERSION_2_0: return QByteArrayLiteral("HTTP/2");
    case CURL_HTTP_VERSION_3: return QByteArrayLiteral("HTTP/3");
    default: return {};
    }
}

long curlHttpVersion(HttpVersion version)
{
    switch (version) {
    case HttpVersion::Auto: return CURL_HTTP_VERSION_2TLS;
    case HttpVersion::Http1_1: return CURL_HTTP_VERSION_1_1;
    case HttpVersion::Http2: return CURL_HTTP_VERSION_2TLS;
    case HttpVersion::Http3: return CURL_HTTP_VERSION_3;  // falls back to HTTP/2 or 1.1 when QUIC fails
    }
    return CURL_HTTP_VERSION_2TLS;
}

int defaultPort(const QUrl& url)
{
    return url.port(url.scheme() == u"https" ? 443 : 80);
}

QByteArray resolveEntry(const QString& host, int port, const std::vector<IpAddress>& addresses)
{
    // An IPv6 literal host needs its brackets: libcurl splits an entry at the first ':' outside them.
    const auto literal = IpAddress::parse(host);
    const QString name = literal && literal->family() == AddressFamily::V6 ? u'[' + host + u']' : host;
    QByteArray entry = name.toLatin1() + ':' + QByteArray::number(port) + ':';
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        entry += (i == 0 ? "" : ",") + addresses[i].toUrlHost().toLatin1();
    }
    return entry;
}

double millis(CURL* easy, CURLINFO info)
{
    curl_off_t micros = 0;
    (void)curl_easy_getinfo(easy, info, &micros);
    return static_cast<double>(micros) / 1000.0;
}

template <class T> void setopt(CURL* easy, CURLoption option, T value)
{
    if (const CURLcode rc = curl_easy_setopt(easy, option, value); rc != CURLE_OK) {
        qCWarning(lcNet) << "curl_easy_setopt" << static_cast<int>(option) << "failed:" << curl_easy_strerror(rc);
    }
}

}  // namespace

// One request through all its attempts and redirects.
struct Transfer {
    CurlTransport* owner = nullptr;
    std::uint64_t id = 0;
    Request request;
    Callbacks callbacks;
    std::shared_ptr<RequestHandle::State> state;
    ChannelPtr channel;                                  // null for internal transfers
    std::function<void(Result<Response>)> internalDone;  // DoH
    std::vector<IpAddress> pinned;                       // DoH: addresses of the server

    // Current attempt.
    CurlEasy easy;
    CurlSlist headerList;
    CurlSlist resolveList;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    CurlSession* session = nullptr;
    ProxyRoute route;
    ResolverSource dnsSource = ResolverSource::Literal;
    QElapsedTimer clock;
    double dnsMs = 0;
    Response response;
    bool finalHeaders = false;
    bool streaming = false;
    bool deliveredData = false;
    bool paused = false;
    int retries = 0;
    int redirects = 0;
    std::optional<ErrorCode> abortReason;  // set when the transport itself aborts (offline mode)

    [[nodiscard]] bool internal() const noexcept { return channel == nullptr; }
    [[nodiscard]] bool cancelled() const noexcept { return state->cancelled.load(std::memory_order_relaxed); }

    void post(std::function<void()> closure) const
    {
        if (channel) {
            channel->post(std::move(closure));
        }
    }
};

// One curl multi handle with its connection pool. Replaced when the configuration changes; the old one
// drains (finishes its transfers, then closes its connections).
class CurlSession {
public:
    CurlSession(CurlTransport& owner, const TransportConfig& config)
        : m_owner(owner)
        , m_multi(curl_multi_init())
    {
        m_timer.setSingleShot(true);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] { action(CURL_SOCKET_TIMEOUT, 0); });
        (void)curl_multi_setopt(m_multi, CURLMOPT_SOCKETFUNCTION, &CurlSession::onSocket);
        (void)curl_multi_setopt(m_multi, CURLMOPT_SOCKETDATA, this);
        (void)curl_multi_setopt(m_multi, CURLMOPT_TIMERFUNCTION, &CurlSession::onTimer);
        (void)curl_multi_setopt(m_multi, CURLMOPT_TIMERDATA, this);
        (void)curl_multi_setopt(m_multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, static_cast<long>(config.maxConnections));
        (void)curl_multi_setopt(m_multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                                static_cast<long>(config.maxConnectionsPerHost));
        (void)curl_multi_setopt(m_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    }

    ~CurlSession()
    {
        // Cleanup first: it closes the pooled connections and reports their sockets through onSocket, which
        // must still find live notifiers (it disables them and deletes them later). Only what curl did not
        // report is deleted here.
        (void)curl_multi_cleanup(m_multi);
        for (auto& [fd, watch] : m_watches) {
            delete watch.read;   // NOLINT(cppcoreguidelines-owning-memory): owned by the watch map
            delete watch.write;  // NOLINT(cppcoreguidelines-owning-memory)
        }
    }

    CurlSession(const CurlSession&) = delete;
    CurlSession& operator=(const CurlSession&) = delete;
    CurlSession(CurlSession&&) = delete;
    CurlSession& operator=(CurlSession&&) = delete;

    bool add(Transfer& transfer)
    {
        if (curl_multi_add_handle(m_multi, transfer.easy.get()) != CURLM_OK) {
            return false;
        }
        transfer.session = this;
        ++m_transfers;
        return true;
    }

    void remove(Transfer& transfer)
    {
        (void)curl_multi_remove_handle(m_multi, transfer.easy.get());
        transfer.session = nullptr;
        --m_transfers;
    }

    [[nodiscard]] int transfers() const noexcept { return m_transfers; }

private:
    struct Watch {
        QSocketNotifier* read = nullptr;
        QSocketNotifier* write = nullptr;
    };

    static int onSocket(CURL* /*easy*/, curl_socket_t fd, int what, void* user, void* /*socketData*/)
    {
        static_cast<CurlSession*>(user)->watch(fd, what);
        return 0;
    }

    static int onTimer(CURLM* /*multi*/, long timeoutMs, void* user)
    {
        auto* self = static_cast<CurlSession*>(user);
        if (timeoutMs < 0) {
            self->m_timer.stop();
        } else {
            self->m_timer.start(static_cast<int>(timeoutMs));
        }
        return 0;
    }

    void watch(curl_socket_t fd, int what)
    {
        Watch& watch = m_watches[fd];
        if (what == CURL_POLL_REMOVE) {
            // May run inside this socket's own notifier signal: disable now, delete later.
            for (QSocketNotifier* notifier : {watch.read, watch.write}) {
                if (notifier != nullptr) {
                    notifier->setEnabled(false);
                    notifier->deleteLater();
                }
            }
            m_watches.erase(fd);
            return;
        }
        const auto ensure = [&](QSocketNotifier*& notifier, QSocketNotifier::Type type, int event) {
            if (notifier == nullptr) {
                notifier = new QSocketNotifier(fd, type);  // NOLINT(cppcoreguidelines-owning-memory)
                QObject::connect(notifier, &QSocketNotifier::activated, notifier,
                                 [this, fd, event] { action(fd, event); });
            }
            return notifier;
        };
        const bool wantRead = what == CURL_POLL_IN || what == CURL_POLL_INOUT;
        const bool wantWrite = what == CURL_POLL_OUT || what == CURL_POLL_INOUT;
        if (wantRead || watch.read != nullptr) {
            ensure(watch.read, QSocketNotifier::Read, CURL_CSELECT_IN)->setEnabled(wantRead);
        }
        if (wantWrite || watch.write != nullptr) {
            ensure(watch.write, QSocketNotifier::Write, CURL_CSELECT_OUT)->setEnabled(wantWrite);
        }
    }

    void action(curl_socket_t fd, int events)
    {
        int running = 0;
        (void)curl_multi_socket_action(m_multi, fd, events, &running);
        int queued = 0;
        while (const CURLMsg* message = curl_multi_info_read(m_multi, &queued)) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            Transfer* transfer = nullptr;
            (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &transfer);
            const CURLcode code = message->data.result;
            if (transfer != nullptr) {
                m_owner.onTransferDone(*transfer, code);
            }
        }
    }

    CurlTransport& m_owner;
    CURLM* m_multi;
    QTimer m_timer;
    std::unordered_map<curl_socket_t, Watch> m_watches;
    int m_transfers = 0;
};

CurlTransport::CurlTransport(IDnsResolver& resolver, IProxyRouter& router, QObject* parent)
    : QObject(parent)
    , m_resolver(resolver)
    , m_router(router)
{
    if (auto init = initialiseCurl(); !init) {
        qCCritical(lcNet).noquote() << QString::fromStdString(init.error().describe());
    }
    qRegisterMetaType<RequestRecord>();
    m_refill.setSingleShot(true);
    connect(&m_refill, &QTimer::timeout, this, &CurlTransport::resumePaused);
    m_session = std::make_unique<CurlSession>(*this, m_config);
}

CurlTransport::~CurlTransport()
{
    for (auto& [id, transfer] : m_transfers) {
        if (transfer->session != nullptr) {
            transfer->session->remove(*transfer);
        }
    }
    m_transfers.clear();
}

void CurlTransport::setConfig(const TransportConfig& config)
{
    const bool connectionsChanged =
        config.httpVersion != m_config.httpVersion || config.maxConnections != m_config.maxConnections ||
        config.maxConnectionsPerHost != m_config.maxConnectionsPerHost || config.tls13Only != m_config.tls13Only ||
        config.caBundle != m_config.caBundle || config.connectTimeout != m_config.connectTimeout;
    const bool bandwidthChanged = config.bandwidthBytesPerSecond != m_config.bandwidthBytesPerSecond;
    m_config = config;
    if (connectionsChanged) {
        drainConnections();
    }
    if (bandwidthChanged) {
        m_bandwidth.setRate(config.bandwidthBytesPerSecond, TokenBucket::Clock::now());
        resumePaused();
    }
    if (config.offline) {
        for (auto& [id, transfer] : m_transfers) {
            transfer->abortReason = ErrorCode::Offline;
            transfer->state->cancelled.store(true, std::memory_order_relaxed);
        }
    }
}

void CurlTransport::setMetered(bool metered)
{
    m_metered = metered;
}

void CurlTransport::drainConnections()
{
    if (m_session && m_session->transfers() > 0) {
        m_draining.push_back(std::move(m_session));
    }
    m_session = std::make_unique<CurlSession>(*this, m_config);
}

RequestHandle CurlTransport::send(Request request, QObject* context, Callbacks callbacks)
{
    Q_ASSERT(context != nullptr && callbacks.onFinished);
    const auto state = std::make_shared<RequestHandle::State>();
    auto transfer = std::make_unique<Transfer>();
    transfer->request = std::move(request);
    transfer->callbacks = std::move(callbacks);
    transfer->state = state;
    transfer->channel = makeChannel(context, std::shared_ptr<std::atomic<bool>>(state, &state->cancelled), thread());
    handoff::post(this, [this, transfer = std::move(transfer)] mutable { begin(std::move(transfer)); });
    return RequestHandle(state);
}

void CurlTransport::sendDoh(const QUrl& url, std::vector<IpAddress> pinned, QByteArray query,
                            std::function<void(Result<QByteArray>)> callback)
{
    auto transfer = std::make_unique<Transfer>();
    transfer->request.method = Method::Post;
    transfer->request.url = url;
    transfer->request.body = std::move(query);
    transfer->request.headers = {{"content-type", "application/dns-message"}, {"accept", "application/dns-message"}};
    transfer->request.timeout = kDohTimeout;
    transfer->request.retry = false;
    transfer->request.maxRedirects = 0;
    transfer->state = std::make_shared<RequestHandle::State>();
    transfer->pinned = std::move(pinned);
    transfer->internalDone = [callback = std::move(callback)](Result<Response> result) {
        if (!result) {
            const ErrorCode code =
                result.error().code == ErrorCode::Offline ? ErrorCode::Offline : ErrorCode::DohUnreachable;
            callback(fail(code, result.error().describe()));
        } else if (result->status != 200 || !result->header("content-type").startsWith("application/dns-message")) {
            callback(fail(ErrorCode::DohInvalidResponse, std::format("HTTP {}", result->status)));
        } else {
            callback(std::move(result->body));
        }
    };
    begin(std::move(transfer));
}

DohSender CurlTransport::dohSender()
{
    return [self = QPointer(this)](const QUrl& url, std::vector<IpAddress> pinned, QByteArray query,
                                   std::function<void(Result<QByteArray>)> callback) {
        if (self != nullptr) {
            self->sendDoh(url, std::move(pinned), std::move(query), std::move(callback));
        }
    };
}

Transfer* CurlTransport::find(std::uint64_t id)
{
    const auto it = m_transfers.find(id);
    return it == m_transfers.end() ? nullptr : it->second.get();
}

void CurlTransport::begin(std::unique_ptr<Transfer> transfer)
{
    const std::uint64_t id = m_nextId++;
    transfer->id = id;
    transfer->owner = this;
    m_transfers.emplace(id, std::move(transfer));
    startAttempt(id);
}

void CurlTransport::startAttempt(std::uint64_t id)
{
    Transfer* transfer = find(id);
    if (transfer == nullptr) {
        return;
    }
    if (transfer->cancelled()) {
        finish(id, fail(transfer->abortReason.value_or(ErrorCode::Cancelled)));
        return;
    }
    const QUrl& url = transfer->request.url;
    if (!url.isValid() || (url.scheme() != u"https" && url.scheme() != u"http") || url.host().isEmpty()) {
        finish(id, fail(ErrorCode::InvalidArgument, "unsupported URL"));
        return;
    }
    if (m_config.offline) {
        finish(id, fail(ErrorCode::Offline, "offline mode"));
        return;
    }
    if (m_metered && transfer->request.background) {
        finish(id, fail(ErrorCode::MeteredDeferred, "background request on a metered connection"));
        return;
    }
    transfer->clock.start();
    transfer->response = Response{};
    transfer->response.url = url;
    transfer->response.attempts = transfer->retries + 1;
    transfer->finalHeaders = false;
    transfer->streaming = false;
    transfer->dnsMs = 0;
    if (transfer->internal()) {
        // DoH goes straight to its pinned addresses: no proxy, no resolver (which would be circular).
        transfer->dnsSource = ResolverSource::Literal;
        transfer->route = {};
        launch(id, {resolveEntry(url.host(QUrl::FullyEncoded), defaultPort(url), transfer->pinned)});
        return;
    }
    m_router.route(url, [self = QPointer(this), id](Result<ProxyRoute> route) {
        if (self != nullptr) {
            self->onRouted(id, std::move(route));
        }
    });
}

void CurlTransport::onRouted(std::uint64_t id, Result<ProxyRoute> route)
{
    Transfer* transfer = find(id);
    if (transfer == nullptr) {
        return;
    }
    if (!route) {
        finish(id, std::unexpected(route.error()));
        return;
    }
    transfer->route = std::move(*route);
    const QUrl& url = transfer->request.url;

    struct Lookup {
        QString host;
        int port = 0;
        bool proxyHost = false;
    };
    std::vector<Lookup> lookups;
    std::vector<QByteArray> literals;
    // IP literals are pinned too: curl may otherwise still start its resolver for them (seen with a
    // refused proxy connection), which the guard would turn into a failure.
    const auto add = [&](const QString& host, int port, bool proxyHost) {
        if (const auto literal = IpAddress::parse(host)) {
            if (literal->family() == AddressFamily::V4) {
                literals.push_back(resolveEntry(host, port, {*literal}));
            }
        } else {
            lookups.push_back({host, port, proxyHost});
        }
    };
    const auto& proxy = transfer->route.proxy;
    if (proxy) {
        add(proxy->host, proxy->port, true);
    }
    // Destination names stay with the proxy unless it needs addresses (plain SOCKS4/SOCKS5).
    if (!proxy || !proxy->resolvesRemotely()) {
        add(url.host(QUrl::FullyEncoded), defaultPort(url), false);
    }
    if (lookups.empty()) {
        launch(id, literals);
        return;
    }

    struct Gathered {
        int remaining = 0;
        std::vector<QByteArray> entries;
        std::optional<Error> error;
    };
    const auto gathered = std::make_shared<Gathered>();
    gathered->remaining = static_cast<int>(lookups.size());
    gathered->entries = std::move(literals);
    for (const Lookup& lookup : lookups) {
        m_resolver.resolve(lookup.host, [self = QPointer(this), id, lookup, gathered](Result<Resolution> result) {
            if (self == nullptr) {
                return;
            }
            Transfer* t = self->find(id);
            if (result) {
                gathered->entries.push_back(resolveEntry(lookup.host, lookup.port, result->addresses));
                if (t != nullptr && !lookup.proxyHost) {
                    t->dnsSource = result->source;
                }
            } else if (!gathered->error) {
                Error error = result.error();
                if (lookup.proxyHost) {
                    error.detail = "proxy host: " + error.detail;
                }
                gathered->error = std::move(error);
            }
            if (--gathered->remaining > 0 || t == nullptr) {
                return;
            }
            t->dnsMs = static_cast<double>(t->clock.nsecsElapsed()) / 1e6;
            if (gathered->error) {
                Result<Response> failure = std::unexpected(*gathered->error);
                self->record(*t, failure);
                self->finish(id, std::move(failure));
                return;
            }
            self->launch(id, gathered->entries);
        });
    }
}

void CurlTransport::launch(std::uint64_t id, const std::vector<QByteArray>& resolveEntries)
{
    Transfer* transfer = find(id);
    if (transfer == nullptr) {
        return;
    }
    if (transfer->cancelled()) {
        finish(id, fail(transfer->abortReason.value_or(ErrorCode::Cancelled)));
        return;
    }
    const Request& request = transfer->request;
    transfer->easy.reset(curl_easy_init());
    CURL* easy = transfer->easy.get();
    if (easy == nullptr) {
        finish(id, fail(ErrorCode::Unknown, "curl_easy_init failed"));
        return;
    }
    setopt(easy, CURLOPT_PRIVATE, transfer);
    setopt(easy, CURLOPT_URL, request.url.toEncoded().constData());
    setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    setopt(easy, CURLOPT_NOSIGNAL, 1L);
    setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    setopt(easy, CURLOPT_PIPEWAIT, 1L);
    setopt(easy, CURLOPT_ERRORBUFFER, transfer->errorBuffer.data());
    setopt(easy, CURLOPT_USERAGENT, "Noctara/" NOCTARA_VERSION);
    setopt(easy, CURLOPT_HTTP_VERSION, curlHttpVersion(m_config.httpVersion));
    setopt(easy, CURLOPT_CONNECTTIMEOUT_MS,
           static_cast<long>(std::chrono::milliseconds(m_config.connectTimeout).count()));
    setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    setopt(easy, CURLOPT_LOW_SPEED_TIME, static_cast<long>(m_config.readTimeout.count()));
    if (request.timeout.count() > 0) {
        setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
    }

    // TLS: verification always on; the minimum version and CA bundle are the only knobs.
    const long tlsVersion = m_config.tls13Only ? CURL_SSLVERSION_TLSv1_3 : CURL_SSLVERSION_TLSv1_2;
    setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    setopt(easy, CURLOPT_SSLVERSION, tlsVersion);
    if (!m_config.caBundle.isEmpty()) {
        setopt(easy, CURLOPT_CAINFO, m_config.caBundle.toLocal8Bit().constData());
    }

    // Proxy: empty strings disable curl's own environment lookups (*_proxy and no_proxy, which would
    // otherwise drop the chosen proxy); routing, bypass rules included, was decided above.
    const auto& proxy = transfer->route.proxy;
    setopt(easy, CURLOPT_PROXY, proxy ? proxy->curlUrl().toUtf8().constData() : "");
    setopt(easy, CURLOPT_NOPROXY, "");
    if (proxy) {
        setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
        setopt(easy, CURLOPT_PROXY_SSL_VERIFYPEER, 1L);
        setopt(easy, CURLOPT_PROXY_SSL_VERIFYHOST, 2L);
        setopt(easy, CURLOPT_PROXY_SSLVERSION, tlsVersion);
        if (!m_config.caBundle.isEmpty()) {
            setopt(easy, CURLOPT_PROXY_CAINFO, m_config.caBundle.toLocal8Bit().constData());
        }
        if (proxy->hasCredentials()) {
            setopt(easy, CURLOPT_PROXYUSERNAME, proxy->username.toUtf8().constData());
            setopt(easy, CURLOPT_PROXYPASSWORD, proxy->password.toUtf8().constData());
            setopt(easy, CURLOPT_PROXYAUTH, static_cast<long>(CURLAUTH_ANY));
        }
    }

    // Pinned addresses; curl must never resolve anything itself.
    transfer->resolveList.reset();
    for (const QByteArray& entry : resolveEntries) {
        transfer->resolveList.reset(curl_slist_append(transfer->resolveList.release(), entry.constData()));
    }
    if (transfer->resolveList) {
        setopt(easy, CURLOPT_RESOLVE, transfer->resolveList.get());
    }
    setopt(easy, CURLOPT_RESOLVER_START_FUNCTION, &CurlTransport::onResolverStart);
    setopt(easy, CURLOPT_RESOLVER_START_DATA, this);

    // Method, body and headers.
    switch (request.method) {
    case Method::Get: break;
    case Method::Head: setopt(easy, CURLOPT_NOBODY, 1L); break;
    case Method::Post:
    case Method::Put:
    case Method::Delete:
        if (request.method != Method::Post) {
            setopt(easy, CURLOPT_CUSTOMREQUEST, methodName(request.method).constData());
        }
        if (request.method != Method::Delete || !request.body.isEmpty()) {
            setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
            setopt(easy, CURLOPT_COPYPOSTFIELDS, request.body.constData());
        }
        break;
    }
    transfer->headerList.reset(curl_slist_append(nullptr, "Expect:"));
    for (const auto& [name, value] : request.headers) {
        const QByteArray line = name + ": " + value;
        transfer->headerList.reset(curl_slist_append(transfer->headerList.release(), line.constData()));
    }
    if (request.range) {
        const QByteArray range = QByteArray::number(request.range->first) + '-' +
                                 (request.range->last >= 0 ? QByteArray::number(request.range->last) : QByteArray());
        setopt(easy, CURLOPT_RANGE, range.constData());
    } else if (!transfer->callbacks.onData) {
        setopt(easy, CURLOPT_ACCEPT_ENCODING, "");  // all encodings curl supports; streams stay identity
    }
    setopt(easy, CURLOPT_HTTPHEADER, transfer->headerList.get());

    setopt(easy, CURLOPT_WRITEFUNCTION, &CurlTransport::onWrite);
    setopt(easy, CURLOPT_WRITEDATA, transfer);
    setopt(easy, CURLOPT_HEADERFUNCTION, &CurlTransport::onHeader);
    setopt(easy, CURLOPT_HEADERDATA, transfer);
    setopt(easy, CURLOPT_NOPROGRESS, 0L);
    setopt(easy, CURLOPT_XFERINFOFUNCTION, &CurlTransport::onProgress);
    setopt(easy, CURLOPT_XFERINFODATA, transfer);

    if (!m_session->add(*transfer)) {
        finish(id, fail(ErrorCode::Unknown, "curl_multi_add_handle failed"));
    }
}

std::size_t CurlTransport::onWrite(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* transfer = static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    if (transfer->cancelled()) {
        return 0;  // aborts the transfer (CURLE_WRITE_ERROR)
    }
    CurlTransport& owner = *transfer->owner;
    if (!owner.m_bandwidth.tryConsume(static_cast<std::int64_t>(bytes), TokenBucket::Clock::now())) {
        owner.pauseForBandwidth(*transfer);
        return CURL_WRITEFUNC_PAUSE;
    }
    transfer->response.bytesReceived += static_cast<std::int64_t>(bytes);
    if (!transfer->finalHeaders) {
        return bytes;  // body of an intermediate response (redirect or proxy reply): not delivered
    }
    if (transfer->streaming) {
        transfer->deliveredData = true;
        transfer->post([onData = transfer->callbacks.onData, chunk = QByteArray(data, static_cast<qsizetype>(bytes))] {
            onData(chunk);
        });
    } else {
        transfer->response.body.append(data, static_cast<qsizetype>(bytes));
    }
    return bytes;
}

std::size_t CurlTransport::onHeader(char* data, std::size_t size, std::size_t count, void* user)
{
    auto* transfer = static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    const QByteArrayView line = QByteArrayView(data, static_cast<qsizetype>(bytes)).trimmed();
    Response& response = transfer->response;
    if (line.startsWith("HTTP/")) {
        // A new response begins (1xx interim answers precede the final one).
        const QList<QByteArray> parts = line.toByteArray().split(' ');
        response.status = parts.size() > 1 ? parts.at(1).toInt() : 0;
        response.headers.clear();
        transfer->finalHeaders = false;
        return bytes;
    }
    if (!line.isEmpty()) {
        const qsizetype colon = line.indexOf(':');
        if (colon > 0) {
            response.headers.append(
                {line.first(colon).trimmed().toByteArray().toLower(), line.sliced(colon + 1).trimmed().toByteArray()});
        }
        return bytes;
    }
    if (response.status < 200) {
        return bytes;  // end of an interim response
    }
    const bool willRedirect = isRedirect(response.status) && !response.header("location").isEmpty() &&
                              transfer->redirects < transfer->request.maxRedirects;
    if (willRedirect) {
        return bytes;
    }
    transfer->finalHeaders = true;
    transfer->streaming = transfer->callbacks.onData && response.successful();
    if (transfer->callbacks.onResponse) {
        transfer->post([onResponse = transfer->callbacks.onResponse, head = response] { onResponse(head); });
    }
    return bytes;
}

int CurlTransport::onProgress(void* user, std::int64_t /*dlTotal*/, std::int64_t /*dlNow*/, std::int64_t /*ulTotal*/,
                              std::int64_t /*ulNow*/)
{
    return static_cast<Transfer*>(user)->cancelled() ? 1 : 0;
}

int CurlTransport::onResolverStart(void* /*resolverState*/, void* /*reserved*/, void* user)
{
    auto* self = static_cast<CurlTransport*>(user);
    ++self->m_unexpectedResolves;
    qCWarning(lcNet) << "blocked a DNS lookup by libcurl itself: every host must be resolved by Noctara";
    return 1;
}

void CurlTransport::pauseForBandwidth(Transfer& transfer)
{
    transfer.paused = true;
    if (!m_refill.isActive()) {
        m_refill.start(m_bandwidth.waitTime(TokenBucket::Clock::now()));
    }
}

void CurlTransport::resumePaused()
{
    std::vector<std::uint64_t> paused;
    for (const auto& [id, transfer] : m_transfers) {
        if (transfer->paused) {
            paused.push_back(id);
        }
    }
    for (const std::uint64_t id : paused) {
        if (Transfer* transfer = find(id); transfer != nullptr && transfer->easy) {
            transfer->paused = false;
            (void)curl_easy_pause(transfer->easy.get(), CURLPAUSE_CONT);
        }
    }
}

bool CurlTransport::retryAllowed(const Transfer& transfer) const
{
    return !transfer.internal() && !transfer.cancelled() && !transfer.deliveredData &&
           transfer.request.retry.value_or(isIdempotent(transfer.request.method)) &&
           transfer.retries < m_config.retries;
}

void CurlTransport::detach(Transfer& transfer)
{
    CurlSession* session = transfer.session;
    if (session != nullptr) {
        session->remove(transfer);
    }
    transfer.easy.reset();
    transfer.headerList.reset();
    transfer.resolveList.reset();
    transfer.paused = false;
    if (session != nullptr && session != m_session.get() && session->transfers() == 0) {
        // A drained session closes its connections once nothing runs on it; not from inside its own callback.
        QMetaObject::invokeMethod(
            this,
            [this, session] {
                std::erase_if(m_draining, [&](const auto& s) { return s.get() == session && s->transfers() == 0; });
            },
            Qt::QueuedConnection);
    }
}

void CurlTransport::onTransferDone(Transfer& transfer, int rawCode)
{
    const auto code = static_cast<CURLcode>(rawCode);
    CURL* easy = transfer.easy.get();
    Response& response = transfer.response;
    long httpVersion = 0;
    long connectStatus = 0;
    long proxyError = 0;
    long osErrno = 0;
    const char* remote = nullptr;
    (void)curl_easy_getinfo(easy, CURLINFO_HTTP_VERSION, &httpVersion);
    (void)curl_easy_getinfo(easy, CURLINFO_HTTP_CONNECTCODE, &connectStatus);
    (void)curl_easy_getinfo(easy, CURLINFO_PROXY_ERROR, &proxyError);
    (void)curl_easy_getinfo(easy, CURLINFO_OS_ERRNO, &osErrno);
    (void)curl_easy_getinfo(easy, CURLINFO_PRIMARY_IP, &remote);
    response.httpVersion = httpVersionName(httpVersion);
    response.remoteAddress = remote != nullptr ? QString::fromLatin1(remote) : QString();
    response.route = transfer.route.proxy ? transfer.route.proxy->displayUrl() : QStringLiteral("direct");
    const double dns = transfer.dnsMs;
    response.timings = {
        .dnsMs = dns,
        .connectMs = dns + millis(easy, CURLINFO_CONNECT_TIME_T),
        .tlsMs = millis(easy, CURLINFO_APPCONNECT_TIME_T) > 0 ? dns + millis(easy, CURLINFO_APPCONNECT_TIME_T) : 0,
        .firstByteMs = dns + millis(easy, CURLINFO_STARTTRANSFER_TIME_T),
        .totalMs = static_cast<double>(transfer.clock.nsecsElapsed()) / 1e6};
    // CONNECT never answered, no TLS finished, yet the proxy took the TCP connection: its TLS is what failed.
    const bool proxyTlsPending = transfer.route.proxy && transfer.route.proxy->type == ProxyType::Https &&
                                 connectStatus == 0 && millis(easy, CURLINFO_CONNECT_TIME_T) > 0 &&
                                 millis(easy, CURLINFO_APPCONNECT_TIME_T) == 0;
    const QString curlDetail = QString::fromLatin1(transfer.errorBuffer.data());
    detach(transfer);
    const std::uint64_t id = transfer.id;

    if (code != CURLE_OK) {
        ErrorCode error = transfer.abortReason.value_or(errorFromCurl({.code = code,
                                                                       .osErrno = osErrno,
                                                                       .connectStatus = connectStatus,
                                                                       .proxyError = proxyError,
                                                                       .viaProxy = transfer.route.proxy.has_value(),
                                                                       .proxyTlsPending = proxyTlsPending}));
        if (transfer.cancelled() && !transfer.abortReason) {
            error = ErrorCode::Cancelled;
        }
        Result<Response> failure =
            fail(error, curlDetail.isEmpty() ? curl_easy_strerror(code) : curlDetail.toStdString());
        record(transfer, failure);
        if (error != ErrorCode::Cancelled && RetryPolicy::retryableError(error) && retryAllowed(transfer)) {
            retryLater(transfer, std::nullopt);
            return;
        }
        finish(id, std::move(failure));
        return;
    }
    if (response.status == 407 && transfer.route.proxy) {
        Result<Response> failure = fail(ErrorCode::ProxyAuthRequired, "proxy answered 407");
        record(transfer, failure);
        finish(id, std::move(failure));
        return;
    }
    record(transfer, response);
    if (isRedirect(response.status) && !response.header("location").isEmpty()) {
        if (transfer.redirects >= transfer.request.maxRedirects) {
            finish(id, fail(ErrorCode::HttpError, "too many redirects"));
            return;
        }
        followRedirect(transfer, response.url.resolved(QUrl::fromEncoded(response.header("location"))));
        return;
    }
    if (RetryPolicy::retryableStatus(response.status) && retryAllowed(transfer)) {
        retryLater(transfer, parseRetryAfter(response.header("retry-after"), QDateTime::currentDateTimeUtc()));
        return;
    }
    finish(id, std::move(response));
}

void CurlTransport::retryLater(Transfer& transfer, std::optional<std::chrono::seconds> retryAfter)
{
    const RetryPolicy policy{.maxRetries = m_config.retries, .baseDelay = m_config.retryBackoff};
    const auto delay = policy.delay(transfer.retries, retryAfter, QRandomGenerator::global()->generateDouble());
    ++transfer.retries;
    qCDebug(lcNet).noquote() << "retrying" << redactUrl(transfer.request.url) << "in" << delay.count() << "ms";
    QTimer::singleShot(delay, this, [this, id = transfer.id] { startAttempt(id); });
}

void CurlTransport::followRedirect(Transfer& transfer, const QUrl& target)
{
    Request& request = transfer.request;
    const int status = transfer.response.status;
    if (status == 303 || ((status == 301 || status == 302) && request.method == Method::Post)) {
        request.method = Method::Get;
        request.body.clear();
    }
    if (target.scheme() != request.url.scheme() || target.host() != request.url.host() ||
        defaultPort(target) != defaultPort(request.url)) {
        // Credentials meant for one origin never travel to another (nor from https down to http).
        request.headers.removeIf(
            [](const auto& header) { return header.first == "authorization" || header.first == "cookie"; });
    }
    request.url = target;
    ++transfer.redirects;
    startAttempt(transfer.id);
}

void CurlTransport::record(const Transfer& transfer, const Result<Response>& result)
{
    RequestRecord entry;
    entry.finishedAt = QDateTime::currentDateTime();
    entry.method = methodName(transfer.request.method);
    entry.url = redactUrl(transfer.request.url);
    entry.route = transfer.route.proxy ? transfer.route.proxy->displayUrl() : QStringLiteral("direct");
    entry.dnsSource = transfer.dnsSource;
    entry.attempt = transfer.retries + 1;
    entry.internal = transfer.internal();
    if (result) {
        entry.status = result->status;
        entry.timings = result->timings;
        entry.bytes = result->bytesReceived;
        entry.httpVersion = result->httpVersion;
        entry.remoteAddress = result->remoteAddress;
    } else {
        entry.error = result.error().code;
        entry.errorDetail = QString::fromStdString(result.error().detail);
        entry.timings = transfer.response.timings;
        entry.timings.dnsMs = transfer.dnsMs;
        entry.timings.totalMs = static_cast<double>(transfer.clock.nsecsElapsed()) / 1e6;
    }
    handoff::send([&] { Q_EMIT requestFinished(entry); });  // received on the GUI thread
}

void CurlTransport::finish(std::uint64_t id, Result<Response> result)
{
    const auto it = m_transfers.find(id);
    if (it == m_transfers.end()) {
        return;
    }
    std::unique_ptr<Transfer> transfer = std::move(it->second);
    m_transfers.erase(it);
    detach(*transfer);
    if (!result) {
        qCDebug(lcNet).noquote() << methodName(transfer->request.method) << redactUrl(transfer->request.url)
                                 << "failed:" << QString::fromStdString(result.error().describe());
    }
    if (transfer->internalDone) {
        QMetaObject::invokeMethod(
            this, [done = std::move(transfer->internalDone), result = std::move(result)] { done(result); },
            Qt::QueuedConnection);
        return;
    }
    transfer->post(
        [callbacks = std::move(transfer->callbacks), result = std::move(result)] { callbacks.onFinished(result); });
}

QByteArray Response::header(QByteArrayView name) const
{
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

bool isIdempotent(Method method) noexcept
{
    return method != Method::Post;
}

QByteArray methodName(Method method)
{
    switch (method) {
    case Method::Get: return QByteArrayLiteral("GET");
    case Method::Head: return QByteArrayLiteral("HEAD");
    case Method::Post: return QByteArrayLiteral("POST");
    case Method::Put: return QByteArrayLiteral("PUT");
    case Method::Delete: return QByteArrayLiteral("DELETE");
    }
    return QByteArrayLiteral("GET");
}

}  // namespace noctara::net
