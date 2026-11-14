#include "api/Streams.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <tuple>

namespace noctara::api {

int estimatedBitrateKbps(const Transcoding& transcoding)
{
    static const QRegularExpression explicitRate(QStringLiteral(R"(_(\d+)k\b)"));
    if (const auto match = explicitRate.match(transcoding.preset); match.hasMatch()) {
        return match.captured(1).toInt();
    }
    if (transcoding.preset.startsWith(u"opus")) {
        return 64;
    }
    return 128;  // mp3 and the unknown
}

std::optional<Transcoding> pickTranscoding(const Track& track, StreamPreference preference)
{
    const bool wholeAvailable =
        std::ranges::any_of(track.transcodings, [](const Transcoding& t) { return !t.encrypted && !t.snipped; });
    std::optional<Transcoding> best;
    const auto rank = [&](const Transcoding& t) {
        const int rate = estimatedBitrateKbps(t);
        return std::tuple(preference == StreamPreference::Best ? rate : -rate,
                          t.protocol == cache::StreamProtocol::Progressive);
    };
    for (const Transcoding& candidate : track.transcodings) {
        // "abr_*" presets are adaptive master playlists; the cache stores media playlists only.
        if (candidate.encrypted || candidate.url.isEmpty() || (wholeAvailable && candidate.snipped) ||
            candidate.preset.startsWith(u"abr")) {
            continue;
        }
        if (!best || rank(candidate) > rank(*best)) {
            best = candidate;
        }
    }
    return best;
}

std::optional<Transcoding> pickProtectedTranscoding(const Track& track, StreamPreference preference)
{
    std::optional<Transcoding> best;
    const auto rate = [preference](const Transcoding& t) {
        return preference == StreamPreference::Best ? estimatedBitrateKbps(t) : -estimatedBitrateKbps(t);
    };
    for (const Transcoding& candidate : track.transcodings) {
        if (!candidate.encrypted || candidate.protocolName != u"ctr-encrypted-hls" || candidate.url.isEmpty() ||
            candidate.snipped || candidate.preset.startsWith(u"abr")) {
            continue;
        }
        if (!best || rate(candidate) > rate(*best)) {
            best = candidate;
        }
    }
    return best;
}

cache::TrackInfo toTrackInfo(const Track& track, const Transcoding& transcoding, const QString& context)
{
    cache::TrackInfo info;
    info.key = {.trackId = QString::number(track.id), .preset = transcoding.cacheName()};
    info.drm = transcoding.encrypted;
    info.title = track.title;
    info.artist = track.artist();
    info.uploader = track.user.username;
    info.album = track.publisherAlbum;
    info.context = context;
    info.genre = track.genre;
    info.durationMs = transcoding.durationMs > 0 ? transcoding.durationMs : track.durationMs;
    info.created = track.createdAt;
    info.released = track.releasedAt;
    info.permalink = QUrl(track.permalinkUrl);
    info.artworkUrl = QUrl(sizedArtwork(track.artwork(), 500));
    info.artworkThumbUrl = QUrl(sizedArtwork(track.artwork(), 67));
    info.waveformUrl = QUrl(track.waveformUrl);
    info.protocol = transcoding.protocol;
    info.mimeType = transcoding.mimeType;
    const QString preset = transcoding.preset;
    info.codec = preset.section(u'_', 0, 0);
    info.bitrateKbps = estimatedBitrateKbps(transcoding);
    info.quality = transcoding.quality;
    info.sourceUpdatedAt = track.lastModified;
    return info;
}

Track trackFromCache(const cache::TrackInfo& info)
{
    Track track;
    track.id = info.key.trackId.toLongLong();
    track.stale = true;
    track.title = info.title;
    track.user.username = info.uploader.isEmpty() ? info.artist : info.uploader;
    if (info.artist != track.user.username) {
        track.publisherArtist = info.artist;
    }
    track.publisherAlbum = info.album;
    track.genre = info.genre;
    track.durationMs = info.durationMs;
    track.fullDurationMs = info.durationMs;
    track.createdAt = info.created;
    track.releasedAt = info.released;
    track.lastModified = info.sourceUpdatedAt;
    track.permalinkUrl = info.permalink.toString();
    track.artworkUrl = info.artworkUrl.toString();
    track.waveformUrl = info.waveformUrl.toString();
    // The cache name is "<protocol>/<preset>/<quality>" (Transcoding::cacheName()).
    Transcoding stored;
    stored.url = QStringLiteral("noctara-cache:stored");
    stored.protocolName = info.key.preset.section(u'/', 0, 0);
    stored.protocol = info.protocol;
    stored.preset = info.key.preset.section(u'/', 1, 1);
    stored.quality = info.key.preset.section(u'/', 2);
    stored.mimeType = info.mimeType;
    stored.durationMs = info.durationMs;
    track.transcodings.push_back(std::move(stored));
    return track;
}

}  // namespace noctara::api
