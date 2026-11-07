#pragma once

#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace noctara::cache {

// Which pieces of an entry are stored, with a checksum per piece. Progressive streams are cut into fixed-size
// chunks at fixed offsets (a sparse file plus this bitmap makes partial entries resumable); HLS entries have
// one piece per segment (the init segment, if any, is piece 0).
class PieceMap {
public:
    enum class Layout : std::uint8_t { Progressive, Hls };

    static constexpr std::int64_t kChunkSize = 64 * 1024;

    PieceMap() = default;
    static PieceMap progressive(std::int64_t contentLength, std::int64_t chunkSize = kChunkSize);
    static PieceMap hls(int segments);

    [[nodiscard]] Layout layout() const noexcept { return m_layout; }
    [[nodiscard]] std::int64_t chunkSize() const noexcept { return m_chunkSize; }
    // Progressive: total length in bytes, -1 while unknown (the server sent no length yet).
    [[nodiscard]] std::int64_t contentLength() const noexcept { return m_contentLength; }
    // A length needing more pieces than a map may hold (an absurd server value) is ignored: the length stays as it was.
    void setContentLength(std::int64_t length);
    // Whether a known length (>= 0) fits in a map of this chunk size.
    [[nodiscard]] static bool plausibleLength(std::int64_t length, std::int64_t chunkSize = kChunkSize) noexcept;

    // -1 while a progressive length is unknown.
    [[nodiscard]] int pieceCount() const noexcept;
    [[nodiscard]] std::int64_t pieceOffset(int piece) const noexcept;  // progressive only
    // Progressive: the expected size of a piece (the last one may be short), -1 if unknown; HLS: stored size.
    [[nodiscard]] std::int64_t pieceSize(int piece) const noexcept;
    [[nodiscard]] int pieceAt(std::int64_t offset) const noexcept;  // progressive only

    [[nodiscard]] bool has(int piece) const noexcept;
    [[nodiscard]] std::uint64_t checksum(int piece) const noexcept;
    void mark(int piece, std::uint32_t size, std::uint64_t checksum);
    void unmark(int piece);
    void clear();

    [[nodiscard]] int presentCount() const noexcept { return m_present; }
    // Every piece is stored. Never true while the length (progressive) or the playlist (HLS, no pieces) is unknown.
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::int64_t storedBytes() const noexcept { return m_storedBytes; }
    // The first missing piece at or after `from` (pieces past a still unknown end count as missing).
    [[nodiscard]] std::optional<int> firstMissing(int from = 0) const noexcept;
    // The first present piece after `from`, or pieceCount() (or -1 if the count is unknown).
    [[nodiscard]] int runEnd(int from) const noexcept;
    // Stored byte ranges [first, last) of a progressive stream, merged.
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> ranges() const;

    [[nodiscard]] QByteArray serialize() const;
    [[nodiscard]] static Result<PieceMap> parse(QByteArrayView data);

private:
    struct Piece {
        std::uint32_t size = 0;
        std::uint64_t checksum = 0;
        bool present = false;
    };

    void ensure(int piece);

    Layout m_layout = Layout::Progressive;
    std::int64_t m_chunkSize = kChunkSize;
    std::int64_t m_contentLength = -1;
    std::vector<Piece> m_pieces;
    int m_present = 0;
    std::int64_t m_storedBytes = 0;
};

}  // namespace noctara::cache
