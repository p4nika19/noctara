#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstdint>

namespace noctara::cache {

enum class Container : std::uint8_t { Unknown, Mpeg, Mp4, Ogg };

[[nodiscard]] Container detectContainer(QByteArrayView head) noexcept;

// Hash of the audio data alone, independent of tags: MPEG frames between leading ID3v2 and trailing ID3v1/APE
// tags, the contents of every MP4 'mdat' box, or the Ogg audio packets after the codec headers (page headers,
// sequence numbers and CRCs change when comment pages grow, the packets do not). Tagging must leave it equal.
[[nodiscard]] Result<QByteArray> payloadHashOfFile(const QString& path);
[[nodiscard]] Result<QByteArray> payloadHash(QByteArrayView file);

}  // namespace noctara::cache
