// The SoundCloud API layer: JSON parsing into domain structs, endpoints, client_id discovery, the client's
// deduplication, coalescing, rate-limit pause and 401 handling (against tests/mock/FakeApi.hpp), stream
// selection, and the collection model's pagination and hydration.
#include "TestUtil.hpp"
#include "api/ApiClient.hpp"
#include "api/ApiService.hpp"
#include "api/ClientId.hpp"
#include "api/Endpoints.hpp"
#include "api/Parse.hpp"
#include "api/Session.hpp"
#include "api/Snapshot.hpp"
#include "api/Streams.hpp"
#include "mock/FakeApi.hpp"

#include "models/CollectionModel.hpp"
#include "net/NetworkService.hpp"
#include "net/ProxyRouter.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QAbstractItemModelTester>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>

#include <chrono>
#include <memory>

using namespace noctara;
using namespace noctara::api;
using namespace std::chrono_literals;
using test::FakeApi;

namespace {

constexpr auto kClientId = "AbCdEfGhIjKlMnOpQrStUvWxYz012345";
constexpr auto kNewClientId = "ZyXwVuTsRqPoNmLkJiHgFeDcBa987654";

QByteArray trackJson(Id id, const QString& title = QStringLiteral("Night Drive"), const char* extra = "")
{
    return QStringLiteral(R"({"kind":"track","id":%1,"title":"%2","duration":215000,"full_duration":215000,
        "genre":"Synthwave","description":"Late night","created_at":"2021-03-04T05:06:07Z",
        "release_date":"2021-03-01T00:00:00Z","last_modified":"2023-01-02T03:04:05Z",
        "permalink_url":"https://soundcloud.com/artist/night-drive",
        "artwork_url":"https://i1.sndcdn.com/artworks-abc-large.jpg",
        "waveform_url":"https://wave.sndcdn.com/abc_m.json","playback_count":123456,"likes_count":789,
        "reposts_count":12,"comment_count":null,"policy":"ALLOW","streamable":true,
        "track_authorization":"auth-token-xyz",
        "publisher_metadata":{"artist":"The Artist","album_title":"Neon"},
        "user":{"id":42,"username":"uploader","permalink":"uploader","avatar_url":"https://i1.sndcdn.com/avatars-u-large.jpg","verified":true},
        "media":{"transcodings":[
          {"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:%1/aaa/stream/hls","preset":"aac_160k","duration":215000,"snipped":false,"format":{"protocol":"hls","mime_type":"audio/mp4; codecs=\"mp4a.40.2\""},"quality":"sq"},
          {"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:%1/bbb/stream/progressive","preset":"mp3_1_0","duration":215000,"snipped":false,"format":{"protocol":"progressive","mime_type":"audio/mpeg"},"quality":"sq"},
          {"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:%1/ccc/stream/hls","preset":"opus_0_0","duration":215000,"snipped":false,"format":{"protocol":"hls","mime_type":"audio/ogg; codecs=\"opus\""},"quality":"sq"}
        ]}%3})")
        .arg(id)
        .arg(title, QString::fromLatin1(extra))
        .toUtf8();
}

QByteArray page(const QList<QByteArray>& items, const QString& next = {})
{
    QByteArray json = "{\"collection\":[" + items.join(',') + "],\"next_href\":";
    json += next.isEmpty() ? QByteArray("null") : QByteArray('"' + next.toUtf8() + '"');
    return json + "}";
}

net::Headers jsonHeaders()
{
    return {{QByteArrayLiteral("content-type"), QByteArrayLiteral("application/json")}};
}

// An ApiClient on the test thread with a fixed client_id, unless discovery is under test.
struct Harness {
    FakeApi transport;
    Session session;
    ClientIdProvider clientIds{transport, {}};
    ApiClient client{transport, session, clientIds};
    QObject context;

    explicit Harness(bool fixedId = true)
    {
        if (fixedId) {
            clientIds.setOverride(QString::fromLatin1(kClientId));
        }
    }
};

template <class T>
std::shared_ptr<std::optional<Result<T>>> run(IApiClient& client, const api::Endpoint<T>& endpoint, QObject* context)
{
    auto result = std::make_shared<std::optional<Result<T>>>();
    fetch(client, endpoint, context, [result](Result<T> value) { *result = std::move(value); });
    return result;
}

template <class T> Result<T> await(const std::shared_ptr<std::optional<Result<T>>>& result, int timeoutMs = 3000)
{
    REQUIRE(test::waitUntil([&] { return result->has_value(); }, timeoutMs));
    return std::move(**result);
}

}  // namespace

