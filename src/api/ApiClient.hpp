#pragma once

#include "api/Endpoints.hpp"
#include "api/Types.hpp"

#include "cache/Mailbox.hpp"
#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QByteArray>
#include <QHash>
#include <QObject>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace noctara::api {

class ClientIdProvider;
class Session;

struct Payload {
    QByteArray body;
    bool stale = false;  // served from the HTTP cache past its freshness because the network was unavailable
};

// Runs on the API thread with the answer (or the error) and returns what to run on the caller's thread: parsing
// happens off the caller's thread, and the caller receives only compact structs.
using Decoder = std::function<std::function<void()>(Result<Payload>)>;

// SoundCloud's api-v2 on top of INetworkTransport (so DNS, proxy, TLS, limits and the HTTP cache apply).
// Thread-safe. The closure returned by the decoder runs on `context`'s thread and is dropped if `context` is
// destroyed first; cancelling the handle delivers ErrorCode::Cancelled instead of the answer.
class IApiClient {
public:
    IApiClient() = default;
    virtual ~IApiClient() = default;
    IApiClient(const IApiClient&) = delete;
    IApiClient& operator=(const IApiClient&) = delete;
    IApiClient(IApiClient&&) = delete;
    IApiClient& operator=(IApiClient&&) = delete;

    virtual net::RequestHandle send(ApiRequest request, QObject* context, Decoder decode) = 0;
    // Full tracks for ids (e.g. a playlist's stubs). Lookups arriving within a few milliseconds of each other
    // are coalesced into one /tracks?ids= request per 50 distinct ids; unknown ids are left out of the result.
    virtual net::RequestHandle lookupTracks(std::vector<Id> ids, QObject* context,
                                            std::function<void(Result<std::vector<Track>>)> done) = 0;
};

// Sends an endpoint's request and delivers its parsed answer on `context`'s thread.
template <class T, class Done>
net::RequestHandle fetch(IApiClient& client, const Endpoint<T>& endpoint, QObject* context, Done done)
{
    return client.send(
        endpoint.request, context, [parse = endpoint.parse, done = std::move(done)](Result<Payload> payload) {
            Result<T> result = payload ? parse(payload->body) : std::unexpected(payload.error());
            return std::function<void()>([done, result = std::move(result)] mutable { done(std::move(result)); });
        });
}

// The api-v2 client. Identical GETs in flight share one request (per account); a 429 pauses every request
// until Retry-After has passed; a 401 on an anonymous request means the client_id expired (a new one is
// discovered and the request repeated once); a 401 with a token reports the session as rejected and, where
// the endpoint allows it, repeats the request anonymously. Lives on the API thread.
class ApiClient : public QObject, public IApiClient {
    Q_OBJECT

public:
    ApiClient(net::INetworkTransport& transport, Session& session, ClientIdProvider& clientIds,
              QObject* parent = nullptr);
    ~ApiClient() override;
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;
    ApiClient(ApiClient&&) = delete;
    ApiClient& operator=(ApiClient&&) = delete;

    net::RequestHandle send(ApiRequest request, QObject* context, Decoder decode) override;
    net::RequestHandle lookupTracks(std::vector<Id> ids, QObject* context,
                                    std::function<void(Result<std::vector<Track>>)> done) override;

    static constexpr int kMaxAttempts = 3;
    static constexpr std::chrono::seconds kDefaultPause{10};  // after a 429 without Retry-After
    static constexpr std::chrono::seconds kMaxWait{20};       // longer pauses fail with RateLimited
    static constexpr std::chrono::milliseconds kCoalesceWindow{10};

private:
    struct Subscriber;
    struct Flight;
    struct Lookup;
    struct Batch;

    void admit(ApiRequest request, const std::shared_ptr<Subscriber>& subscriber);
    void dispatch(const std::shared_ptr<Flight>& flight);
    void transmit(const std::shared_ptr<Flight>& flight);
    void onFinished(const std::shared_ptr<Flight>& flight, Result<net::Response> result);
    void complete(const std::shared_ptr<Flight>& flight, const Result<Payload>& result);
    static void finish(const std::shared_ptr<Subscriber>& subscriber, Result<Payload> result);
    void flushLookups();
    static void completeBatch(const Batch& batch);

    net::INetworkTransport& m_transport;
    Session& m_session;
    ClientIdProvider& m_clientIds;
    cache::MailboxPtr m_mailbox;
    QHash<QByteArray, std::shared_ptr<Flight>> m_flights;  // shared GETs in flight
    std::chrono::steady_clock::time_point m_pausedUntil;
    std::vector<std::shared_ptr<Lookup>> m_lookups;
    bool m_lookupScheduled = false;
};

}  // namespace noctara::api
