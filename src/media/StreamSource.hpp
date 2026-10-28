#pragma once

#include "cache/AudioCache.hpp"
#include "core/Error.hpp"

#include <QByteArray>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>

namespace noctara::media {

// The bytes of one audio cache entry as the decoder reads them, so every byte comes through the cache (RAM, disk,
// then the network transport with its DNS and proxy rules); the media framework never opens a URL itself.
// Progressive entries are a random-access file. HLS entries are read as one stream: the init segment (if any),
// then the media segments from `firstSegment` on; MP3, ADTS, Ogg and fragmented-MP4 segments concatenate into a
// valid stream, so seeking means opening a new source at another segment. Used on one decoder thread.
class StreamSource {
public:
    // `waiting` (optional) is set while read() waits for the network, for the buffering indicator.
    StreamSource(cache::AudioReaderPtr reader, std::stop_token stop, int firstSegment = 0,
                 std::atomic<bool>* waiting = nullptr);

    // Fills `out` from the current position; 0 at the end of the stream. Blocks while a piece is on its way.
    [[nodiscard]] Result<std::size_t> read(std::span<std::byte> out);
    // Progressive only.
    [[nodiscard]] bool seekable() const;
    [[nodiscard]] Result<void> seek(std::int64_t offset);
    // Progressive: total bytes (-1 until known); HLS: -1.
    [[nodiscard]] std::int64_t size() const;
    [[nodiscard]] std::int64_t position() const noexcept { return m_position; }

    // How long a single piece may take before the read fails with ErrorCode::Timeout.
    static constexpr std::chrono::seconds kPieceTimeout{45};

private:
    [[nodiscard]] Result<QByteArray> fetch(int piece);
    [[nodiscard]] Result<std::size_t> readProgressive(std::span<std::byte> out);
    [[nodiscard]] Result<std::size_t> readHls(std::span<std::byte> out);

    cache::AudioReaderPtr m_reader;
    std::stop_token m_stop;
    bool m_hls = false;
    int m_firstSegment = 0;
    std::int64_t m_position = 0;  // progressive: file offset; HLS: bytes delivered so far
    // The piece being read.
    int m_piece = -1;
    QByteArray m_data;
    std::int64_t m_pieceStart = 0;  // progressive: file offset of m_data
    qsizetype m_within = 0;         // HLS: read position inside m_data
    int m_hlsNext = 0;              // HLS: next piece to load (-1 = before the first)
    std::atomic<bool>* m_waiting = nullptr;
};

}  // namespace noctara::media
