#pragma once

#include "api/Types.hpp"

#include "core/Error.hpp"
#include "net/Transport.hpp"

#include <QByteArray>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

namespace noctara::api {

// Whether the user's OAuth token is sent. It is only ever sent to SoundCloud's API hosts (isApiHost).
enum class Auth : std::uint8_t {
    None,      // public data that must not depend on the account (waveforms)
    Optional,  // sent when signed in: the answer may include private items the account can see
    Required,  // fails with ErrorCode::SignInRequired before any request when nobody is signed in
};

struct ApiRequest {
    net::Method method = net::Method::Get;
    QUrl url;  // absolute; the client adds client_id
    QByteArray body;
    Auth auth = Auth::Optional;
    bool clientId = true;  // append the web client's client_id (not for media hosts)
    net::CacheMode cache = net::CacheMode::Default;
    std::chrono::seconds cacheTtl{0};  // freshness when SoundCloud sends no Cache-Control
    bool background = false;
};

// A request plus the parser of its answer.
template <class T> struct Endpoint {
    ApiRequest request;
    Result<T> (*parse)(const QByteArray& json) = nullptr;
};

inline constexpr const char* kApiBase = "https://api-v2.soundcloud.com";

// SoundCloud's API hosts over https: the only hosts that ever get the OAuth token or the client_id.
[[nodiscard]] bool isApiHost(const QUrl& url);

namespace endpoints {

enum class SearchKind : std::uint8_t { All, Tracks, Users, Playlists, Albums };
enum class ChartKind : std::uint8_t { Top, Trending };

[[nodiscard]] Endpoint<Item> resolve(const QUrl& permalink);
[[nodiscard]] Endpoint<Track> track(Id id);
// At most kMaxTrackIds per request; IApiClient::lookupTracks batches larger sets.
[[nodiscard]] Endpoint<std::vector<Track>> tracks(std::span<const Id> ids);
inline constexpr std::size_t kMaxTrackIds = 50;
[[nodiscard]] Endpoint<User> user(Id id);
[[nodiscard]] Endpoint<User> me();
[[nodiscard]] Endpoint<Playlist> playlist(Id id);

[[nodiscard]] Endpoint<Page<Entry>> search(const QString& query, SearchKind kind, int limit);
[[nodiscard]] Endpoint<Page<Entry>> userTracks(Id user, int limit);
[[nodiscard]] Endpoint<Page<Entry>> userPlaylists(Id user, int limit);
[[nodiscard]] Endpoint<Page<Entry>> userLikes(Id user, int limit);
[[nodiscard]] Endpoint<Page<Entry>> related(Id track, int limit);
[[nodiscard]] Endpoint<Page<Entry>> stream(int limit);   // the signed-in user's feed
[[nodiscard]] Endpoint<Page<Entry>> history(int limit);  // recently played tracks
// genre: "soundcloud:genres:all-music", "soundcloud:genres:electronic", …
[[nodiscard]] Endpoint<Page<Entry>> charts(ChartKind kind, const QString& genre, int limit);
// The page after `page`, from its next_href, with the same auth and freshness as the first request. A next_href
// that does not point at an API host yields a request the client refuses.
[[nodiscard]] Endpoint<Page<Entry>> next(const Endpoint<Page<Entry>>& first, const QString& href);

[[nodiscard]] Endpoint<Waveform> waveform(const QString& url);
// The signed, short-lived media URL of a transcoding. Never cached: the signature expires.
[[nodiscard]] Endpoint<QUrl> streamUrl(const Transcoding& transcoding, const QString& trackAuthorization);

}  // namespace endpoints

}  // namespace noctara::api
