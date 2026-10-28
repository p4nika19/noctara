#include "media/StreamSource.hpp"

#include <algorithm>
#include <cstring>

namespace noctara::media {

StreamSource::StreamSource(cache::AudioReaderPtr reader, std::stop_token stop, int firstSegment,
                           std::atomic<bool>* waiting)
    : m_reader(std::move(reader))
    , m_stop(std::move(stop))
    , m_hls(m_reader->protocol() == cache::StreamProtocol::Hls)
    , m_firstSegment(std::max(firstSegment, 0))
    , m_hlsNext(-1)
    , m_waiting(waiting)
{
}

bool StreamSource::seekable() const
{
    return !m_hls;
}

std::int64_t StreamSource::size() const
{
    return m_hls ? -1 : m_reader->contentLength();
}

Result<QByteArray> StreamSource::fetch(int piece)
{
    if (const cache::AudioReader::Piece ready = m_reader->tryRead(piece);
        ready.status == cache::AudioReader::Status::Ready) {
        return ready.data;
    }
    if (m_waiting != nullptr) {
        m_waiting->store(true, std::memory_order_relaxed);
    }
    Result<QByteArray> data = m_reader->read(piece, m_stop, kPieceTimeout);
    if (m_waiting != nullptr) {
        m_waiting->store(false, std::memory_order_relaxed);
    }
    return data;
}

Result<void> StreamSource::seek(std::int64_t offset)
{
    if (m_hls || offset < 0) {
        return fail(ErrorCode::Unsupported, "seek in a segmented stream");
    }
    m_position = offset;
    return {};
}

Result<std::size_t> StreamSource::read(std::span<std::byte> out)
{
    if (out.empty()) {
        return 0;
    }
    return m_hls ? readHls(out) : readProgressive(out);
}

Result<std::size_t> StreamSource::readProgressive(std::span<std::byte> out)
{
    const std::int64_t length = m_reader->contentLength();
    if (length >= 0 && m_position >= length) {
        return 0;
    }
    const auto within = [&] {
        return m_position - m_pieceStart;
    };
    if (m_piece < 0 || within() < 0 || within() >= m_data.size()) {
        const int piece = m_reader->pieceAt(m_position);
        Result<QByteArray> data = fetch(piece);
        if (!data) {
            if (data.error().code == ErrorCode::InvalidArgument) {
                return 0;  // past the end
            }
            return std::unexpected(data.error());
        }
        m_piece = piece;
        m_data = std::move(*data);
        m_pieceStart = m_reader->pieceOffset(piece);
        if (within() < 0 || within() >= m_data.size()) {
            return 0;
        }
    }
    const auto take = std::min<std::int64_t>(static_cast<std::int64_t>(out.size()), m_data.size() - within());
    std::memcpy(out.data(), m_data.constData() + within(), static_cast<std::size_t>(take));
    m_position += take;
    return static_cast<std::size_t>(take);
}

Result<std::size_t> StreamSource::readHls(std::span<std::byte> out)
{
    while (m_piece < 0 || m_within >= m_data.size()) {
        int piece = m_hlsNext;
        if (m_hlsNext < 0) {
            // The playlist is known once the first piece was asked for. Its init segment (EXT-X-MAP) always comes
            // first; the media segments start at m_firstSegment.
            std::optional<QByteArray> first;
            if (!m_reader->playlist()) {
                Result<QByteArray> data = fetch(0);
                if (!data) {
                    return std::unexpected(data.error());
                }
                first = std::move(*data);
            }
            const std::optional<cache::HlsPlaylist> playlist = m_reader->playlist();
            const int init = playlist && playlist->init ? 1 : 0;
            const int start = init + m_firstSegment;
            if (first && (init == 1 || start == 0)) {
                m_piece = 0;
                m_data = std::move(*first);
                m_within = 0;
                m_hlsNext = init == 1 ? start : 1;
                continue;
            }
            piece = init == 1 ? 0 : start;
            m_hlsNext = init == 1 ? start : start + 1;
        } else {
            ++m_hlsNext;
        }
        const int count = m_reader->pieceCount();
        if (count >= 0 && piece >= count) {
            return 0;
        }
        Result<QByteArray> data = fetch(piece);
        if (!data) {
            if (data.error().code == ErrorCode::InvalidArgument) {
                return 0;
            }
            return std::unexpected(data.error());
        }
        m_piece = piece;
        m_data = std::move(*data);
        m_within = 0;
    }
    const auto take = std::min<qsizetype>(static_cast<qsizetype>(out.size()), m_data.size() - m_within);
    std::memcpy(out.data(), m_data.constData() + m_within, static_cast<std::size_t>(take));
    m_within += take;
    m_position += take;
    return static_cast<std::size_t>(take);
}

}  // namespace noctara::media
