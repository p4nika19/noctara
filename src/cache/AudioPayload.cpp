#include "cache/AudioPayload.hpp"

#include "cache/Hash.hpp"
#include "core/FileIo.hpp"

#include <QtEndian>

#include <utility>

namespace noctara::cache {

namespace {

std::uint32_t be32(QByteArrayView data, qsizetype at)
{
    return qFromBigEndian<std::uint32_t>(data.data() + at);
}

std::uint64_t be64(QByteArrayView data, qsizetype at)
{
    return qFromBigEndian<std::uint64_t>(data.data() + at);
}

std::uint32_t syncsafe(QByteArrayView data, qsizetype at)
{
    std::uint32_t value = 0;
    for (qsizetype i = 0; i < 4; ++i) {
        value = (value << 7U) | (static_cast<std::uint8_t>(data[at + i]) & 0x7FU);
    }
    return value;
}

Result<QByteArray> mpegPayload(QByteArrayView file)
{
    qsizetype first = 0;
    // Leading ID3v2 tags (there may be more than one).
    while (file.size() - first >= 10 && file.sliced(first).startsWith("ID3")) {
        const auto flags = static_cast<std::uint8_t>(file[first + 5]);
        const qsizetype size = 10 + syncsafe(file, first + 6) + ((flags & 0x10U) != 0 ? 10 : 0);
        first += size;
    }
    qsizetype last = file.size();
    bool trimmed = true;
    while (trimmed && last > first) {
        trimmed = false;
        if (last - first >= 128 && file.sliced(last - 128).startsWith("TAG")) {
            last -= 128;  // ID3v1
            trimmed = true;
        }
        if (last - first >= 32 && file.sliced(last - 32).startsWith("APETAGEX")) {
            const qsizetype size = qFromLittleEndian<std::uint32_t>(file.data() + last - 32 + 12);
            const bool header = (qFromLittleEndian<std::uint32_t>(file.data() + last - 32 + 20) & 0x80000000U) != 0;
            last -= size + (header ? 32 : 0);  // APEv2 (footer + items [+ header])
            trimmed = true;
        }
    }
    if (first >= last) {
        return fail(ErrorCode::Corrupt, "no MPEG audio data");
    }
    Hasher128 hasher;
    hasher.add(file.sliced(first, last - first));
    return hasher.hex();
}

Result<QByteArray> mp4Payload(QByteArrayView file)
{
    Hasher128 hasher;
    bool found = false;
    qsizetype at = 0;
    while (file.size() - at >= 8) {
        std::uint64_t size = be32(file, at);
        qsizetype header = 8;
        if (size == 1) {
            if (file.size() - at < 16) {
                break;
            }
            size = be64(file, at + 8);
            header = 16;
        } else if (size == 0) {
            size = static_cast<std::uint64_t>(file.size() - at);
        }
        if (std::cmp_less(size, header) || std::cmp_greater(size, file.size() - at)) {
            return fail(ErrorCode::Corrupt, "truncated MP4 box");
        }
        if (file.sliced(at + 4, 4) == "mdat") {
            hasher.add(file.sliced(at + header, static_cast<qsizetype>(size) - header));
            found = true;
        }
        at += static_cast<qsizetype>(size);
    }
    if (!found) {
        return fail(ErrorCode::Corrupt, "no MP4 media data");
    }
    return hasher.hex();
}

Result<QByteArray> oggPayload(QByteArrayView file)
{
    Hasher128 hasher;
    QByteArray packet;
    int packets = 0;
    int headerPackets = 0;  // set from the first packet: Opus 2, Vorbis 3
    qsizetype at = 0;
    while (file.size() - at >= 27) {
        if (file.sliced(at, 4) != "OggS") {
            return fail(ErrorCode::Corrupt, "lost Ogg page sync");
        }
        const int segments = static_cast<std::uint8_t>(file[at + 26]);
        if (file.size() - at < 27 + segments) {
            return fail(ErrorCode::Corrupt, "truncated Ogg page");
        }
        qsizetype body = at + 27 + segments;
        for (int i = 0; i < segments; ++i) {
            const int lace = static_cast<std::uint8_t>(file[at + 27 + i]);
            if (file.size() - body < lace) {
                return fail(ErrorCode::Corrupt, "truncated Ogg packet");
            }
            packet.append(file.sliced(body, lace));
            body += lace;
            if (lace < 255) {  // the packet ends here
                if (packets == 0) {
                    headerPackets = packet.startsWith("OpusHead") ? 2 : packet.startsWith("\x01vorbis") ? 3 : 1;
                }
                if (packets >= headerPackets) {
                    hasher.add(packet);
                }
                ++packets;
                packet.clear();
            }
        }
        at = body;
    }
    if (packets <= headerPackets) {
        return fail(ErrorCode::Corrupt, "no Ogg audio packets");
    }
    return hasher.hex();
}

}  // namespace

Container detectContainer(QByteArrayView head) noexcept
{
    if (head.startsWith("OggS")) {
        return Container::Ogg;
    }
    if (head.size() >= 8 && head.sliced(4, 4) == "ftyp") {
        return Container::Mp4;
    }
    if (head.startsWith("ID3")) {
        return Container::Mpeg;
    }
    // MPEG audio frame sync: 11 set bits, a valid layer.
    if (head.size() >= 2 && static_cast<std::uint8_t>(head[0]) == 0xFF &&
        (static_cast<std::uint8_t>(head[1]) & 0xE0U) == 0xE0U && (static_cast<std::uint8_t>(head[1]) & 0x06U) != 0) {
        return Container::Mpeg;
    }
    return Container::Unknown;
}

Result<QByteArray> payloadHash(QByteArrayView file)
{
    switch (detectContainer(file.first(std::min<qsizetype>(file.size(), 16)))) {
    case Container::Mpeg: return mpegPayload(file);
    case Container::Mp4: return mp4Payload(file);
    case Container::Ogg: return oggPayload(file);
    case Container::Unknown: break;
    }
    return fail(ErrorCode::Unsupported, "unknown audio container");
}

Result<QByteArray> payloadHashOfFile(const QString& path)
{
    const Result<QByteArray> data = readWholeFile(path);
    if (!data) {
        return std::unexpected(data.error());
    }
    return payloadHash(*data);
}

}  // namespace noctara::cache