TEST_SUITE("api.parse")
{
    TEST_CASE("a full track")
    {
        const Result<Track> track = parse::track(trackJson(7));
        REQUIRE_MESSAGE(track, test::describe(track));
        CHECK(track->id == 7);
        CHECK_FALSE(track->stub);
        CHECK(track->title == u"Night Drive");
        CHECK(track->artist() == u"The Artist");
        CHECK(track->publisherAlbum == u"Neon");
        CHECK(track->user.id == 42);
        CHECK(track->user.verified);
        CHECK(track->durationMs == 215000);
        CHECK(track->playbackCount == 123456);
        CHECK(track->commentCount == -1);  // null: hidden
        CHECK(track->createdAt == QDateTime(QDate(2021, 3, 4), QTime(5, 6, 7), QTimeZone::UTC));
        CHECK(track->releasedAt.date() == QDate(2021, 3, 1));
        CHECK(track->trackAuthorization == u"auth-token-xyz");
        REQUIRE(track->transcodings.size() == 3);
        CHECK(track->transcodings[1].protocol == cache::StreamProtocol::Progressive);
        CHECK(track->transcodings[1].cacheName() == u"progressive/mp3_1_0/sq");
        CHECK_FALSE(track->drm());
        CHECK_FALSE(track->previewOnly());
    }

    TEST_CASE("mixed-script titles survive")
    {
        const QString title = QStringLiteral("Ночь 夜 🌙 Drive");
        const Result<Track> track = parse::track(trackJson(1, title));
        REQUIRE(track);
        CHECK(track->title == title);
    }

    TEST_CASE("a stub, a missing id, broken JSON")
    {
        const Result<Track> stub = parse::track(R"({"kind":"track","id":99,"policy":"SNIP"})");
        REQUIRE(stub);
        CHECK(stub->stub);
        CHECK(stub->policy == Policy::Snip);
        CHECK(test::errorOf(parse::track(R"({"kind":"track","title":"x"})")) == ErrorCode::InvalidResponse);
        CHECK(test::errorOf(parse::track("{\"kind\":")) == ErrorCode::InvalidResponse);
        CHECK(test::errorOf(parse::track("[]")) == ErrorCode::InvalidResponse);
    }

    TEST_CASE("encrypted and snipped transcodings")
    {
        const QByteArray json = R"({"kind":"track","id":5,"title":"Go+","policy":"SNIP","media":{"transcodings":[
            {"url":"https://api-v2.soundcloud.com/media/x/ctr","preset":"aac_256k","format":{"protocol":"ctr-encrypted-hls","mime_type":"audio/mp4"},"quality":"hq"},
            {"url":"https://api-v2.soundcloud.com/media/x/prev","preset":"mp3_1_0","snipped":true,"format":{"protocol":"progressive","mime_type":"audio/mpeg"},"quality":"sq"}]}})";
        const Result<Track> track = parse::track(json);
        REQUIRE(track);
        CHECK(track->transcodings[0].encrypted);
        CHECK(track->drm());
        CHECK(track->previewOnly());
        const auto chosen = pickTranscoding(*track, StreamPreference::Best);
        REQUIRE(chosen);
        CHECK(chosen->snipped);  // only the preview can play natively
        CHECK_FALSE(chosen->encrypted);
    }

    TEST_CASE("a playlist with full tracks and stubs")
    {
        const QByteArray json = R"({"kind":"playlist","id":300,"title":"Mix","set_type":"album","track_count":3,
            "duration":600000,"artwork_url":null,"user":{"id":1,"username":"curator"},
            "tracks":[)" + trackJson(1) +
                                R"(,{"kind":"track","id":2},{"kind":"track","id":3}]})";
        const Result<Playlist> playlist = parse::playlist(json);
        REQUIRE_MESSAGE(playlist, test::describe(playlist));
        CHECK(playlist->album);
        CHECK(playlist->trackCount == 3);
        REQUIRE(playlist->tracks.size() == 3);
        CHECK_FALSE(playlist->tracks[0].stub);
        CHECK(playlist->tracks[1].stub);
        CHECK(playlist->artwork() == playlist->tracks[0].artworkUrl);  // falls back to the first track's
    }

    TEST_CASE("collections unwrap likes, history, feed and charts and skip unknown kinds")
    {
        const QByteArray json = page(
            {
                R"({"created_at":"2022-01-01T00:00:00Z","kind":"like","track":)" + trackJson(1) + "}",
                R"({"played_at":1650000000000,"track_id":2,"track":)" + trackJson(2) + "}",
                R"({"type":"track-repost","created_at":"2022-02-02T00:00:00Z","user":{"id":9,"username":"friend"},"track":)" +
                    trackJson(3) + "}",
                R"({"track":)" + trackJson(4) + R"(,"score":12.5})",
                R"({"kind":"user","id":5,"username":"someone","followers_count":10})",
                R"({"kind":"system-playlist","id":"soundcloud:system-playlists:x","title":"Daily"})",
                R"({"kind":"brand-new-thing","id":6})",
            },
            QStringLiteral("https://api-v2.soundcloud.com/x?offset=7"));
        const Result<Page<Entry>> parsed = parse::collection(json);
        REQUIRE_MESSAGE(parsed, test::describe(parsed));
        REQUIRE(parsed->items.size() == 5);
        CHECK(parsed->next == u"https://api-v2.soundcloud.com/x?offset=7");
        CHECK(parsed->items[0].at.date() == QDate(2022, 1, 1));
        CHECK(parsed->items[1].at == QDateTime::fromMSecsSinceEpoch(1650000000000, QTimeZone::UTC));
        REQUIRE(parsed->items[2].repostedBy);
        CHECK(parsed->items[2].repostedBy->username == u"friend");
        CHECK(itemId(parsed->items[3].item) == 4);
        CHECK(std::get<User>(parsed->items[4].item).followersCount == 10);
    }

    TEST_CASE("a bare array and the end of a collection")
    {
        const Result<std::vector<Track>> tracks = parse::tracks("[" + trackJson(1) + "," + trackJson(2) + "]");
        REQUIRE(tracks);
        CHECK(tracks->size() == 2);
        const Result<Page<Entry>> last = parse::collection(page({trackJson(1)}));
        REQUIRE(last);
        CHECK(last->next.isEmpty());
        CHECK(test::errorOf(parse::collection(R"({"items":[]})")) == ErrorCode::InvalidResponse);
    }

    TEST_CASE("waveform and stream URL")
    {
        const Result<Waveform> wave = parse::waveform(R"({"width":4,"height":140,"samples":[0,70,140,35]})");
        REQUIRE(wave);
        REQUIRE(wave->samples.size() == 4);
        CHECK(wave->samples[2] == doctest::Approx(1.0));
        CHECK(wave->samples[1] == doctest::Approx(0.5));
        CHECK(parse::streamUrl(R"({"url":"https://cf-media.sndcdn.com/a.mp3?Policy=x"})").value().host() ==
              u"cf-media.sndcdn.com");
        CHECK(test::errorOf(parse::streamUrl(R"({"url":"http://plain.example/a.mp3"})")) == ErrorCode::InvalidResponse);
        const Result<ProtectedStream> secured = parse::protectedStream(
            R"({"url":"https://playback.media-streaming.soundcloud.cloud/cenc/x","licenseAuthToken":"tok"})");
        REQUIRE(secured);
        CHECK(secured->licenseToken == u"tok");
        CHECK(test::errorOf(parse::protectedStream(R"({"url":"https://playback.example/x"})")) ==
              ErrorCode::InvalidResponse);
    }

    TEST_CASE("artwork sizes")
    {
        const QString large = QStringLiteral("https://i1.sndcdn.com/artworks-000-abc-large.jpg");
        CHECK(sizedArtwork(large, 60) == u"https://i1.sndcdn.com/artworks-000-abc-t67x67.jpg");
        CHECK(sizedArtwork(large, 300) == u"https://i1.sndcdn.com/artworks-000-abc-t300x300.jpg");
        CHECK(sizedArtwork(large, 2000) == u"https://i1.sndcdn.com/artworks-000-abc-t500x500.jpg");
        const QString odd = QStringLiteral("https://example.com/cover.jpg");
        CHECK(sizedArtwork(odd, 300) == odd);
        CHECK(sizedArtwork({}, 300).isEmpty());
    }
}

TEST_SUITE("api.endpoints")
{
    TEST_CASE("queries are encoded and only API hosts are trusted")
    {
        const auto search = endpoints::search(QStringLiteral("drum & bass + ночь"), endpoints::SearchKind::Tracks, 25);
        CHECK(search.request.url.path() == u"/search/tracks");
        CHECK(QUrlQuery(search.request.url).queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded) ==
              u"drum & bass + ночь");
        CHECK(QUrlQuery(search.request.url).queryItemValue(QStringLiteral("limit")) == u"25");
        CHECK(isApiHost(search.request.url));
        CHECK_FALSE(isApiHost(QUrl(QStringLiteral("http://api-v2.soundcloud.com/x"))));
        CHECK_FALSE(isApiHost(QUrl(QStringLiteral("https://api-v2.soundcloud.com.evil.test/x"))));

        const auto next = endpoints::next(search, QStringLiteral("https://evil.test/steal?offset=2"));
        CHECK(next.request.url.isEmpty());
        CHECK(endpoints::stream(10).request.auth == Auth::Required);
        CHECK(endpoints::waveform(QStringLiteral("https://wave.sndcdn.com/a.json")).request.auth == Auth::None);
        CHECK(endpoints::streamUrl({}, {}).request.cache == net::CacheMode::Bypass);
    }
}

TEST_SUITE("api.clientid")
{
    TEST_CASE("found in hydration data, assignments and URLs")
    {
        CHECK(
            clientid::find(
                R"(window.__sc_hydration = [{"hydratable":"apiClient","data":{"id":"AbCdEfGhIjKlMnOpQrStUvWxYz012345","isExpiring":false}}];)") ==
            QString::fromLatin1(kClientId));
        CHECK(clientid::find(R"(...,{client_id:"AbCdEfGhIjKlMnOpQrStUvWxYz012345",env:"production"})") ==
              QString::fromLatin1(kClientId));
        CHECK(clientid::find(R"(e.client_id = 'AbCdEfGhIjKlMnOpQrStUvWxYz012345';)") == QString::fromLatin1(kClientId));
        CHECK(clientid::find("?client_id=AbCdEfGhIjKlMnOpQrStUvWxYz012345&app_version=1") ==
              QString::fromLatin1(kClientId));
        CHECK_FALSE(clientid::find("client_id:e,client_id=t.id,client_id:\"short\""));
    }

    TEST_CASE("only the asset CDN's scripts, in page order")
    {
        const QByteArray html = R"(<html><script crossorigin src="https://a-v2.sndcdn.com/assets/0-abc.js"></script>
            <script src="https://evil.test/x.js"></script><script src="/relative.js"></script>
            <script crossorigin src="https://a-v2.sndcdn.com/assets/49-def.js"></script></html>)";
        const auto scripts = clientid::scripts(html, QUrl(QStringLiteral("https://soundcloud.com/")));
        REQUIRE(scripts.size() == 2);
        CHECK(scripts[1].path() == u"/assets/49-def.js");
    }

    TEST_CASE("discovered from the last scripts first, stored, and reloaded")
    {
        QTemporaryDir dir;
        const QString store = dir.filePath(QStringLiteral("soundcloud.json"));
        FakeApi transport;
        transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            const QString url = request.url.toString();
            if (url == u"https://soundcloud.com/") {
                return FakeApi::reply(200, R"(<script src="https://a-v2.sndcdn.com/assets/1.js"></script>
                                              <script src="https://a-v2.sndcdn.com/assets/2.js"></script>
                                              <script src="https://a-v2.sndcdn.com/assets/3.js"></script>)");
            }
            if (url.endsWith(u"/2.js")) {
                return FakeApi::reply(200, R"(x={client_id:"AbCdEfGhIjKlMnOpQrStUvWxYz012345"})");
            }
            return FakeApi::reply(200, "var nothing;");
        });
        {
            ClientIdProvider provider(transport, store);
            std::optional<Result<QString>> found;
            provider.acquire([&](Result<QString> id) { found = std::move(id); });
            REQUIRE(test::waitUntil([&] { return found.has_value(); }));
            REQUIRE_MESSAGE(*found, test::describe(*found));
            CHECK(**found == QString::fromLatin1(kClientId));
            CHECK(provider.source() == ClientIdProvider::Source::Discovered);
            CHECK(transport.count() == 3);  // the page, then 3.js, then 2.js
            CHECK(transport.requests()[1].url.path() == u"/assets/3.js");
        }
        transport.reset();
        ClientIdProvider reloaded(transport, store);
        CHECK(reloaded.current() == QString::fromLatin1(kClientId));
        CHECK(reloaded.source() == ClientIdProvider::Source::Stored);
        CHECK(transport.count() == 0);
    }

    TEST_CASE("a failed discovery is not repeated right away")
    {
        FakeApi transport;
        transport.setHandler([](const net::Request&) -> Result<net::Response> {
            return FakeApi::reply(200, "<html>no scripts</html>");
        });
        ClientIdProvider provider(transport, {});
        std::optional<Result<QString>> first;
        provider.acquire([&](Result<QString> id) { first = std::move(id); });
        REQUIRE(test::waitUntil([&] { return first.has_value(); }));
        CHECK(test::errorOf(*first) == ErrorCode::ClientIdUnavailable);
        std::optional<Result<QString>> second;
        provider.acquire([&](Result<QString> id) { second = std::move(id); });
        REQUIRE(second.has_value());  // answered at once from the throttle
        CHECK(test::errorOf(*second) == ErrorCode::ClientIdUnavailable);
        CHECK(transport.count() == 1);
    }
}

TEST_SUITE("api.client")
{
    TEST_CASE("client_id and token are attached; identical requests share one transfer")
    {
        Harness h;
        h.session.setToken("secret-token");
        h.transport.setHandler([](const net::Request&) -> Result<net::Response> {
            return FakeApi::reply(200, trackJson(7), jsonHeaders());
        });
        const auto first = run(h.client, endpoints::track(7), &h.context);
        const auto second = run(h.client, endpoints::track(7), &h.context);
        CHECK(await(first)->id == 7);
        CHECK(await(second)->id == 7);
        REQUIRE(h.transport.count() == 1);
        const net::Request sent = h.transport.requests().front();
        CHECK(FakeApi::query(sent, QStringLiteral("client_id")) == QString::fromLatin1(kClientId));
        CHECK(FakeApi::header(sent, "authorization") == "OAuth secret-token");
        CHECK(sent.cacheTtl > 0s);
    }

    TEST_CASE("the token never leaves the API hosts")
    {
        Harness h;
        h.session.setToken("secret-token");
        h.transport.setHandler(
            [](const net::Request&) -> Result<net::Response> { return FakeApi::reply(200, R"({"samples":[1,2]})"); });
        CHECK(
            await(run(h.client, endpoints::waveform(QStringLiteral("https://wave.sndcdn.com/a_m.json")), &h.context)));
        const net::Request sent = h.transport.requests().front();
        CHECK(FakeApi::header(sent, "authorization").isEmpty());
        CHECK(FakeApi::query(sent, QStringLiteral("client_id")).isEmpty());

        auto foreign = endpoints::track(1);
        foreign.request.url = QUrl(QStringLiteral("https://evil.test/tracks/1"));
        CHECK(test::errorOf(await(run(h.client, foreign, &h.context))) == ErrorCode::InvalidArgument);
        CHECK(h.transport.count() == 1);
    }

    TEST_CASE("signed-in endpoints fail early without a session")
    {
        Harness h;
        CHECK(test::errorOf(await(run(h.client, endpoints::stream(10), &h.context))) == ErrorCode::SignInRequired);
        CHECK(h.transport.count() == 0);
    }

    TEST_CASE("an expired client_id is replaced and the request repeated")
    {
        Harness h(false);
        h.clientIds.setOverride({});
        int pages = 0;
        h.transport.setHandler([&](const net::Request& request) -> Result<net::Response> {
            if (request.url.host() == u"soundcloud.com") {
                ++pages;
                const char* id = pages == 1 ? kClientId : kNewClientId;
                return FakeApi::reply(200, QByteArray(R"({"hydratable":"apiClient","data":{"id":")") + id + "\"}}");
            }
            if (FakeApi::query(request, QStringLiteral("client_id")) == QString::fromLatin1(kClientId)) {
                return FakeApi::reply(401, R"({"error":"invalid client"})");
            }
            return FakeApi::reply(200, trackJson(3));
        });
        const Result<Track> track = await(run(h.client, endpoints::track(3), &h.context));
        REQUIRE_MESSAGE(track, test::describe(track));
        CHECK(pages == 2);
        CHECK(h.clientIds.current() == QString::fromLatin1(kNewClientId));
    }

    TEST_CASE("a refused token is reported once and public endpoints continue anonymously")
    {
        Harness h;
        h.session.setToken("stale-token");
        QSignalSpy rejected(&h.session, &Session::rejected);
        h.transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            if (!FakeApi::header(request, "authorization").isEmpty()) {
                return FakeApi::reply(401, "{}");
            }
            return FakeApi::reply(200, trackJson(4));
        });
        CHECK(await(run(h.client, endpoints::track(4), &h.context)));
        CHECK(rejected.count() == 1);
        CHECK(test::errorOf(await(run(h.client, endpoints::me(), &h.context))) == ErrorCode::Unauthorized);
        CHECK(rejected.count() == 1);  // the same token is reported only once
    }

    TEST_CASE("status codes map to error codes")
    {
        Harness h;
        int status = 404;
        h.transport.setHandler(
            [&](const net::Request&) -> Result<net::Response> { return FakeApi::reply(status, "{}"); });
        CHECK(test::errorOf(await(run(h.client, endpoints::track(1), &h.context))) == ErrorCode::NotFound);
        status = 403;
        CHECK(test::errorOf(await(run(h.client, endpoints::track(2), &h.context))) == ErrorCode::Forbidden);
        status = 500;
        CHECK(test::errorOf(await(run(h.client, endpoints::track(3), &h.context))) == ErrorCode::HttpError);
        status = 200;  // but not JSON
        CHECK(test::errorOf(await(run(h.client, endpoints::track(4), &h.context))) == ErrorCode::InvalidResponse);
    }

    TEST_CASE("a 429 pauses every request until Retry-After has passed")
    {
        Harness h;
        int limited = 1;
        QElapsedTimer clock;
        qint64 secondSent = -1;
        h.transport.setHandler([&](const net::Request& request) -> Result<net::Response> {
            if (request.url.path().endsWith(u"/tracks/2")) {
                secondSent = clock.elapsed();
            }
            if (limited-- > 0) {
                return FakeApi::reply(429, "{}", {{QByteArrayLiteral("retry-after"), QByteArrayLiteral("1")}});
            }
            return FakeApi::reply(200, trackJson(1));
        });
        clock.start();
        const auto first = run(h.client, endpoints::track(1), &h.context);
        REQUIRE(test::waitUntil([&] { return h.transport.count() == 1; }));
        QCoreApplication::processEvents();                                   // the 429 is handled
        const auto second = run(h.client, endpoints::track(2), &h.context);  // arrives during the pause
        QCoreApplication::processEvents();
        CHECK(h.transport.count() == 1);  // held back, not sent
        CHECK(await(first));
        CHECK(await(second));
        CHECK(secondSent >= 900);
        CHECK(h.transport.count() == 3);
    }

    TEST_CASE("a pause longer than the client waits fails with RateLimited")
    {
        Harness h;
        h.transport.setHandler([](const net::Request&) -> Result<net::Response> {
            return FakeApi::reply(429, "{}", {{QByteArrayLiteral("retry-after"), QByteArrayLiteral("3600")}});
        });
        CHECK(test::errorOf(await(run(h.client, endpoints::track(1), &h.context))) == ErrorCode::RateLimited);
        CHECK(test::errorOf(await(run(h.client, endpoints::track(2), &h.context))) == ErrorCode::RateLimited);
        CHECK(h.transport.count() == 1);  // the second never left
    }

    TEST_CASE("track lookups are coalesced and batched by 50")
    {
        Harness h;
        h.transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            QList<QByteArray> tracks;
            for (const QString& id : FakeApi::query(request, QStringLiteral("ids")).split(u',')) {
                if (id.toLongLong() % 10 != 0) {  // multiples of ten were deleted
                    tracks.append(trackJson(id.toLongLong()));
                }
            }
            return FakeApi::reply(200, "[" + tracks.join(',') + "]");
        });
        std::vector<std::optional<Result<std::vector<Track>>>> results(3);
        std::vector<Id> many;
        for (Id id = 1; id <= 60; ++id) {
            many.push_back(id);
        }
        h.client.lookupTracks({1, 2, 3}, &h.context, [&](auto tracks) { results[0] = std::move(tracks); });
        h.client.lookupTracks({3, 4, 10}, &h.context, [&](auto tracks) { results[1] = std::move(tracks); });
        h.client.lookupTracks(many, &h.context, [&](auto tracks) { results[2] = std::move(tracks); });
        REQUIRE(test::waitUntil([&] { return std::ranges::all_of(results, [](auto& r) { return r.has_value(); }); }));
        CHECK(h.transport.count() == 2);  // 60 distinct ids
        REQUIRE(*results[0]);
        CHECK((*results[0])->size() == 3);
        REQUIRE(*results[1]);
        REQUIRE((*results[1])->size() == 2);  // 10 is gone
        CHECK((**results[1])[1].id == 4);
        CHECK((*results[2])->size() == 54);
    }

    TEST_CASE("cancelled and orphaned requests deliver nothing")
    {
        Harness h;
        h.transport.setHandler(
            [](const net::Request&) -> Result<net::Response> { return FakeApi::reply(200, trackJson(1)); });
        const auto cancelled = std::make_shared<std::optional<Result<Track>>>();
        fetch(h.client, endpoints::track(1), &h.context, [cancelled](Result<Track> r) {
            *cancelled = std::move(r);
        }).cancel();
        REQUIRE(test::waitUntil([&] { return cancelled->has_value(); }));
        CHECK(test::errorOf(**cancelled) == ErrorCode::Cancelled);
        CHECK(h.transport.count() == 0);  // never sent

        bool called = false;
        {
            QObject shortLived;
            fetch(h.client, endpoints::track(2), &shortLived, [&](Result<Track>) { called = true; });
        }
        test::waitUntil([] { return false; }, 100);
        CHECK_FALSE(called);
    }

    TEST_CASE("a request cancelled while the client_id is discovered is not sent")
    {
        Harness h(false);
        const auto handle = std::make_shared<net::RequestHandle>();
        h.transport.setHandler([handle](const net::Request& request) -> Result<net::Response> {
            if (request.url.host() == u"soundcloud.com") {
                handle->cancel();  // the caller gives up while discovery is still running
                return FakeApi::reply(200,
                                      QByteArray(R"({"hydratable":"apiClient","data":{"id":")") + kClientId + "\"}}");
            }
            return FakeApi::reply(200, trackJson(1));
        });
        const auto result = std::make_shared<std::optional<Result<Track>>>();
        *handle =
            fetch(h.client, endpoints::track(1), &h.context, [result](Result<Track> r) { *result = std::move(r); });
        REQUIRE(test::waitUntil([&] { return result->has_value(); }));
        CHECK(test::errorOf(**result) == ErrorCode::Cancelled);
        CHECK(h.clientIds.current() == QString::fromLatin1(kClientId));
        CHECK(h.transport.count() == 1);  // only the page; the track request nobody wants is not sent
    }

    TEST_CASE("the service parses off the caller's thread and follows the client_id setting")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::ClientId, QString::fromLatin1(kClientId)));
        FakeApi transport;
        transport.setHandler(
            [](const net::Request&) -> Result<net::Response> { return FakeApi::reply(200, trackJson(1)); });
        Session session;
        ApiService service(store, transport, session, {});
        QObject context;
        QThread* decodedOn = nullptr;
        std::optional<Result<Payload>> got;
        service.client().send(endpoints::track(1).request, &context, [&](Result<Payload> payload) {
            decodedOn = QThread::currentThread();
            return std::function<void()>([&got, payload] { got = payload; });
        });
        REQUIRE(test::waitUntil([&] { return got.has_value(); }));
        CHECK(decodedOn != QThread::currentThread());
        CHECK(FakeApi::query(transport.requests().front(), QStringLiteral("client_id")) ==
              QString::fromLatin1(kClientId));
        CHECK(service.pageSize() == 50);
    }
}

