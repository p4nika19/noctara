#include "cache/HttpCache.hpp"

#include "cache/Hash.hpp"
#include "core/Log.hpp"
#include "net/CallbackChannel.hpp"

#include <QDataStream>
#include <QDateTime>
#include <QIODevice>
#include <QLocale>
#include <QTimeZone>

#include <algorithm>

namespace noctara::cache {

namespace {

constexpr quint32 kMagic = 0x4e434852;  // "NCHR"
constexpr quint16 kVersion = 1;

QByteArray headerOf(const net::Headers& headers, QByteArrayView name)
{
    for (const auto& [key, value] : headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

std::optional<std::int64_t> httpDate(const QByteArray& value)
{
    if (value.isEmpty()) {
        return std::nullopt;
    }
    // IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
    const QDateTime when = QLocale::c().toDateTime(QString::fromLatin1(value.trimmed()).chopped(4),
                                                   QStringLiteral("ddd, dd MMM yyyy HH:mm:ss"));
    if (!when.isValid()) {
        return std::nullopt;
    }
    return QDateTime(when.date(), when.time(), QTimeZone::UTC).toSecsSinceEpoch();
}

QList<QByteArray> varyNames(const net::Headers& headers)
{
    QList<QByteArray> names;
    for (const QByteArray& name : headerOf(headers, "vary").split(',')) {
        if (const QByteArray trimmed = name.trimmed().toLower(); !trimmed.isEmpty()) {
            names.append(trimmed);
        }
    }
    return names;
}

bool transportFailure(const Error& error)
{
    return error.code != ErrorCode::Cancelled && error.code != ErrorCode::InvalidArgument;
}

}  // namespace

CacheControl parseCacheControl(QByteArrayView value)
{
    CacheControl control;
    for (const QByteArray& raw : value.toByteArray().split(',')) {
        const QByteArray directive = raw.trimmed().toLower();
        const qsizetype eq = directive.indexOf('=');
        const QByteArray name = eq < 0 ? directive : directive.first(eq);
        const QByteArray argument = eq < 0 ? QByteArray() : directive.sliced(eq + 1).trimmed();
        bool ok = false;
        const std::int64_t seconds = QByteArray(argument).replace('"', "").toLongLong(&ok);
        if (name == "no-store") {
            control.noStore = true;
        } else if (name == "no-cache") {
            control.noCache = true;
        } else if (name == "must-revalidate" || name == "proxy-revalidate") {
            control.mustRevalidate = true;
        } else if (name == "immutable") {
            control.immutable = true;
        } else if (name == "max-age" && ok) {
            control.maxAge = std::max<std::int64_t>(0, seconds);
        } else if (name == "stale-if-error" && ok) {
            control.staleIfError = std::max<std::int64_t>(0, seconds);
        }
    }
    return control;
}

QByteArray StoredResponse::header(QByteArrayView name) const
{
    return headerOf(headers, name);
}

QByteArray StoredResponse::serialize() const
{
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << kMagic << kVersion << static_cast<qint32>(status) << url.toEncoded() << static_cast<qint64>(storedAt)
        << static_cast<qint64>(ttlFallback) << static_cast<quint32>(headers.size());
    for (const auto& [name, value] : headers) {
        out << name << value;
    }
    out << static_cast<quint32>(varyValues.size());
    for (const auto& [name, value] : varyValues) {
        out << name << value;
    }
    out << body << static_cast<quint64>(checksum(body));
    return data;
}

Result<StoredResponse> StoredResponse::parse(const QByteArray& data)
{
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint16 version = 0;
    qint32 status = 0;
    QByteArray url;
    qint64 storedAt = 0;
    qint64 ttl = 0;
    quint32 count = 0;
    in >> magic >> version >> status >> url >> storedAt >> ttl >> count;
    if (magic != kMagic || version != kVersion || count > 512) {
        return fail(ErrorCode::Corrupt, "not a stored HTTP response");
    }
    StoredResponse stored;
    stored.status = status;
    stored.url = QUrl::fromEncoded(url);
    stored.storedAt = storedAt;
    stored.ttlFallback = ttl;
    const auto readHeaders = [&](net::Headers& target, quint32 n) {
        for (quint32 i = 0; i < n && in.status() == QDataStream::Ok; ++i) {
            QByteArray name;
            QByteArray value;
            in >> name >> value;
            target.append({name, value});
        }
    };
    readHeaders(stored.headers, count);
    quint32 varyCount = 0;
    in >> varyCount;
    if (varyCount > 64) {
        return fail(ErrorCode::Corrupt, "bad vary list");
    }
    readHeaders(stored.varyValues, varyCount);
    quint64 sum = 0;
    in >> stored.body >> sum;
    if (in.status() != QDataStream::Ok || sum != checksum(stored.body)) {
        return fail(ErrorCode::Corrupt, "stored HTTP response is damaged");
    }
    return stored;
}

bool isStorable(const net::Request& request, const net::Response& response)
{
    if (request.method != net::Method::Get || request.range || request.cache == net::CacheMode::Bypass) {
        return false;
    }
    if (parseCacheControl(headerOf(request.headers, "cache-control")).noStore ||
        parseCacheControl(response.header("cache-control")).noStore) {
        return false;
    }
    if (response.header("vary").trimmed() == "*") {
        return false;
    }
    switch (response.status) {
    case 200:
    case 203:
    case 204:
    case 300:
    case 301:
    case 404:
    case 410: return response.body.size() <= CachingTransport::kMaxStoredBody;
    default: return false;
    }
}

std::int64_t freshnessLifetime(const StoredResponse& stored)
{
    const CacheControl control = parseCacheControl(stored.header("cache-control"));
    if (control.noCache) {
        return 0;
    }
    if (control.maxAge) {
        return *control.maxAge;
    }
    const std::optional<std::int64_t> expires = httpDate(stored.header("expires"));
    if (expires) {
        const std::int64_t date = httpDate(stored.header("date")).value_or(stored.storedAt);
        return std::max<std::int64_t>(0, *expires - date);
    }
    return stored.ttlFallback;
}

bool isFresh(const StoredResponse& stored, std::int64_t now)
{
    bool ok = false;
    const std::int64_t age = std::max<std::int64_t>(0, stored.header("age").toLongLong(&ok));
    return now - stored.storedAt + (ok ? age : 0) < freshnessLifetime(stored);
}

QByteArray httpCacheKey(const net::Request& request)
{
    const QByteArray credentials = headerOf(request.headers, "authorization");
    return nameFor(QByteArray("http/v1\n" + request.url.adjusted(QUrl::RemoveFragment).toEncoded() + '\n' +
                              (credentials.isEmpty() ? QByteArray() : contentHash(credentials))));
}

bool varyMatches(const StoredResponse& stored, const net::Request& request)
{
    return std::ranges::all_of(stored.varyValues,
                               [&](const auto& pair) { return headerOf(request.headers, pair.first) == pair.second; });
}

// --- CachingTransport ---

struct CachingTransport::Pending {
    net::Request request;
    net::Callbacks callbacks;
    std::shared_ptr<net::RequestHandle::State> state;
    net::ChannelPtr channel;
    QByteArray key;
};

CachingTransport::CachingTransport(net::INetworkTransport& inner, FileOps& ops, QObject* parent)
    : QObject(parent)
    , m_inner(inner)
    , m_ops(ops)
    , m_mailbox(std::make_shared<Mailbox>(this))
{
}

CachingTransport::~CachingTransport()
{
    m_mailbox->close();
    flush();
}

void CachingTransport::setDisk(const QString& directory, std::int64_t budget)
{
    if (directory.isEmpty()) {
        flush();
        m_disk.reset();
        return;
    }
    if (!m_disk || m_disk->directory() != directory) {
        flush();
        m_disk = std::make_unique<DiskLru>(directory, budget, m_ops);
    }
    m_disk->setBudget(budget);
}

void CachingTransport::clear()
{
    if (m_disk) {
        m_disk->clear();
    }
}

void CachingTransport::flush()
{
    if (m_disk) {
        m_disk->flushIndex();
    }
}

std::int64_t CachingTransport::diskBytes() const
{
    if (!m_disk) {
        return 0;
    }
    m_disk->load();
    return m_disk->bytes();
}

net::RequestHandle CachingTransport::send(net::Request request, QObject* context, net::Callbacks callbacks)
{
    const bool cacheable = request.method == net::Method::Get && !request.range && !callbacks.onData &&
                           request.cache != net::CacheMode::Bypass;
    const bool unsafe = request.method == net::Method::Post || request.method == net::Method::Put ||
                        request.method == net::Method::Delete;
    if (!cacheable && !unsafe) {
        return m_inner.send(std::move(request), context, std::move(callbacks));
    }
    const auto pending = std::make_shared<Pending>();
    pending->state = std::make_shared<net::RequestHandle::State>();
    pending->channel = net::makeChannel(
        context, std::shared_ptr<std::atomic<bool>>(pending->state, &pending->state->cancelled), thread());
    pending->key = httpCacheKey(request);
    pending->request = std::move(request);
    pending->callbacks = std::move(callbacks);
    m_mailbox->post([this, pending] { handle(pending); });
    return net::RequestHandle(pending->state);
}

void CachingTransport::handle(const std::shared_ptr<Pending>& pending)
{
    if (pending->state->cancelled.load(std::memory_order_relaxed)) {
        complete(pending, fail(ErrorCode::Cancelled, "cancelled"));
        return;
    }
    if (pending->request.method != net::Method::Get) {
        forward(pending, std::nullopt);
        return;
    }
    std::optional<StoredResponse> stored;
    if (m_disk) {
        if (Result<QByteArray> data = m_disk->get(pending->key)) {
            if (Result<StoredResponse> parsed = StoredResponse::parse(*data);
                parsed && varyMatches(*parsed, pending->request)) {
                stored = std::move(*parsed);
            } else if (!parsed) {
                m_disk->remove(pending->key);  // damaged: dropped silently
            }
        }
    }

    const auto serve = [&](const StoredResponse& response, bool stale) {
        net::Response result;
        result.status = response.status;
        result.url = response.url;
        result.headers = response.headers;
        result.body = response.body;
        result.route = QStringLiteral("cache");
        result.fromCache = true;
        result.stale = stale;
        complete(pending, std::move(result));
    };
    const net::CacheMode mode = pending->request.cache;
    if (stored && mode == net::CacheMode::OnlyIfCached) {
        serve(*stored, !isFresh(*stored, unixNow()));
        return;
    }
    if (!stored && mode == net::CacheMode::OnlyIfCached) {
        complete(pending, fail(ErrorCode::Offline, "not in the HTTP cache"));
        return;
    }
    if (stored && mode == net::CacheMode::Default && isFresh(*stored, unixNow())) {
        serve(*stored, false);
        return;
    }
    forward(pending, std::move(stored));
}

void CachingTransport::forward(const std::shared_ptr<Pending>& pending, std::optional<StoredResponse> stored)
{
    net::Request request = pending->request;
    if (stored) {
        if (const QByteArray etag = stored->header("etag"); !etag.isEmpty()) {
            request.headers.append({"if-none-match", etag});
        }
        if (const QByteArray modified = stored->header("last-modified"); !modified.isEmpty()) {
            request.headers.append({"if-modified-since", modified});
        }
    }
    net::Callbacks callbacks;
    const auto previous = std::make_shared<std::optional<StoredResponse>>(std::move(stored));
    callbacks.onFinished = [this, pending, previous](Result<net::Response> result) {
        const std::optional<StoredResponse>& old = *previous;
        if (pending->request.method != net::Method::Get) {
            if (result && result->successful() && m_disk) {
                m_disk->remove(pending->key);  // the resource changed
            }
            complete(pending, std::move(result));
            return;
        }
        if (result && result->status == 304 && old) {
            // Revalidated: refresh the stored headers and serve the stored body.
            StoredResponse refreshed = *old;
            for (const auto& [name, value] : result->headers) {
                if (name == "cache-control" || name == "expires" || name == "date" || name == "etag" ||
                    name == "last-modified" || name == "age") {
                    refreshed.headers.removeIf([&](const auto& pair) { return pair.first == name; });
                    refreshed.headers.append({name, value});
                }
            }
            refreshed.storedAt = unixNow();
            if (m_disk) {
                if (auto stored = m_disk->put(pending->key, refreshed.serialize()); !stored) {
                    qCDebug(lcCache) << "revalidated response not stored:" << stored.error().describe();
                }
            }
            net::Response response = *result;
            response.status = refreshed.status;
            response.headers = refreshed.headers;
            response.body = refreshed.body;
            response.fromCache = true;
            complete(pending, std::move(response));
            return;
        }
        const bool failed = !result ? transportFailure(result.error()) : result->status >= 500;
        if (failed && old) {
            // The network let us down: a stale copy beats an error (offline mode, outages).
            net::Response response;
            response.status = old->status;
            response.url = old->url;
            response.headers = old->headers;
            response.body = old->body;
            response.route = QStringLiteral("cache");
            response.fromCache = true;
            response.stale = true;
            complete(pending, std::move(response));
            return;
        }
        if (result && m_disk) {
            if (isStorable(pending->request, *result)) {
                StoredResponse store;
                store.status = result->status;
                store.url = result->url;
                store.headers = result->headers;
                store.body = result->body;
                store.storedAt = unixNow();
                store.ttlFallback = pending->request.cacheTtl.count();
                for (const QByteArray& name : varyNames(result->headers)) {
                    store.varyValues.append({name, headerOf(pending->request.headers, name)});
                }
                if (auto stored = m_disk->put(pending->key, store.serialize()); !stored) {
                    qCDebug(lcCache) << "response not stored:" << stored.error().describe();
                }
            } else if (parseCacheControl(result->header("cache-control")).noStore) {
                m_disk->remove(pending->key);
            }
        }
        complete(pending, std::move(result));
    };
    // Headers are only final after a possible revalidation, so onResponse is reported together with the result.
    m_inner.send(std::move(request), this, std::move(callbacks));
}

void CachingTransport::complete(const std::shared_ptr<Pending>& pending, Result<net::Response> result)
{
    if (!pending->channel) {
        return;
    }
    if (pending->state->cancelled.load(std::memory_order_relaxed)) {
        result = fail(ErrorCode::Cancelled, "cancelled");
    }
    pending->channel->post([callbacks = pending->callbacks, result = std::move(result)] {
        if (result && callbacks.onResponse) {
            callbacks.onResponse(*result);
        }
        callbacks.onFinished(result);
    });
    pending->channel.reset();
}

}  // namespace noctara::cache
