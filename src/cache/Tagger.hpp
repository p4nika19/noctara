#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace noctara::cache {

class FileOps;
struct TrackInfo;

struct TagData {
    QString title;
    QString artist;
    QString album;  // empty: no album frame (never invented)
    QString genre;
    QString date;  // ISO 8601 date, e.g. "2024-05-01"
    QUrl permalink;
    QByteArray cover;  // embedded exactly as received
    QString coverMime;

    friend bool operator==(const TagData&, const TagData&) = default;
};

// Tag text without control characters (C0, DEL, C1), trimmed. Everything else (Cyrillic, CJK, emoji with
// joiners) is kept and written as UTF-8.
[[nodiscard]] QString sanitizeTagText(const QString& text);
[[nodiscard]] TagData tagDataFor(const TrackInfo& info, const QByteArray& cover, const QString& coverMime);

// Writes standard tags into a complete cached audio file: ID3v2.4 for MP3, MP4 atoms for AAC/M4A, Vorbis
// comments with an embedded picture for Opus and Vorbis. Works on a temporary copy, then checks that the audio
// payload hash is unchanged (and equals `expectedPayload`, if given) and that duration, sample rate and channels
// are the same before atomically replacing the file. On any failure the original stays as it was.
// Containers that cannot be tagged without remuxing yield ErrorCode::Unsupported.
[[nodiscard]] Result<void> writeTags(const QString& path, const TagData& tags, const QByteArray& expectedPayload,
                                     FileOps& ops);

// Reads back what writeTags() wrote (for verification and tests).
[[nodiscard]] Result<TagData> readTags(const QString& path);

}  // namespace noctara::cache