TEST_SUITE("api.streams")
{
    TEST_CASE("the best plain transcoding, progressive breaking ties")
    {
        Result<Track> track = parse::track(trackJson(1));
        REQUIRE(track);
        const auto best = pickTranscoding(*track, StreamPreference::Best);
        REQUIRE(best);
        CHECK(best->preset == u"aac_160k");
        const auto saver = pickTranscoding(*track, StreamPreference::DataSaver);
        REQUIRE(saver);
        CHECK(saver->preset == u"opus_0_0");
        track->transcodings.erase(track->transcodings.begin());  // without aac: mp3 progressive vs opus
        CHECK(pickTranscoding(*track, StreamPreference::Best)->protocol == cache::StreamProtocol::Progressive);
    }

    TEST_CASE("protected playback takes the Widevine (ctr) transcoding, never cbc, abr or a preview")
    {
        Track track;
        const auto add = [&track](const char* protocol, const char* preset, bool encrypted, bool snipped = false) {
            Transcoding t;
            t.url = QStringLiteral("https://api-v2.soundcloud.com/media/x/%1").arg(QString::fromLatin1(preset));
            t.protocolName = QString::fromLatin1(protocol);
            t.preset = QString::fromLatin1(preset);
            t.encrypted = encrypted;
            t.snipped = snipped;
            track.transcodings.push_back(t);
        };
        add("cbc-encrypted-hls", "aac_160k", true);
        add("ctr-encrypted-hls", "abr_sq", true);
        add("ctr-encrypted-hls", "aac_96k", true);
        add("ctr-encrypted-hls", "aac_160k", true);
        add("hls", "mp3_0_0", false, true);
        CHECK(track.drm());
        CHECK_FALSE(pickTranscoding(track, StreamPreference::Best)->encrypted);  // natively: the preview only
        const auto best = pickProtectedTranscoding(track, StreamPreference::Best);
        REQUIRE(best);
        CHECK(best->protocolName == u"ctr-encrypted-hls");
        CHECK(best->preset == u"aac_160k");
        CHECK(pickProtectedTranscoding(track, StreamPreference::DataSaver)->preset == u"aac_96k");
        const auto endpoint = endpoints::protectedStream(*best, QStringLiteral("auth"));
        CHECK(endpoint.request.cache == net::CacheMode::Bypass);
        CHECK(endpoint.request.url.query().contains(u"track_authorization=auth"));
        CHECK(endpoints::widevineLicense().host() == u"license.media-streaming.soundcloud.cloud");
    }

    TEST_CASE("the cache record carries the API metadata")
    {
        const Result<Track> track = parse::track(trackJson(77));
        REQUIRE(track);
        const cache::TrackInfo info = toTrackInfo(*track, track->transcodings[1], QStringLiteral("My mix"));
        CHECK(info.key.trackId == u"77");
        CHECK(info.key.preset == u"progressive/mp3_1_0/sq");
        CHECK_FALSE(info.drm);
        CHECK(info.artist == u"The Artist");
        CHECK(info.uploader == u"uploader");
        CHECK(info.album == u"Neon");
        CHECK(info.context == u"My mix");
        CHECK(info.codec == u"mp3");
        CHECK(info.artworkUrl.path().endsWith(u"-t500x500.jpg"));
        CHECK(info.artworkThumbUrl.path().endsWith(u"-t67x67.jpg"));
        CHECK(info.sourceUpdatedAt == u"2023-01-02T03:04:05Z");
    }
}

