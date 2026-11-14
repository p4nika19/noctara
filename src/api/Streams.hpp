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

// The transcoding for the protected player: Widevine-encrypted HLS ("ctr-encrypted-hls"; the cbc variant carries
// only FairPlay keys), the whole track, no adaptive master playlist. nullopt when there is none.
[[nodiscard]] std::optional<Transcoding> pickProtectedTranscoding(const Track& track, StreamPreference preference);

// Rough bitrate of a preset ("aac_160k" → 160; "mp3_*" → 128; "opus_*" → 64), for ordering only.
[[nodiscard]] int estimatedBitrateKbps(const Transcoding& transcoding);

// The audio cache's self-describing record for `track` played as `transcoding` (`context`: the playlist or set it
// was played from, if any). The cache fills in its own fields (size, hashes, cached-at).
[[nodiscard]] cache::TrackInfo toTrackInfo(const Track& track, const Transcoding& transcoding,
                                           const QString& context = {});

// A track stored in the audio cache, as a list shows and the player plays it (the Downloaded page). It is `stale`:
// online the player looks it up again first (fresh authorization and transcodings); offline it plays as it is,
// from the stored transcoding, which the cache serves without asking for a URL. That transcoding's URL is a
// placeholder the API client never sends anywhere (transcodingUrl() accepts only API hosts).
[[nodiscard]] Track trackFromCache(const cache::TrackInfo& info);

}  // namespace noctara::api
