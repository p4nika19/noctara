#include "cache/PieceMap.hpp"

#include "cache/Hash.hpp"

#include <QDataStream>
#include <QIODevice>

#include <algorithm>
#include <utility>

namespace noctara::cache {

namespace {

constexpr quint32 kMagic = 0x4e43504d;  // "NCPM"
constexpr quint16 kVersion = 1;
constexpr int kMaxPieces = 4 * 1024 * 1024;  // 256 GiB of 64 KiB chunks: anything larger is corrupt

}  // namespace

PieceMap PieceMap::progressive(std::int64_t contentLength, std::int64_t chunkSize)
{
    PieceMap map;
    map.m_layout = Layout::Progressive;
    map.m_chunkSize = chunkSize;
    map.setContentLength(contentLength);
    return map;
}

PieceMap PieceMap::hls(int segments)
{
    PieceMap map;
    map.m_layout = Layout::Hls;
    map.m_pieces.resize(static_cast<std::size_t>(std::max(0, segments)));
    return map;
}

void PieceMap::setContentLength(std::int64_t length)
{
    m_contentLength = length;
    if (length < 0) {
        return;
    }
    const auto count = static_cast<std::size_t>((length + m_chunkSize - 1) / m_chunkSize);
    // Pieces beyond the real end (a stream that turned out shorter) are dropped.
    while (m_pieces.size() > count) {
        unmark(static_cast<int>(m_pieces.size()) - 1);
        m_pieces.pop_back();
    }
    m_pieces.resize(count);
}

int PieceMap::pieceCount() const noexcept
{
    if (m_layout == Layout::Progressive && m_contentLength < 0) {
        return -1;
    }
    return static_cast<int>(m_pieces.size());
}

std::int64_t PieceMap::pieceOffset(int piece) const noexcept
{
    return static_cast<std::int64_t>(piece) * m_chunkSize;
}

std::int64_t PieceMap::pieceSize(int piece) const noexcept
{
    if (m_layout == Layout::Hls) {
        return has(piece) ? m_pieces[static_cast<std::size_t>(piece)].size : -1;
    }
    if (m_contentLength < 0) {
        return has(piece) ? m_pieces[static_cast<std::size_t>(piece)].size : m_chunkSize;
    }
    return std::clamp<std::int64_t>(m_contentLength - pieceOffset(piece), 0, m_chunkSize);
}

int PieceMap::pieceAt(std::int64_t offset) const noexcept
{
    return static_cast<int>(offset / m_chunkSize);
}

bool PieceMap::has(int piece) const noexcept
{
    return piece >= 0 && static_cast<std::size_t>(piece) < m_pieces.size() &&
           m_pieces[static_cast<std::size_t>(piece)].present;
}

std::uint64_t PieceMap::checksum(int piece) const noexcept
{
    return has(piece) ? m_pieces[static_cast<std::size_t>(piece)].checksum : 0;
}

void PieceMap::ensure(int piece)
{
    if (static_cast<std::size_t>(piece) >= m_pieces.size()) {
        m_pieces.resize(static_cast<std::size_t>(piece) + 1);
    }
}

void PieceMap::mark(int piece, std::uint32_t size, std::uint64_t checksum)
{
    if (piece < 0 || piece >= kMaxPieces) {
        return;
    }
    ensure(piece);
    Piece& slot = m_pieces[static_cast<std::size_t>(piece)];
    if (slot.present) {
        m_storedBytes -= slot.size;
    } else {
        ++m_present;
    }
    slot = Piece{size, checksum, true};
    m_storedBytes += size;
}

void PieceMap::unmark(int piece)
{
    if (!has(piece)) {
        return;
    }
    Piece& slot = m_pieces[static_cast<std::size_t>(piece)];
    m_storedBytes -= slot.size;
    --m_present;
    slot = Piece{};
}

void PieceMap::clear()
{
    for (Piece& piece : m_pieces) {
        piece = Piece{};
    }
    m_present = 0;
    m_storedBytes = 0;
}

bool PieceMap::complete() const noexcept
{
    const int count = pieceCount();
    return count >= 0 && m_present == count;
}

std::optional<int> PieceMap::firstMissing(int from) const noexcept
{
    const int count = pieceCount();
    for (int piece = std::max(0, from); count < 0 || piece < count; ++piece) {
        if (!has(piece)) {
            return piece;
        }
    }
    return std::nullopt;
}

int PieceMap::runEnd(int from) const noexcept
{
    const auto size = static_cast<int>(m_pieces.size());
    for (int piece = std::max(0, from); piece < size; ++piece) {
        if (has(piece)) {
            return piece;
        }
    }
    return pieceCount();
}

std::vector<std::pair<std::int64_t, std::int64_t>> PieceMap::ranges() const
{
    std::vector<std::pair<std::int64_t, std::int64_t>> result;
    for (int piece = 0; std::cmp_less(piece, m_pieces.size()); ++piece) {
        if (!has(piece)) {
            continue;
        }
        const std::int64_t first = pieceOffset(piece);
        const std::int64_t last = first + m_pieces[static_cast<std::size_t>(piece)].size;
        if (!result.empty() && result.back().second == first) {
            result.back().second = last;
        } else {
            result.emplace_back(first, last);
        }
    }
    return result;
}

QByteArray PieceMap::serialize() const
{
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << kMagic << kVersion << static_cast<quint8>(m_layout) << static_cast<qint64>(m_chunkSize)
        << static_cast<qint64>(m_contentLength) << static_cast<quint32>(m_pieces.size());
    for (const Piece& piece : m_pieces) {
        out << static_cast<quint8>(piece.present ? 1 : 0) << piece.size << static_cast<quint64>(piece.checksum);
    }
    out << static_cast<quint64>(cache::checksum(data));
    return data;
}

Result<PieceMap> PieceMap::parse(QByteArrayView data)
{
    if (data.size() < 8) {
        return fail(ErrorCode::Corrupt, "piece map too short");
    }
    const QByteArrayView body = data.first(data.size() - 8);
    QByteArray copy = data.toByteArray();
    QDataStream in(&copy, QIODevice::ReadOnly);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint16 version = 0;
    quint8 layout = 0;
    qint64 chunkSize = 0;
    qint64 contentLength = 0;
    quint32 count = 0;
    in >> magic >> version >> layout >> chunkSize >> contentLength >> count;
    if (magic != kMagic || version != kVersion || layout > 1 || chunkSize <= 0 || count > kMaxPieces) {
        return fail(ErrorCode::Corrupt, "bad piece map header");
    }
    PieceMap map;
    map.m_layout = static_cast<Layout>(layout);
    map.m_chunkSize = chunkSize;
    map.m_contentLength = contentLength;
    map.m_pieces.resize(count);
    for (quint32 i = 0; i < count; ++i) {
        quint8 present = 0;
        quint32 size = 0;
        quint64 sum = 0;
        in >> present >> size >> sum;
        if (present != 0) {
            map.mark(static_cast<int>(i), size, sum);
        }
    }
    quint64 trailer = 0;
    in >> trailer;
    if (in.status() != QDataStream::Ok || trailer != cache::checksum(body)) {
        return fail(ErrorCode::Corrupt, "piece map checksum mismatch");
    }
    return map;
}

}  // namespace noctara::cache