TEST_SUITE("models.collection")
{
    TEST_CASE("pages load on demand, repeats are dropped, errors stop paging")
    {
        Harness h;
        h.transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            const QString offset = FakeApi::query(request, QStringLiteral("offset"));
            if (offset.isEmpty()) {
                return FakeApi::reply(200,
                                      page({trackJson(1), trackJson(2)},
                                           QStringLiteral("https://api-v2.soundcloud.com/search/tracks?q=x&offset=2")));
            }
            if (offset == u"2") {
                return FakeApi::reply(200,
                                      page({trackJson(2), trackJson(3)},
                                           QStringLiteral("https://api-v2.soundcloud.com/search/tracks?q=x&offset=4")));
            }
            return FakeApi::reply(503, "{}");
        });
        models::CollectionModel model(h.client);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
        model.setSource(
            [](int limit) { return endpoints::search(QStringLiteral("x"), endpoints::SearchKind::Tracks, limit); }, 20);
        CHECK(model.loading());
        REQUIRE(test::waitUntil([&] { return !model.loading(); }));
        CHECK(model.count() == 2);
        CHECK(model.canFetchMore({}));
        CHECK(FakeApi::query(h.transport.requests().front(), QStringLiteral("limit")) == u"20");

        model.fetchMore({});
        REQUIRE(test::waitUntil([&] { return !model.loading(); }));
        CHECK(model.count() == 3);  // track 2 was on both pages
        CHECK(model.data(model.index(2), models::CollectionModel::TitleRole).toString() == u"Night Drive");
        CHECK(model.data(model.index(2), models::CollectionModel::KindRole).toString() == u"track");

        model.fetchMore({});
        REQUIRE(test::waitUntil([&] { return !model.loading(); }));
        CHECK(model.errorCode() == static_cast<int>(ErrorCode::HttpError));
        CHECK_FALSE(model.canFetchMore({}));
        CHECK(model.count() == 3);
    }

    TEST_CASE("stub tracks are hydrated in one request, and deleted ones removed")
    {
        Harness h;
        h.transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            CHECK(request.url.path() == u"/tracks");
            return FakeApi::reply(200, "[" + trackJson(2, QStringLiteral("Two")) + "]");  // 3 is gone
        });
        const Result<Playlist> playlist =
            parse::playlist(R"({"kind":"playlist","id":1,"title":"P","tracks":[)" + trackJson(1) +
                            R"(,{"kind":"track","id":2},{"kind":"track","id":3}]})");
        REQUIRE(playlist);
        std::vector<Entry> entries;
        for (const Track& track : playlist->tracks) {
            entries.push_back({.item = track, .at = {}, .repostedBy = {}});
        }
        models::CollectionModel model(h.client);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
        model.setEntries(std::move(entries));
        CHECK(model.data(model.index(1), models::CollectionModel::PendingRole).toBool());
        (void)model.data(model.index(1), models::CollectionModel::TitleRole);  // a view asks for both stubs
        (void)model.data(model.index(2), models::CollectionModel::TitleRole);
        REQUIRE(test::waitUntil([&] { return model.count() == 2; }));
        CHECK(h.transport.count() == 1);
        CHECK(FakeApi::query(h.transport.requests().front(), QStringLiteral("ids")) == u"2,3");
        CHECK(model.data(model.index(1), models::CollectionModel::TitleRole).toString() == u"Two");
        CHECK_FALSE(model.data(model.index(1), models::CollectionModel::PendingRole).toBool());
    }

    TEST_CASE("a reload whose first page is empty reports the list as changed")
    {
        Harness h;
        bool empty = false;
        h.transport.setHandler([&](const net::Request&) -> Result<net::Response> {
            return FakeApi::reply(200, empty ? page({}) : page({trackJson(1), trackJson(2)}));
        });
        models::CollectionModel model(h.client);
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Warning);
        model.setSource(
            [](int limit) { return endpoints::search(QStringLiteral("x"), endpoints::SearchKind::Tracks, limit); }, 20);
        REQUIRE(test::waitUntil([&] { return !model.loading() && model.count() == 2; }));
        QSignalSpy changed(&model, &models::CollectionModel::listChanged);
        empty = true;
        model.reload();
        REQUIRE(test::waitUntil([&] { return !model.loading(); }));
        CHECK(model.count() == 0);
        CHECK(changed.count() == 1);  // the rows are gone, like after clear()
    }
}

