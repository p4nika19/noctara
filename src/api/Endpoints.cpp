#include "api/Endpoints.hpp"

#include "api/Parse.hpp"

#include <QStringList>
#include <QUrlQuery>

namespace noctara::api {

namespace {

using namespace std::chrono_literals;

// Freshness when SoundCloud sends no Cache-Control; stale answers are revalidated (ETag) and still served
// while offline.
constexpr std::chrono::seconds kVolatile = 30s;    // feed, likes, history: change with every action
constexpr std::chrono::seconds kListing = 5min;    // search results, a user's uploads
constexpr std::chrono::seconds kMetadata = 1h;     // a track, user or playlist
constexpr std::chrono::seconds kImmutable = 720h;  // waveforms never change

QUrl apiUrl(const QString& path, const QList<std::pair<QString, QString>>& parameters = {})
{
    QUrl url(QString::fromLatin1(kApiBase) + path);
    if (!parameters.isEmpty()) {
        QUrlQuery query;
        for (const auto& [name, value] : parameters) {
            query.addQueryItem(name, QString::fromUtf8(QUrl::toPercentEncoding(value)));
        }
        url.setQuery(query);
    }
    return url;
}

template <class T>
Endpoint<T> get(QUrl url, Result<T> (*parse)(const QByteArray&), std::chrono::seconds ttl, Auth auth = Auth::Optional)
{
    Endpoint<T> endpoint;
    endpoint.request.url = std::move(url);
    endpoint.request.auth = auth;
    endpoint.request.cacheTtl = ttl;
    endpoint.parse = parse;
    return endpoint;
}

QList<std::pair<QString, QString>> paging(int limit)
{
    return {{QStringLiteral("limit"), QString::number(limit)},
            {QStringLiteral("linked_partitioning"), QStringLiteral("1")}};
}

Endpoint<Page<Entry>> list(const QString& path, int limit, std::chrono::seconds ttl, Auth auth = Auth::Optional,
                           QList<std::pair<QString, QString>> extra = {})
{
    extra.append(paging(limit));
    return get<Page<Entry>>(apiUrl(path, extra), parse::collection, ttl, auth);
}

QString idPath(const char* prefix, Id id, const char* suffix = "")
{
    return QLatin1StringView(prefix) + QString::number(id) + QLatin1StringView(suffix);
}

}  // namespace

bool isApiHost(const QUrl& url)
{
    return url.scheme() == u"https" && url.port(443) == 443 &&
           (url.host() == u"api-v2.soundcloud.com" || url.host() == u"api.soundcloud.com");
}

namespace endpoints {

Endpoint<Item> resolve(const QUrl& permalink)
{
    return get<Item>(apiUrl(QStringLiteral("/resolve"), {{QStringLiteral("url"), permalink.toString()}}), parse::item,
                     kMetadata);
}

Endpoint<Track> track(Id id)
{
    return get<Track>(apiUrl(idPath("/tracks/", id)), parse::track, kMetadata);
}

Endpoint<std::vector<Track>> tracks(std::span<const Id> ids)
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(ids.size()));
    for (const Id id : ids) {
        list.append(QString::number(id));
    }
    return get<std::vector<Track>>(apiUrl(QStringLiteral("/tracks"), {{QStringLiteral("ids"), list.join(u',')}}),
                                   parse::tracks, kMetadata);
}

Endpoint<User> user(Id id)
{
    return get<User>(apiUrl(idPath("/users/", id)), parse::user, kMetadata);
}

Endpoint<User> me()
{
    return get<User>(apiUrl(QStringLiteral("/me")), parse::user, kVolatile, Auth::Required);
}

Endpoint<User> verifyToken(const QByteArray& token)
{
    Endpoint<User> endpoint = me();
    endpoint.request.candidateToken = token;
    endpoint.request.cache = net::CacheMode::Bypass;
    return endpoint;
}

Endpoint<Playlist> playlist(Id id)
{
    return get<Playlist>(apiUrl(idPath("/playlists/", id)), parse::playlist, kMetadata);
}

