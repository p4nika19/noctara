#pragma once

#include "api/Types.hpp"

#include "core/Error.hpp"

#include <QByteArray>

#include <vector>

// Parsers for api-v2 JSON (simdjson, kept behind this interface). Each parses straight into the compact domain
// structs; nothing of the JSON document survives the call. Malformed JSON or a missing required field yields
// ErrorCode::InvalidResponse; objects of unknown kinds inside collections are skipped, so a new kind of feed
// entry does not break a whole page. Not thread-safe per thread: each thread has its own parser.
namespace noctara::api::parse {

[[nodiscard]] Result<Track> track(const QByteArray& json);
[[nodiscard]] Result<User> user(const QByteArray& json);
[[nodiscard]] Result<Playlist> playlist(const QByteArray& json);
// /resolve: whatever the permalink points at.
[[nodiscard]] Result<Item> item(const QByteArray& json);
// /tracks?ids=: a bare array; ids SoundCloud does not know (deleted, private) are simply missing.
[[nodiscard]] Result<std::vector<Track>> tracks(const QByteArray& json);
// Any paginated collection ({"collection": […], "next_href": …}) or bare array: search results, a user's
// tracks, likes, play history, the feed, charts. Wrapped items ({"track": …, "created_at": …}) are unwrapped.
[[nodiscard]] Result<Page<Entry>> collection(const QByteArray& json);
[[nodiscard]] Result<Waveform> waveform(const QByteArray& json);
// The tracks of any playlist object (system playlists too, whose ids are text) as one page of entries.
[[nodiscard]] Result<Page<Entry>> playlistTracks(const QByteArray& json);
// A transcoding's resolution: {"url": "<signed media URL>"}.
[[nodiscard]] Result<QUrl> streamUrl(const QByteArray& json);
// A saved list (api/Snapshot): a collection page plus Noctara's "noctara_tag" and "noctara_complete" keys.
struct SnapshotFields {
    Page<Entry> page;
    QString tag;
    bool complete = true;
};
[[nodiscard]] Result<SnapshotFields> snapshot(const QByteArray& json);
// An encrypted transcoding's resolution: {"url": …, "licenseAuthToken": …}.
[[nodiscard]] Result<ProtectedStream> protectedStream(const QByteArray& json);

}  // namespace noctara::api::parse
