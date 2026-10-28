#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <cstdint>

namespace noctara::cache {

enum class StreamProtocol : std::uint8_t { Progressive, Hls };

// An audio cache entry is identified by the track and the transcoding, never by the (signed, expiring) URL.
struct TrackKey {
    QString trackId;
    QString preset;  // transcoding, e.g. "progressive/mp3_1_0/sq"; the same track in another quality is another entry

    [[nodiscard]] QByteArray name() const;   // opaque directory name
    [[nodiscard]] std::uint64_t id() const;  // in-memory id for the RAM tier
    friend bool operator==(const TrackKey&, const TrackKey&) = default;
};

// Everything the cache records about an entry, written as a versioned sidecar before the first audio byte, so
// any entry (partial or complete) can be shown and played offline with correct metadata.
struct TrackInfo {
    static constexpr int kVersion = 1;

    TrackKey key;
    bool drm = false;  // protected tracks never get an entry, sidecar or tags (see docs/caching.md)

    QString title;
    QString artist;  // as credited (publisher metadata), else the uploader
    QString uploader;
    QString album;    // only a real album title; never invented
    QString context;  // playlist or set the track was played from, if any
    QString genre;
    std::int64_t durationMs = 0;
    QDateTime created;
    QDateTime released;
    QUrl permalink;
    QUrl artworkUrl;       // large cover
    QUrl artworkThumbUrl;  // small thumbnail
    QUrl waveformUrl;

    StreamProtocol protocol = StreamProtocol::Progressive;
    QString mimeType;  // "audio/mpeg", "audio/mp4", "audio/ogg; codecs=\"opus\""
    QString codec;     // "mp3", "aac", "opus"
    int bitrateKbps = 0;
    QString quality;  // "sq", "hq"

    QString sourceUpdatedAt;  // the track's updated_at from the API; a change refreshes metadata and tags
    QByteArray mediaEtag;     // validator of the media itself; a change invalidates the stored audio

    // Maintained by the cache.
    std::int64_t contentLength = -1;  // of the source stream
    QDateTime cachedAt;
    bool complete = false;
    QByteArray coverHash;  // content hashes of the stored covers (empty: not stored yet, retried later)
    QByteArray thumbHash;
    QString coverMime;
    QByteArray payloadHash;  // hash of the audio frames, checked after every tagging pass
    QString taggedFor;       // sourceUpdatedAt the tags were written for; differs = re-tag lazily

    [[nodiscard]] QByteArray toJson() const;
    [[nodiscard]] static Result<TrackInfo> fromJson(const QByteArray& json);
    // Copies the API-provided fields (not the cache-maintained ones) from `fresh`.
    void refreshFrom(const TrackInfo& fresh);
    [[nodiscard]] bool sameMetadata(const TrackInfo& other) const;
};

}  // namespace noctara::cache