Endpoint<Page<Entry>> search(const QString& query, SearchKind kind, int limit)
{
    QString path;
    switch (kind) {
    case SearchKind::All: path = QStringLiteral("/search"); break;
    case SearchKind::Tracks: path = QStringLiteral("/search/tracks"); break;
    case SearchKind::Users: path = QStringLiteral("/search/users"); break;
    case SearchKind::Playlists: path = QStringLiteral("/search/playlists_without_albums"); break;
    case SearchKind::Albums: path = QStringLiteral("/search/albums"); break;
    }
    return list(path, limit, kListing, Auth::Optional, {{QStringLiteral("q"), query}});
}

Endpoint<Page<Entry>> userTracks(Id user, int limit)
{
    return list(idPath("/users/", user, "/tracks"), limit, kListing);
}

Endpoint<Page<Entry>> userPlaylists(Id user, int limit)
{
    return list(idPath("/users/", user, "/playlists"), limit, kListing);
}

Endpoint<Page<Entry>> userLikes(Id user, int limit)
{
    return list(idPath("/users/", user, "/track_likes"), limit, kVolatile);
}

Endpoint<Page<Entry>> followings(Id user, int limit)
{
    return list(idPath("/users/", user, "/followings"), limit, kListing);
}

Endpoint<Page<Entry>> library(int limit)
{
    return list(QStringLiteral("/me/library/all"), limit, kVolatile, Auth::Required);
}

Endpoint<Page<Entry>> related(Id track, int limit)
{
    return list(idPath("/tracks/", track, "/related"), limit, kListing);
}

Endpoint<Page<Entry>> stream(int limit)
{
    return list(QStringLiteral("/stream"), limit, kVolatile, Auth::Required);
}

Endpoint<Page<Entry>> history(int limit)
{
    return list(QStringLiteral("/me/play-history/tracks"), limit, kVolatile, Auth::Required);
}

Endpoint<Page<Entry>> trending(const QString& genre)
{
    // Only safe characters reach the path: genres are fixed ids, but never trust a string in a URL.
    QString id = genre;
    id.removeIf([](QChar c) { return !(c.isLetterOrNumber() || c == u'-'); });
    return get<Page<Entry>>(
        apiUrl(QStringLiteral("/system-playlists/soundcloud:system-playlists:trending-by-genre:") + id),
        parse::playlistTracks, kListing);
}

Endpoint<Page<Entry>> next(const Endpoint<Page<Entry>>& first, const QString& href)
{
    Endpoint<Page<Entry>> endpoint = first;
    const QUrl url(href);
    endpoint.request.url = isApiHost(url) ? url : QUrl();
    return endpoint;
}

Endpoint<Waveform> waveform(const QString& url)
{
    Endpoint<Waveform> endpoint = get<Waveform>(QUrl(url), parse::waveform, kImmutable, Auth::None);
    endpoint.request.clientId = false;
    return endpoint;
}

namespace {

QUrl transcodingUrl(const Transcoding& transcoding, const QString& trackAuthorization)
{
    QUrl url(transcoding.url);
    if (!trackAuthorization.isEmpty()) {
        QUrlQuery query(url);
        query.removeAllQueryItems(QStringLiteral("track_authorization"));
        query.addQueryItem(QStringLiteral("track_authorization"),
                           QString::fromUtf8(QUrl::toPercentEncoding(trackAuthorization)));
        url.setQuery(query);
    }
    return isApiHost(url) ? url : QUrl();
}

}  // namespace

Endpoint<QUrl> streamUrl(const Transcoding& transcoding, const QString& trackAuthorization)
{
    Endpoint<QUrl> endpoint =
        get<QUrl>(transcodingUrl(transcoding, trackAuthorization), parse::streamUrl, std::chrono::seconds(0));
    endpoint.request.cache = net::CacheMode::Bypass;
    return endpoint;
}

Endpoint<ProtectedStream> protectedStream(const Transcoding& transcoding, const QString& trackAuthorization)
{
    Endpoint<ProtectedStream> endpoint = get<ProtectedStream>(transcodingUrl(transcoding, trackAuthorization),
                                                              parse::protectedStream, std::chrono::seconds(0));
    endpoint.request.cache = net::CacheMode::Bypass;  // signed and short-lived: never stored
    return endpoint;
}

QUrl widevineLicense()
{
    return QUrl(QStringLiteral("https://license.media-streaming.soundcloud.cloud/playback/widevine"));
}

}  // namespace endpoints

}  // namespace noctara::api
