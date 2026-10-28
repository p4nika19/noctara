#include "media/Voice.hpp"

#include "core/Handoff.hpp"
#include "core/Log.hpp"

#include <vector>

namespace noctara::media {

Voice::Voice(cache::AudioReaderPtr reader, AudioFormat format, std::int64_t startMs, std::int64_t ringFrames,
             QObject* parent)
    : QObject(parent)
    , m_reader(std::move(reader))
    , m_format(format)
    , m_ring(std::make_shared<PcmRing>(format.channels, ringFrames))
{
    if (startMs > 0) {
        m_seekTo = toFrames(startMs);
    }
    m_ring->hold();
    connect(this, &Voice::ready, this, [this] { m_ready = true; });
    m_thread = std::thread([this] { run(); });
}

Voice::~Voice()
{
    {
        const std::scoped_lock lock(m_mutex);
        m_quit = true;
        m_operation.request_stop();
    }
    m_wake.notify_all();
    m_thread.join();
}

std::int64_t Voice::toFrames(std::int64_t ms) const noexcept
{
    return ms * m_format.sampleRate / 1000;
}

std::int64_t Voice::toMs(std::int64_t frames) const noexcept
{
    return frames * 1000 / m_format.sampleRate;
}

void Voice::seek(std::int64_t ms)
{
    {
        const std::scoped_lock lock(m_mutex);
        m_seekTo = toFrames(std::max<std::int64_t>(ms, 0));
        m_ring->hold();
        m_operation.request_stop();
    }
    m_wake.notify_all();
}

void Voice::park(const std::stop_token& token)
{
    std::unique_lock lock(m_mutex);
    m_wake.wait(lock, token, [] { return false; });
}

std::pair<int, std::int64_t> Voice::segmentFor(std::int64_t frame, const std::stop_token& token) const
{
    std::optional<cache::HlsPlaylist> playlist = m_reader->playlist();
    if (!playlist) {
        // The playlist comes with the first piece.
        if (const Result<QByteArray> first = m_reader->read(0, token, StreamSource::kPieceTimeout); !first) {
            return {0, 0};  // the decoder's own read reports the failure
        }
        playlist = m_reader->playlist();
    }
    if (!playlist) {
        return {0, 0};
    }
    double start = 0;
    const double target = static_cast<double>(frame) / m_format.sampleRate;
    for (std::size_t i = 0; i < playlist->segments.size(); ++i) {
        const double end = start + playlist->segments[i].duration;
        if (target < end || i + 1 == playlist->segments.size()) {
            return {static_cast<int>(i), static_cast<std::int64_t>(start * m_format.sampleRate)};
        }
        start = end;
    }
    return {0, 0};
}

void Voice::run()
{
    bool announced = false;
    std::vector<float> chunk;
    while (true) {
        std::stop_token token;
        std::int64_t target = 0;
        {
            const std::scoped_lock lock(m_mutex);
            if (m_quit) {
                return;
            }
            m_operation = std::stop_source();
            token = m_operation.get_token();
            target = m_seekTo.value_or(0);
            m_seekTo.reset();
        }

        const bool hls = m_reader->protocol() == cache::StreamProtocol::Hls;
        const auto [segment, segmentFrame] = hls ? segmentFor(target, token) : std::pair<int, std::int64_t>{0, 0};
        StreamSource source(m_reader, token, segment, &m_waiting);
        Result<std::unique_ptr<Decoder>> decoder =
            Decoder::open(source, m_format, {.frame = segmentFrame, .trimTo = hls ? target : 0});
        if (decoder && !hls && target > 0) {
            if (Result<void> sought = (*decoder)->seek(target); !sought) {
                decoder = std::unexpected(sought.error());
            }
        }
        if (!decoder) {
            if (!token.stop_requested()) {
                qCWarning(lcMedia).noquote() << "cannot decode" << m_reader->info().key.trackId << ":"
                                             << QString::fromStdString(decoder.error().describe());
                handoff::post(this, [this, error = decoder.error()] { Q_EMIT failed(error); });
                park(token);
            }
            continue;
        }
        bool stale = false;
        {
            // A seek that arrived meanwhile must keep the ring on hold.
            const std::scoped_lock lock(m_mutex);
            stale = token.stop_requested();
            if (!stale) {
                m_ring->reset(target);
            }
        }
        if (stale) {
            continue;
        }
        const std::optional<std::int64_t> duration = (*decoder)->durationMs();
        bool interrupted = false;
        while (!interrupted) {
            chunk.clear();
            Result<bool> more = (*decoder)->decode(chunk);
            if (!more) {
                if (!token.stop_requested()) {
                    qCWarning(lcMedia).noquote() << "decoding" << m_reader->info().key.trackId
                                                 << "stopped:" << QString::fromStdString(more.error().describe());
                    // The ring is not finished: the mixer waits (buffering) instead of moving on to the next track.
                    handoff::post(this, [this, error = more.error()] { Q_EMIT failed(error); });
                    park(token);
                }
                break;
            }
            if (!*more) {
                m_ring->finish();
                if (!announced) {
                    announced = true;
                    handoff::post(this, [this, duration] { Q_EMIT ready(duration); });
                }
                park(token);
                break;
            }
            interrupted = !m_ring->write(chunk, token);
            if (!announced) {
                announced = true;
                handoff::post(this, [this, duration] { Q_EMIT ready(duration); });
            }
        }
    }
}

}  // namespace noctara::media