// Talks to the real SoundCloud. Skipped by default: run with --no-skip -tc="real api*".
TEST_CASE("real api: client_id discovery, search, resolve, stream URL" * doctest::skip())
{
    settings::SettingsStore store(settings::Schema::builtin());
    net::NetworkService network(store, net::processEnvironment(), false);
    Session session;
    ApiService service(store, network.transport(), session, {});
    QObject context;

    const Result<Page<Entry>> found = await(
        run(service.client(), endpoints::search(QStringLiteral("lofi"), endpoints::SearchKind::Tracks, 10), &context),
        30000);
    REQUIRE_MESSAGE(found, test::describe(found));
    REQUIRE_FALSE(found->items.empty());
    MESSAGE("search returned " << found->items.size() << " tracks, next: " << found->next);

    const auto* track = std::get_if<Track>(&found->items.front().item);
    REQUIRE(track != nullptr);
    const Result<Item> resolved =
        await(run(service.client(), endpoints::resolve(QUrl(track->permalinkUrl)), &context), 30000);
    REQUIRE_MESSAGE(resolved, test::describe(resolved));
    CHECK(itemId(*resolved) == track->id);

    for (const auto& entry : found->items) {
        const auto& candidate = std::get<Track>(entry.item);
        if (const auto transcoding = pickTranscoding(candidate, StreamPreference::Best)) {
            const Result<QUrl> url =
                await(run(service.client(), endpoints::streamUrl(*transcoding, candidate.trackAuthorization), &context),
                      30000);
            REQUIRE_MESSAGE(url, test::describe(url));
            MESSAGE("stream " << transcoding->cacheName() << " on " << url->host());
            break;
        }
    }
}

