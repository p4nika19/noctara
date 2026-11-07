#pragma once

#include "cache/TrackInfo.hpp"

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

// Compact domain structs parsed from SoundCloud's api-v2 JSON. No JSON tree outlives parsing; counts the API
// hides (e.g. plays on some tracks) are -1. URLs are kept as text: building a QUrl costs more than parsing the
// JSON around it, and most are only ever displayed or passed to the image cache; requests build QUrls.
namespace noctara::api {

using Id = std::int64_t;

// The user embedded in tracks, playlists and feed entries.
struct UserRef {
    Id id = 0;
    QString username;
    QString permalink;  // URL slug
    QString avatarUrl;
    bool verified = false;
};

// What the listener may do with a track.
enum class Policy : std::uint8_t {
    Allow,
    Monetize,
    Snip,  // only a 30-second preview is available to this account
    Block,
};

struct Transcoding {
    QString url;           // resolves to the signed media URL (see endpoints::streamUrl)
    QString protocolName;  // as delivered: "progressive", "hls", "ctr-encrypted-hls", "cbc-encrypted-hls"
    cache::StreamProtocol protocol = cache::StreamProtocol::Hls;
    QString preset;    // "mp3_1_0", "aac_160k", "opus_0_0"
    QString mimeType;  // "audio/mpeg", "audio/mp4; codecs=\"mp4a.40.2\""
    QString quality;   // "sq", "hq"
    std::int64_t durationMs = 0;
    bool snipped = false;    // a preview, not the whole track
    bool encrypted = false;  // DRM: playable only through the Electron helper, never cached

    // Identifies the transcoding in the audio cache key, e.g. "hls/aac_160k/hq".
    [[nodiscard]] QString cacheName() const;
};

// A resolved encrypted transcoding: the signed media playlist and the token the license server wants with it.
// Both go only to the protected player (the DRM helper); neither is logged, cached or stored.
struct ProtectedStream {
    QUrl url;
    QString licenseToken;
};

struct Track {
    Id id = 0;
    bool stub = false;  // only the id is known (later tracks of a playlist); hydrate with IApiClient::lookupTracks
    // Read from a saved snapshot: shown as is, but fetched again before it plays (its track_authorization may have
    // expired).
    bool stale = false;
    QString title;
    UserRef user;
    QString genre;
    QString description;
    std::int64_t durationMs = 0;      // of what is playable (a preview is 30 s)
    std::int64_t fullDurationMs = 0;  // of the whole track
    QDateTime createdAt;
    QDateTime releasedAt;  // release date or display date; may equal createdAt
    QString lastModified;  // raw "last_modified"; a change refreshes cached metadata and tags
    QString permalinkUrl;
    QString artworkUrl;  // may be empty: artwork() falls back to the uploader's avatar
    QString waveformUrl;
    std::int64_t playbackCount = -1;
    std::int64_t likesCount = -1;
    std::int64_t repostsCount = -1;
    std::int64_t commentCount = -1;
    Policy policy = Policy::Allow;
    bool streamable = true;
    QString trackAuthorization;  // passed along when resolving a transcoding
    QString publisherArtist;     // publisher metadata, when the label supplied it
    QString publisherAlbum;
    // Only read for a track page (parse::trackDetail): lists do not need them.
    QString tagList;         // raw "tag_list": space-separated, multi-word tags in double quotes (see splitTags)
    bool isPrivate = false;  // "sharing": "private"
    std::vector<Transcoding> transcodings;

    [[nodiscard]] QString artist() const { return publisherArtist.isEmpty() ? user.username : publisherArtist; }
    [[nodiscard]] const QString& artwork() const { return artworkUrl.isEmpty() ? user.avatarUrl : artworkUrl; }
    // The whole track is only available encrypted (Go+ and similar): it plays through the DRM helper and is
    // never cached. A preview may still be playable natively.
    [[nodiscard]] bool drm() const;
    // Only a preview can be played with this account.
    [[nodiscard]] bool previewOnly() const;
};

struct User {
    Id id = 0;
    QString username;
    QString fullName;
    QString permalink;
    QString permalinkUrl;
    QString avatarUrl;
    QString city;
    QString countryCode;
    QString description;
    std::int64_t followersCount = -1;
    std::int64_t followingsCount = -1;
    std::int64_t trackCount = -1;
    std::int64_t playlistCount = -1;
    std::int64_t likesCount = -1;
    bool verified = false;

    [[nodiscard]] UserRef ref() const { return {id, username, permalink, avatarUrl, verified}; }
};

struct Playlist {
    Id id = 0;
    QString title;
    UserRef user;
    QString genre;
    QString setType;  // "album", "ep", "single", "compilation" or empty for a plain playlist
    bool album = false;
    QString permalinkUrl;
    QString artworkUrl;  // may be empty: artwork() falls back to the first track's (never the owner's avatar)
    std::int64_t durationMs = 0;
    std::int64_t trackCount = 0;
    std::int64_t likesCount = -1;
    std::int64_t repostsCount = -1;
    QDateTime createdAt;
    QDateTime publishedAt;
    QString lastModified;
    std::vector<Track> tracks;  // the API returns the first few in full and the rest as stubs

    // The playlist's own cover, else the first track's that has one, else empty (lists show a placeholder).
    [[nodiscard]] QString artwork() const;
    // Lists such as /me/library/all carry neither artwork nor tracks: the cover is known only after fetching the
    // playlist itself.
    [[nodiscard]] bool coverUnknown() const;
};

using Item = std::variant<Track, Playlist, User>;

// One row of a collection: a track, playlist or user, with the time it was liked, played or posted to the feed,
// and who reposted it (feed reposts only).
struct Entry {
    Item item;
    QDateTime at;
    std::optional<UserRef> repostedBy;
};

// A comment on a track, optionally at a moment of it.
struct Comment {
    Id id = 0;
    QString body;
    UserRef user;
    QDateTime createdAt;
    std::int64_t timestampMs = -1;  // where in the track it was left; -1 for none
};

// The answer of a write request whose body does not matter (a like, an unlike).
struct Done {};

template <class T> struct Page {
    std::vector<T> items;
    QString next;  // next_href: the following page, empty at the end
};

// SoundCloud's waveform: peak heights normalised to 0…1.
struct Waveform {
    std::vector<float> samples;
};

[[nodiscard]] Id itemId(const Item& item) noexcept;

// A track's tag_list as tags: words separated by spaces, a tag with spaces in double quotes ("deep house").
[[nodiscard]] QStringList splitTags(const QString& tagList);

// SoundCloud serves artwork in fixed sizes encoded in the file name ("…-large.jpg" is 100 px). Returns the
// smallest variant that is at least `pixels` wide (up to 500 px), or `url` unchanged if it has no size suffix.
[[nodiscard]] QString sizedArtwork(const QString& url, int pixels);

}  // namespace noctara::api
