#include "cache/Hls.hpp"

#include <QList>

#include <cmath>
#include <numeric>

namespace noctara::cache {

namespace {

// Value of NAME=value or NAME="value" in an attribute list.
QByteArray attribute(QByteArrayView list, QByteArrayView name)
{
    qsizetype pos = 0;
    while (pos < list.size()) {
        const qsizetype eq = list.indexOf('=', pos);
        if (eq < 0) {
            break;
        }
        const QByteArrayView key = list.sliced(pos, eq - pos).trimmed();
        qsizetype end = eq + 1;
        QByteArray value;
        if (end < list.size() && list[end] == '"') {
            const qsizetype close = list.indexOf('"', end + 1);
            value = list.sliced(end + 1, (close < 0 ? list.size() : close) - end - 1).toByteArray();
            end = close < 0 ? list.size() : close + 1;
        } else {
            const qsizetype comma = list.indexOf(',', end);
            value = list.sliced(end, (comma < 0 ? list.size() : comma) - end).trimmed().toByteArray();
            end = comma < 0 ? list.size() : comma;
        }
        if (key == name) {
            return value;
        }
        pos = end + 1;
    }
    return {};
}

// "length[@offset]"; without an offset the range follows the previous one of the same resource.
bool parseByteRange(QByteArrayView text, std::int64_t previousEnd, HlsSegment& segment)
{
    const qsizetype at = text.indexOf('@');
    bool ok = false;
    segment.rangeLength = (at < 0 ? text : text.first(at)).toLongLong(&ok);
    if (!ok || segment.rangeLength < 0) {
        return false;
    }
    if (at >= 0) {
        segment.rangeOffset = text.sliced(at + 1).toLongLong(&ok);
        return ok && segment.rangeOffset >= 0;
    }
    segment.rangeOffset = std::max<std::int64_t>(0, previousEnd);
    return true;
}

}  // namespace

int HlsPlaylist::pieceCount() const noexcept
{
    return static_cast<int>(segments.size()) + (init ? 1 : 0);
}

const HlsSegment& HlsPlaylist::piece(int index) const
{
    if (init) {
        return index == 0 ? *init : segments.at(static_cast<std::size_t>(index - 1));
    }
    return segments.at(static_cast<std::size_t>(index));
}

double HlsPlaylist::totalDuration() const noexcept
{
    return std::accumulate(segments.begin(), segments.end(), 0.0,
                           [](double sum, const HlsSegment& segment) { return sum + segment.duration; });
}

QByteArray hlsPieceName(int index)
{
    return QByteArray::number(index).rightJustified(5, '0');
}

QByteArray HlsPlaylist::normalized() const
{
    QByteArray out = "#EXTM3U\n#EXT-X-VERSION:" + QByteArray::number(version) +
                     "\n#EXT-X-TARGETDURATION:" + QByteArray::number(static_cast<int>(std::ceil(targetDuration))) +
                     "\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXT-X-MEDIA-SEQUENCE:0\n";
    int index = 0;
    if (init) {
        out += "#EXT-X-MAP:URI=\"" + hlsPieceName(index++) + "\"\n";
    }
    for (const HlsSegment& segment : segments) {
        out += "#EXTINF:" + QByteArray::number(segment.duration, 'f', 6) + ",\n" + hlsPieceName(index++) + '\n';
    }
    out += "#EXT-X-ENDLIST\n";
    return out;
}

bool HlsPlaylist::sameStructure(const HlsPlaylist& other) const noexcept
{
    if (init.has_value() != other.init.has_value() || segments.size() != other.segments.size()) {
        return false;
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (std::abs(segments[i].duration - other.segments[i].duration) > 0.001) {
            return false;
        }
    }
    return true;
}

Result<HlsPlaylist> parseHls(QByteArrayView text, const QUrl& base)
{
    const QList<QByteArray> lines = text.toByteArray().split('\n');
    if (lines.isEmpty() || !lines.front().trimmed().startsWith("#EXTM3U")) {
        return fail(ErrorCode::ParseError, "not an M3U8 playlist");
    }
    HlsPlaylist playlist;
    bool endList = false;
    std::optional<double> pendingDuration;
    std::optional<HlsSegment> pendingRange;
    std::int64_t previousEnd = 0;
    for (const QByteArray& raw : lines) {
        const QByteArrayView line = QByteArrayView(raw).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith("#EXT-X-STREAM-INF")) {
            return fail(ErrorCode::Unsupported, "master playlist");
        }
        if (line.startsWith("#EXT-X-KEY:") || line.startsWith("#EXT-X-SESSION-KEY:")) {
            if (attribute(line.sliced(line.indexOf(':') + 1), "METHOD") != "NONE") {
                return fail(ErrorCode::NotCacheable, "encrypted (protected) HLS media");
            }
        } else if (line.startsWith("#EXT-X-VERSION:")) {
            playlist.version = line.sliced(15).toInt();
        } else if (line.startsWith("#EXT-X-TARGETDURATION:")) {
            playlist.targetDuration = line.sliced(22).toDouble();
        } else if (line.startsWith("#EXT-X-ENDLIST")) {
            endList = true;
        } else if (line.startsWith("#EXT-X-MAP:")) {
            const QByteArrayView list = line.sliced(11);
            HlsSegment init;
            init.url = base.resolved(QUrl::fromEncoded(attribute(list, "URI")));
            if (const QByteArray range = attribute(list, "BYTERANGE");
                !range.isEmpty() && !parseByteRange(range, 0, init)) {
                return fail(ErrorCode::ParseError, "bad EXT-X-MAP byte range");
            }
            playlist.init = init;
        } else if (line.startsWith("#EXTINF:")) {
            const QByteArrayView value = line.sliced(8);
            const qsizetype comma = value.indexOf(',');
            bool ok = false;
            pendingDuration = (comma < 0 ? value : value.first(comma)).toDouble(&ok);
            if (!ok || *pendingDuration < 0) {
                return fail(ErrorCode::ParseError, "bad EXTINF duration");
            }
        } else if (line.startsWith("#EXT-X-BYTERANGE:")) {
            HlsSegment ranged;
            if (!parseByteRange(line.sliced(17), previousEnd, ranged)) {
                return fail(ErrorCode::ParseError, "bad byte range");
            }
            pendingRange = ranged;
        } else if (!line.startsWith('#')) {
            if (!pendingDuration) {
                return fail(ErrorCode::ParseError, "segment without EXTINF");
            }
            HlsSegment segment = pendingRange.value_or(HlsSegment{});
            segment.url = base.resolved(QUrl::fromEncoded(line.toByteArray()));
            segment.duration = *pendingDuration;
            if (segment.rangeOffset >= 0) {
                previousEnd = segment.rangeOffset + segment.rangeLength;
            }
            playlist.segments.push_back(segment);
            pendingDuration.reset();
            pendingRange.reset();
        }
    }
    if (!endList) {
        return fail(ErrorCode::Unsupported, "live playlist");
    }
    if (playlist.segments.empty()) {
        return fail(ErrorCode::ParseError, "playlist without segments");
    }
    return playlist;
}

}  // namespace noctara::cache