// JSON parsing cost of a 50-track page: simdjson into structs vs. QJsonDocument (tree only). Skipped by
// default: run a release build with --no-skip -tc="api parse benchmark".
TEST_CASE("api parse benchmark" * doctest::skip())
{
    QList<QByteArray> items;
    for (Id id = 1; id <= 50; ++id) {
        items.append(trackJson(id));
    }
    const QByteArray json = page(items, QStringLiteral("https://api-v2.soundcloud.com/x?offset=50"));
    constexpr int kRounds = 2000;
    QElapsedTimer clock;
    clock.start();
    std::size_t parsed = 0;
    for (int i = 0; i < kRounds; ++i) {
        parsed += parse::collection(json)->items.size();
    }
    const double simd = static_cast<double>(clock.nsecsElapsed()) / kRounds / 1000.0;
    clock.restart();
    for (int i = 0; i < kRounds; ++i) {
        parsed +=
            static_cast<std::size_t>(QJsonDocument::fromJson(json).object().value(u"collection").toArray().size());
    }
    const double qt = static_cast<double>(clock.nsecsElapsed()) / kRounds / 1000.0;
    MESSAGE(json.size() << " bytes: simdjson → structs " << simd << " µs, QJsonDocument (tree only) " << qt << " µs ("
                        << parsed << ")");
}

