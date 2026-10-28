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
        if (candidate.encrypted || candidate.url.isEmpty() || (wholeAvailable && candidate.snipped)) {
            continue;
        }
        if (!best || rank(candidate) > rank(*best)) {
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

}  // namespace noctara::api
