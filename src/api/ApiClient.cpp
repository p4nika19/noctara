#include "api/ApiClient.hpp"

#include "api/ClientId.hpp"
#include "api/Parse.hpp"
#include "api/Session.hpp"

#include "core/Log.hpp"
#include "net/CallbackChannel.hpp"
#include "net/RetryPolicy.hpp"

#include <QDateTime>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <string>

namespace noctara::api {

namespace {

using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::steady_clock;

using StatePtr = std::shared_ptr<net::RequestHandle::State>;

bool cancelled(const StatePtr& state)
{
    return state->cancelled.load(std::memory_order_relaxed);
}

std::shared_ptr<std::atomic<bool>> cancelFlag(const StatePtr& state)
{
    return {state, &state->cancelled};
}

// Media hosts (waveforms) get neither the token nor the client_id, but still only over https.
bool allowed(const ApiRequest& request)
{
    if (request.clientId || request.auth != Auth::None) {
        return isApiHost(request.url);
    }
    return request.url.isValid() && request.url.scheme() == u"https";
}

QUrl withClientId(QUrl url, const QString& id)
{
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("client_id"));
    query.addQueryItem(QStringLiteral("client_id"), id);
    url.setQuery(query);
    return url;
}

}  // namespace

struct ApiClient::Subscriber {
    Decoder decode;
    StatePtr state = std::make_shared<net::RequestHandle::State>();
    net::ChannelPtr channel;  // null for the client's own requests: the closure runs on the API thread
};

struct ApiClient::Flight {
    QByteArray key;  // empty: not shared with later identical requests
    ApiRequest request;
    std::vector<std::shared_ptr<Subscriber>> subscribers;
    int attempts = 0;
    bool anonymous = false;  // the token was refused; the endpoint is repeated without it
    QByteArray tokenSent;
    QString clientIdSent;
};

struct ApiClient::Lookup {
    std::vector<Id> ids;
    std::function<void(Result<std::vector<Track>>)> done;
    StatePtr state = std::make_shared<net::RequestHandle::State>();
    net::ChannelPtr channel;
};

struct ApiClient::Batch {
    std::vector<std::shared_ptr<Lookup>> lookups;
    QHash<Id, Track> found;
    QHash<Id, Error> failed;
    int pending = 0;
};