namespace {

QByteArray playlistJson(Id id, const QList<QByteArray>& tracks, const char* artwork = nullptr)
{
    QByteArray json = "{\"kind\":\"playlist\",\"id\":" + QByteArray::number(id) +
                      ",\"title\":\"Mix\",\"track_count\":" + QByteArray::number(tracks.size() + 1) +
                      ",\"last_modified\":\"2024-01-01T00:00:00Z\",\"user\":{\"id\":42,\"username\":\"me\","
                      "\"avatar_url\":\"https://i1.sndcdn.com/avatars-me-large.jpg\"}";
    if (artwork != nullptr) {
        json += ",\"artwork_url\":\"" + QByteArray(artwork) + "\"";
    }
    if (!tracks.isEmpty()) {
        json += ",\"tracks\":[" + tracks.join(',') + "]";
    }
    return json + "}";
}

std::vector<Id> idsOf(const models::CollectionModel& model)
{
    std::vector<Id> ids;
    for (const Entry& entry : model.entries()) {
        ids.push_back(itemId(entry.item));
    }
    return ids;
}

// Answers a paged list from `pages` (offset → ids), each page linking the next.
FakeApi::Handler pagedTracks(std::shared_ptr<std::vector<std::vector<Id>>> pages)
{
    return [pages](const net::Request& request) -> Result<net::Response> {
        const int index = FakeApi::query(request, QStringLiteral("offset")).toInt();
        if (index >= std::ssize(*pages)) {
            return FakeApi::reply(404, {});
        }
        QList<QByteArray> items;
        for (const Id id : (*pages)[static_cast<std::size_t>(index)]) {
            items.append(trackJson(id, QStringLiteral("T%1").arg(id)));
        }
        const QString next =
            index + 1 < std::ssize(*pages)
                ? QStringLiteral("https://api-v2.soundcloud.com/users/42/track_likes?offset=%1").arg(index + 1)
                : QString();
        return FakeApi::reply(200, page(items, next));
    };
}

}  // namespace

TEST_SUITE("api.snapshot")
{
    TEST_CASE("a snapshot round-trips tracks, stubs, playlists, users and reposts; tracks come back stale")
    {
        Snapshot saved;
        Result<Track> full = parse::track(trackJson(1));
        REQUIRE(full);
        saved.entries.push_back({.item = *full,
                                 .at = QDateTime(QDate(2024, 5, 6), QTime(7, 8, 9), QTimeZone::UTC),
                                 .repostedBy = std::nullopt});
        Track stub;
        stub.id = 2;
        stub.stub = true;
        saved.entries.push_back({.item = stub, .at = {}, .repostedBy = std::nullopt});
        Result<Playlist> playlist = parse::playlist(playlistJson(10, {trackJson(3), "{\"id\":4,\"kind\":\"track\"}"}));
        REQUIRE(playlist);
        saved.entries.push_back({.item = *playlist, .at = {}, .repostedBy = std::nullopt});
        User user;
        user.id = 20;
        user.username = QStringLiteral("someone");
        user.city = QStringLiteral("Tbilisi");
        user.followersCount = 12;
        saved.entries.push_back({.item = user, .at = {}, .repostedBy = std::nullopt});
        UserRef reposter;
        reposter.id = 30;
        reposter.username = QStringLiteral("reposter");
        saved.entries.push_back({.item = *full, .at = {}, .repostedBy = reposter});
        saved.tag = QStringLiteral("2024-01-01T00:00:00Z");
        saved.complete = false;

        const Result<Snapshot> loaded = decodeSnapshot(encodeSnapshot(saved));
        REQUIRE_MESSAGE(loaded, test::describe(loaded));
        CHECK(loaded->tag == saved.tag);
        CHECK_FALSE(loaded->complete);
        REQUIRE(loaded->entries.size() == 5);
        const auto& track = std::get<Track>(loaded->entries[0].item);
        CHECK(track.stale);
        CHECK(track.title == full->title);
        CHECK(track.artist() == full->artist());
        CHECK(track.trackAuthorization == full->trackAuthorization);
        CHECK(track.createdAt == full->createdAt);
        CHECK(track.likesCount == full->likesCount);
        CHECK(track.commentCount == full->commentCount);
        REQUIRE(track.transcodings.size() == full->transcodings.size());
        CHECK(track.transcodings[1].url == full->transcodings[1].url);
        CHECK(track.transcodings[1].protocol == full->transcodings[1].protocol);
        CHECK(track.transcodings[0].mimeType == full->transcodings[0].mimeType);
        CHECK(loaded->entries[0].at == saved.entries[0].at);
        const auto& reloadedStub = std::get<Track>(loaded->entries[1].item);
        CHECK(reloadedStub.stub);
        CHECK_FALSE(reloadedStub.stale);
        const auto& reloadedPlaylist = std::get<Playlist>(loaded->entries[2].item);
        CHECK(reloadedPlaylist.lastModified == playlist->lastModified);
        CHECK(reloadedPlaylist.trackCount == playlist->trackCount);
        REQUIRE(reloadedPlaylist.tracks.size() == 2);
        CHECK(reloadedPlaylist.tracks[0].stale);
        CHECK(reloadedPlaylist.tracks[1].stub);
        const auto& reloadedUser = std::get<User>(loaded->entries[3].item);
        CHECK(reloadedUser.city == u"Tbilisi");
        CHECK(reloadedUser.followersCount == 12);
        CHECK(reloadedUser.trackCount == -1);
        REQUIRE(loaded->entries[4].repostedBy.has_value());
        CHECK(loaded->entries[4].repostedBy->username == u"reposter");
    }

    TEST_CASE("the store keeps lists per account, ignores damaged files and forgets on sign-out")
    {
        QTemporaryDir dir;
        SnapshotStore store(dir.path());
        QObject context;
        std::optional<std::optional<Snapshot>> got;
        const auto load = [&](const QString& key) {
            got.reset();
            store.load(key, &context, [&](std::optional<Snapshot> snapshot) { got = std::move(snapshot); });
            REQUIRE(test::waitUntil([&] { return got.has_value(); }));
            return std::move(*got);
        };
        Snapshot snapshot;
        Track track;
        track.id = 5;
        track.stub = true;
        snapshot.entries.push_back({.item = track, .at = {}, .repostedBy = std::nullopt});

        store.save(QStringLiteral("likes"), snapshot);  // signed out: nothing is written
        store.flush();
        CHECK(QDir(dir.path()).isEmpty());
        CHECK_FALSE(load(QStringLiteral("likes")).has_value());

        store.setAccount(7);
        store.save(QStringLiteral("likes"), snapshot);
        store.flush();
        const QString file = dir.filePath(QStringLiteral("7/likes.json"));
        CHECK(QFile::exists(file));
        const std::optional<Snapshot> loaded = load(QStringLiteral("likes"));
        REQUIRE(loaded.has_value());
        CHECK(loaded->entries.size() == 1);

        store.setAccount(8);
        CHECK_FALSE(load(QStringLiteral("likes")).has_value());

        store.setAccount(7);
        QFile damaged(file);
        REQUIRE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
        damaged.write("{\"collection\": [");
        damaged.close();
        CHECK_FALSE(load(QStringLiteral("likes")).has_value());

        // The account is remembered for the next start, until it is forgotten.
        store.flush();
        CHECK(SnapshotStore(dir.path()).lastAccount() == 7);  // the last one set (7, 8, then 7 again)
        store.forget(8);
        CHECK(SnapshotStore(dir.path()).lastAccount() == 7);
        store.forget(7);
        CHECK(SnapshotStore(dir.path()).lastAccount() == 0);
        CHECK_FALSE(QDir(dir.filePath(QStringLiteral("7"))).exists());
        store.save(QStringLiteral("x"), snapshot);
        store.clear();
        CHECK_FALSE(QDir(dir.path()).exists());
    }

    TEST_CASE("a store root with percent signs keeps the lists inside it")
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("dav:prefix=%2Fdav%3Ax/library"));  // like a gvfs mount
        SnapshotStore store(root);
        Snapshot snapshot;
        Track track;
        track.id = 5;
        track.stub = true;
        snapshot.entries.push_back({.item = track, .at = {}, .repostedBy = std::nullopt});
        store.setAccount(7);
        store.save(QStringLiteral("likes"), snapshot);
        store.flush();
        CHECK(QFile::exists(root + QStringLiteral("/7/likes.json")));
        CHECK(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot) ==
              QStringList{QStringLiteral("dav:prefix=%2Fdav%3Ax")});
        store.forget(7);
        CHECK_FALSE(QDir(root + QStringLiteral("/7")).exists());
        store.clear();
        CHECK_FALSE(QDir(root).exists());
    }
}

