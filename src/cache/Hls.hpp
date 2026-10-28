#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QUrl>

#include <cstdint>
#include <optional>
#include <vector>

namespace noctara::cache {

struct HlsSegment {
    QUrl url;
    double duration = 0;            // seconds; 0 for the init segment
    std::int64_t rangeOffset = -1;  // EXT-X-BYTERANGE, -1 = whole resource
    std::int64_t rangeLength = -1;
};

// A finished (VOD) HLS media playlist. Pieces of the cache entry are the init segment (EXT-X-MAP), if any,
// followed by the media segments.
struct HlsPlaylist {
    int version = 3;
    double targetDuration = 0;
    std::optional<HlsSegment> init;
    std::vector<HlsSegment> segments;

    [[nodiscard]] int pieceCount() const noexcept;
    [[nodiscard]] const HlsSegment& piece(int index) const;
    [[nodiscard]] double totalDuration() const noexcept;
    // The playlist with every URI replaced by the piece's opaque local name, so a finished entry replays
    // offline without the expired signed URLs.
    [[nodiscard]] QByteArray normalized() const;
    // Same segment structure (count, durations) as `other`, i.e. the same media.
    [[nodiscard]] bool sameStructure(const HlsPlaylist& other) const noexcept;
};

[[nodiscard]] QByteArray hlsPieceName(int index);

// Parses a media playlist, resolving URIs against `base`. Master playlists and live playlists are Unsupported;
// encrypted media (EXT-X-KEY other than NONE, i.e. protected content) is NotCacheable.
[[nodiscard]] Result<HlsPlaylist> parseHls(QByteArrayView text, const QUrl& base);

}  // namespace noctara::cache