ApiClient::ApiClient(net::INetworkTransport& transport, Session& session, ClientIdProvider& clientIds, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_session(session)
    , m_clientIds(clientIds)
    , m_mailbox(std::make_shared<cache::Mailbox>(this))
{
}

ApiClient::~ApiClient()
{
    m_mailbox->close();
}

net::RequestHandle ApiClient::send(ApiRequest request, QObject* context, Decoder decode)
{
    const auto subscriber = std::make_shared<Subscriber>();
    subscriber->decode = std::move(decode);
    subscriber->channel = net::makeChannel(context, cancelFlag(subscriber->state), thread());
    net::RequestHandle handle(subscriber->state);
    m_mailbox->post(
        [this, request = std::move(request), subscriber] mutable { admit(std::move(request), subscriber); });
    return handle;
}

void ApiClient::admit(ApiRequest request, const std::shared_ptr<Subscriber>& subscriber)
{
    if (cancelled(subscriber->state)) {
        finish(subscriber, fail(ErrorCode::Cancelled, "cancelled"));
        return;
    }
    if (!allowed(request)) {
        finish(subscriber, fail(ErrorCode::InvalidArgument, "not a SoundCloud API URL"));
        return;
    }
    const QByteArray token = request.auth == Auth::None          ? QByteArray()
                             : !request.candidateToken.isEmpty() ? request.candidateToken
                                                                 : m_session.token();
    if (request.auth == Auth::Required && token.isEmpty()) {
        finish(subscriber, fail(ErrorCode::SignInRequired, "endpoint needs a signed-in account"));
        return;
    }
    QByteArray key;
    if (request.method == net::Method::Get) {
        key = request.url.toEncoded() + ' ' + Session::digest(token) + ' ' +
              QByteArray::number(static_cast<int>(request.cache));
        if (const auto existing = m_flights.value(key)) {
            existing->subscribers.push_back(subscriber);
            return;
        }
    }
    const auto flight = std::make_shared<Flight>();
    flight->key = key;
    flight->request = std::move(request);
    flight->subscribers.push_back(subscriber);
    if (!key.isEmpty()) {
        m_flights.insert(key, flight);
    }
    dispatch(flight);
}

void ApiClient::dispatch(const std::shared_ptr<Flight>& flight)
{
    // Callers that gave up before the request left are not waited for; a request nobody wants is not sent.
    std::erase_if(flight->subscribers, [](const std::shared_ptr<Subscriber>& subscriber) {
        if (cancelled(subscriber->state)) {
            finish(subscriber, fail(ErrorCode::Cancelled, "cancelled"));
            return true;
        }
        return false;
    });
    if (flight->subscribers.empty()) {
        complete(flight, fail(ErrorCode::Cancelled, "cancelled"));
        return;
    }
    if (const auto now = steady_clock::now(); now < m_pausedUntil) {
        const auto wait = m_pausedUntil - now;
        if (wait > kMaxWait) {
            complete(flight, fail(ErrorCode::RateLimited, "SoundCloud asked to pause requests"));
            return;
        }
        QTimer::singleShot(duration_cast<milliseconds>(wait), this, [this, flight] { dispatch(flight); });
        return;
    }
    if (flight->request.clientId && m_clientIds.current().isEmpty()) {
        m_clientIds.acquire([self = QPointer(this), flight](Result<QString> id) {
            if (!self) {
                return;
            }
            if (id) {
                self->dispatch(flight);  // drops callers that gave up meanwhile and honours a pause
            } else {
                self->complete(flight, std::unexpected(id.error()));
            }
        });
        return;
    }
    transmit(flight);
}

void ApiClient::transmit(const std::shared_ptr<Flight>& flight)
{
    const ApiRequest& api = flight->request;
    net::Request request;
    request.method = api.method;
    request.url = api.url;
    request.body = api.body;
    request.cache = api.cache;
    request.cacheTtl = api.cacheTtl;
    request.background = api.background;
    request.headers.append({QByteArrayLiteral("accept"), QByteArrayLiteral("application/json")});
    if (!api.body.isEmpty()) {
        request.headers.append({QByteArrayLiteral("content-type"), QByteArrayLiteral("application/json")});
    }
    flight->clientIdSent.clear();
    if (api.clientId) {
        flight->clientIdSent = m_clientIds.current();
        if (flight->clientIdSent.isEmpty()) {
            complete(flight, fail(ErrorCode::ClientIdUnavailable, "no client_id"));
            return;
        }
        request.url = withClientId(request.url, flight->clientIdSent);
    }
    flight->tokenSent.clear();
    if (api.auth != Auth::None && !flight->anonymous) {
        flight->tokenSent = api.candidateToken.isEmpty() ? m_session.token() : api.candidateToken;
        if (!flight->tokenSent.isEmpty()) {
            request.headers.append({QByteArrayLiteral("authorization"), "OAuth " + flight->tokenSent});
        } else if (api.auth == Auth::Required) {
            complete(flight, fail(ErrorCode::SignInRequired, "signed out meanwhile"));
            return;
        }
    }
    ++flight->attempts;
    m_transport.send(std::move(request), this, net::whenFinished([this, flight](Result<net::Response> result) {
                         onFinished(flight, std::move(result));
                     }));
}

void ApiClient::onFinished(const std::shared_ptr<Flight>& flight, Result<net::Response> result)
{
    if (!result) {
        complete(flight, std::unexpected(result.error()));
        return;
    }
    const net::Response& response = *result;
    if (response.successful()) {
        complete(flight, Payload{response.body, response.fromCache, response.stale});
        return;
    }
    const bool canRepeat = flight->attempts < kMaxAttempts;
    switch (response.status) {
    case 401:
        if (!flight->request.candidateToken.isEmpty()) {
            complete(flight, fail(ErrorCode::Unauthorized, "the candidate OAuth token was rejected"));
            return;
        }
        if (!flight->tokenSent.isEmpty()) {
            m_session.reportRejected(flight->tokenSent);
            if (flight->request.auth == Auth::Optional && canRepeat) {
                flight->anonymous = true;
                dispatch(flight);
                return;
            }
            complete(flight, fail(ErrorCode::Unauthorized, "the OAuth token was rejected"));
            return;
        }
        if (!flight->clientIdSent.isEmpty()) {
            m_clientIds.invalidate(flight->clientIdSent);
            if (canRepeat) {
                dispatch(flight);
                return;
            }
            complete(flight, fail(ErrorCode::ClientIdUnavailable, "the client_id was rejected"));
            return;
        }
        complete(flight, fail(ErrorCode::Unauthorized, "401"));
        return;
    case 403: complete(flight, fail(ErrorCode::Forbidden, "403")); return;
    case 404:
    case 410: complete(flight, fail(ErrorCode::NotFound, std::to_string(response.status))); return;
    case 429: {
        const auto pause = net::parseRetryAfter(response.header("retry-after"), QDateTime::currentDateTimeUtc())
                               .value_or(kDefaultPause);
        m_pausedUntil = std::max(m_pausedUntil, steady_clock::now() + pause);
        qCInfo(lcApi) << "rate limited by SoundCloud; pausing requests for" << pause.count() << "s";
        if (canRepeat && pause <= kMaxWait) {
            dispatch(flight);
            return;
        }
        complete(flight, fail(ErrorCode::RateLimited, "429"));
        return;
    }
    default: complete(flight, fail(ErrorCode::HttpError, "HTTP " + std::to_string(response.status))); return;
    }
}

void ApiClient::complete(const std::shared_ptr<Flight>& flight, const Result<Payload>& result)
{
    if (!flight->key.isEmpty()) {
        if (const auto it = m_flights.constFind(flight->key); it != m_flights.cend() && it.value() == flight) {
            m_flights.erase(it);
        }
    }
    for (const auto& subscriber : std::exchange(flight->subscribers, {})) {
        finish(subscriber, result);
    }
}

void ApiClient::finish(const std::shared_ptr<Subscriber>& subscriber, Result<Payload> result)
{
    if (cancelled(subscriber->state)) {
        result = fail(ErrorCode::Cancelled, "cancelled");
    }
    std::function<void()> deliver = subscriber->decode(std::move(result));
    if (subscriber->channel) {
        subscriber->channel->post(std::move(deliver));
        subscriber->channel.reset();
    } else if (deliver) {
        deliver();
    }
}

net::RequestHandle ApiClient::lookupTracks(std::vector<Id> ids, QObject* context,
                                           std::function<void(Result<std::vector<Track>>)> done)
{
    const auto lookup = std::make_shared<Lookup>();
    lookup->ids = std::move(ids);
    lookup->done = std::move(done);
    lookup->channel = net::makeChannel(context, cancelFlag(lookup->state), thread());
    net::RequestHandle handle(lookup->state);
    m_mailbox->post([this, lookup] {
        m_lookups.push_back(lookup);
        if (!m_lookupScheduled) {
            m_lookupScheduled = true;
            QTimer::singleShot(kCoalesceWindow, this, &ApiClient::flushLookups);
        }
    });
    return handle;
}

void ApiClient::flushLookups()
{
    m_lookupScheduled = false;
    const auto batch = std::make_shared<Batch>();
    batch->lookups = std::exchange(m_lookups, {});
    std::vector<Id> unique;
    QSet<Id> seen;
    for (const auto& lookup : batch->lookups) {
        if (cancelled(lookup->state)) {
            continue;
        }
        for (const Id id : lookup->ids) {
            if (!seen.contains(id)) {
                seen.insert(id);
                unique.push_back(id);
            }
        }
    }
    if (unique.empty()) {
        completeBatch(*batch);
        return;
    }
    std::vector<std::vector<Id>> chunks;
    for (std::size_t first = 0; first < unique.size(); first += endpoints::kMaxTrackIds) {
        const auto last = std::min(unique.size(), first + endpoints::kMaxTrackIds);
        chunks.emplace_back(unique.begin() + static_cast<std::ptrdiff_t>(first),
                            unique.begin() + static_cast<std::ptrdiff_t>(last));
    }
    batch->pending = static_cast<int>(chunks.size());
    for (auto& chunk : chunks) {
        const auto subscriber = std::make_shared<Subscriber>();
        const Endpoint<std::vector<Track>> endpoint = endpoints::tracks(chunk);
        subscriber->decode = [batch, chunk = std::move(chunk)](Result<Payload> payload) {
            Result<std::vector<Track>> tracks =
                payload ? parse::tracks(payload->body) : std::unexpected(payload.error());
            if (tracks) {
                for (Track& track : *tracks) {
                    batch->found.insert(track.id, std::move(track));
                }
            } else {
                for (const Id id : chunk) {
                    batch->failed.insert(id, tracks.error());
                }
            }
            if (--batch->pending == 0) {
                completeBatch(*batch);
            }
            return std::function<void()>();
        };
        admit(endpoint.request, subscriber);
    }
}

void ApiClient::completeBatch(const Batch& batch)
{
    for (const auto& lookup : batch.lookups) {
        Result<std::vector<Track>> result;
        if (cancelled(lookup->state)) {
            result = fail(ErrorCode::Cancelled, "cancelled");
        } else {
            std::vector<Track> tracks;
            tracks.reserve(lookup->ids.size());
            for (const Id id : lookup->ids) {
                if (const auto failed = batch.failed.constFind(id); failed != batch.failed.cend()) {
                    result = std::unexpected(failed.value());
                    break;
                }
                if (const auto found = batch.found.constFind(id); found != batch.found.cend()) {
                    tracks.push_back(found.value());
                }
            }
            if (result) {
                result = std::move(tracks);
            }
        }
        lookup->channel->post([done = lookup->done, result = std::move(result)] mutable { done(std::move(result)); });
        lookup->channel.reset();
    }
}

}  // namespace noctara::api