TEST_SUITE("models.sync")
{
    TEST_CASE("a synced list loads every page by itself and saves it; next time the snapshot shows at once and only "
              "what differs changes")
    {
        Harness h;
        QTemporaryDir dir;
        SnapshotStore store(dir.path());
        store.setAccount(42);
        auto pages = std::make_shared<std::vector<std::vector<Id>>>(std::vector<std::vector<Id>>{{1, 2}, {3, 4}, {5}});
        h.transport.setHandler(pagedTracks(pages));
        const auto likes = [](int limit) {
            return endpoints::userLikes(42, limit);
        };
        {
            models::CollectionModel model(h.client);
            QAbstractItemModelTester tester(&model);
            model.setSyncedSource(likes, &store, QStringLiteral("likes"));
            REQUIRE(test::waitUntil([&] { return model.complete(); }));
            CHECK(idsOf(model) == std::vector<Id>{1, 2, 3, 4, 5});
            CHECK_FALSE(model.canFetchMore({}));
            CHECK(FakeApi::query(h.transport.requests().front(), QStringLiteral("limit")) == u"200");
            store.flush();
        }
        CHECK(QFile::exists(dir.filePath(QStringLiteral("42/likes.json"))));

        // On SoundCloud meanwhile: track 6 liked, track 4 unliked.
        *pages = {{6, 1, 2}, {3}, {5}};
        models::CollectionModel model(h.client);
        QAbstractItemModelTester tester(&model);
        h.transport.reset();
        // The first change is the snapshot, shown before any request has been answered.
        std::optional<std::vector<Id>> firstShown;
        bool firstStale = false;
        int requestsWhenShown = -1;
        QObject::connect(&model, &models::CollectionModel::listChanged, &model, [&] {
            if (!firstShown) {
                firstShown = idsOf(model);
                firstStale = std::get<Track>(model.entries()[2].item).stale;
                requestsWhenShown = h.transport.count();
            }
        });
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        model.setSyncedSource(likes, &store, QStringLiteral("likes"));
        REQUIRE(test::waitUntil([&] { return model.complete(); }));
        REQUIRE(firstShown.has_value());
        CHECK(*firstShown == std::vector<Id>{1, 2, 3, 4, 5});
        CHECK(firstStale);
        CHECK(requestsWhenShown == 0);
        CHECK(idsOf(model) == std::vector<Id>{6, 1, 2, 3, 5});
        CHECK(resets.isEmpty());      // rows were inserted and removed, never reset: views keep their place
        CHECK(removed.size() == 1);   // track 4
        CHECK(inserted.size() == 2);  // the snapshot, then track 6 on top as soon as page one came
        CHECK_FALSE(std::get<Track>(model.entries()[2].item).stale);
    }

    TEST_CASE("applying a list resets only when items moved, and a stub never replaces a known track")
    {
        Harness h;
        models::CollectionModel model(h.client);
        QAbstractItemModelTester tester(&model);
        const auto entries = [](std::initializer_list<Id> ids, Id stubId = 0) {
            std::vector<Entry> out;
            for (const Id id : ids) {
                Track track;
                track.id = id;
                track.title = QStringLiteral("T%1").arg(id);
                track.stub = id == stubId;
                out.push_back({.item = track, .at = {}, .repostedBy = std::nullopt});
            }
            return out;
        };
        model.setEntries(entries({1, 2, 3}));
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        model.applyEntries(entries({1, 3}));
        CHECK(resets.isEmpty());
        model.applyEntries(entries({3, 1}));
        CHECK(resets.size() == 1);
        CHECK(idsOf(model) == std::vector<Id>{3, 1});
        model.applyEntries(entries({3, 1}, 3));
        CHECK_FALSE(std::get<Track>(model.entries()[0].item).stub);
    }

    TEST_CASE("playlists listed without a cover show their first track's, never the owner's avatar")
    {
        Harness h;
        h.transport.setHandler([](const net::Request& request) -> Result<net::Response> {
            if (request.url.path() == u"/playlists/9") {
                return FakeApi::reply(200, playlistJson(9, {trackJson(1), "{\"id\":2,\"kind\":\"track\"}"}));
            }
            return FakeApi::reply(404, {});
        });
        Result<Playlist> listed = parse::playlist(playlistJson(9, {}));
        REQUIRE(listed);
        CHECK(listed->artwork().isEmpty());
        CHECK(listed->coverUnknown());
        models::CollectionModel model(h.client);
        model.setEntries({{.item = *listed, .at = {}, .repostedBy = std::nullopt}});
        CHECK(model.data(model.index(0), models::CollectionModel::ArtworkRole).toString().isEmpty());
        REQUIRE(test::waitUntil([&] {
            return model.data(model.index(0), models::CollectionModel::ArtworkRole)
                .toString()
                .contains(u"artworks-abc");
        }));
        CHECK(h.transport.count() == 1);
        (void)model.data(model.index(0), models::CollectionModel::ThumbnailRole);
        CHECK(h.transport.count() == 1);  // asked once
    }
}
