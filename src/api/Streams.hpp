#pragma once

#include "api/Types.hpp"

#include "cache/TrackInfo.hpp"

#include <cstdint>
#include <optional>

namespace noctara::api {

enum class StreamPreference : std::uint8_t {
    Best,       // the highest bitrate the account may play
    DataSaver,  // the lowest bitrate (metered connections)
};

// The transcoding to play natively: never an encrypted one (those belong to the DRM helper) nor an adaptive
// master playlist ("abr_*"); a preview only when nothing else is available to this account. Among equals,
// progressive beats HLS (simpler to cache by range).
[[nodiscard]] std::optional<Transcoding> pickTranscoding(const Track& track, StreamPreference preference);

// Rough bitrate of a preset ("aac_160k" → 160; "mp3_*" → 128; "opus_*" → 64), for ordering only.
[[nodiscard]] int estimatedBitrateKbps(const Transcoding& transcoding);

// The audio cache's self-describing record for `track` played as `transcoding` (`context`: the playlist or set it
// was played from, if any). The cache fills in its own fields (size, hashes, cached-at).
[[nodiscard]] cache::TrackInfo toTrackInfo(const Track& track, const Transcoding& transcoding,
                                           const QString& context = {});

}  // namespace noctara::api
