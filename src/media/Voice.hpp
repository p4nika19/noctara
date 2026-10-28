#pragma once

#include "cache/AudioCache.hpp"
#include "core/Error.hpp"
#include "media/Decoder.hpp"
#include "media/PcmRing.hpp"
#include "media/StreamSource.hpp"

#include <QObject>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace noctara::media {

// One track being played natively: decodes its audio cache entry on a thread of its own into a PcmRing that the
// mixer drains. Seeking restarts decoding at the target (progressive: a demuxer seek; HLS: a new source at the
// segment holding the target), dropping audio before it so the position is exact. Created and controlled on one
// thread (the backend's); its signals are delivered there.
class Voice : public QObject {
    Q_OBJECT

public:
    Voice(cache::AudioReaderPtr reader, AudioFormat format, std::int64_t startMs, std::int64_t ringFrames,
          QObject* parent = nullptr);
    ~Voice() override;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    Voice(Voice&&) = delete;
    Voice& operator=(Voice&&) = delete;

    [[nodiscard]] const std::shared_ptr<PcmRing>& ring() const noexcept { return m_ring; }
    [[nodiscard]] const cache::AudioReaderPtr& reader() const noexcept { return m_reader; }
    [[nodiscard]] AudioFormat format() const noexcept { return m_format; }
    // The mixer gets nothing until decoding continues at `ms`.
    void seek(std::int64_t ms);
    // Waiting for the network right now.
    [[nodiscard]] bool waiting() const noexcept { return m_waiting.load(std::memory_order_relaxed); }
    // ready() was delivered.
    [[nodiscard]] bool isReady() const noexcept { return m_ready; }

    [[nodiscard]] std::int64_t toFrames(std::int64_t ms) const noexcept;
    [[nodiscard]] std::int64_t toMs(std::int64_t frames) const noexcept;

Q_SIGNALS:
    // The first audio is decoded (once); `durationMs` as the container reports it.
    void ready(std::optional<std::int64_t> durationMs);
    void failed(const noctara::Error& error);

private:
    void run();
    // Waits until a seek or shutdown stops `token`.
    void park(const std::stop_token& token);
    [[nodiscard]] Result<std::unique_ptr<Decoder>> openAt(StreamSource& source, std::int64_t frame);
    [[nodiscard]] std::pair<int, std::int64_t> segmentFor(std::int64_t frame, const std::stop_token& token) const;

    cache::AudioReaderPtr m_reader;
    AudioFormat m_format;
    std::shared_ptr<PcmRing> m_ring;
    std::atomic<bool> m_waiting{false};
    bool m_ready = false;  // owner thread

    std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::stop_source m_operation;  // stopped by a seek or by shutdown
    std::optional<std::int64_t> m_seekTo;
    bool m_quit = false;
    std::thread m_thread;  // last: started after everything above exists
};

}  // namespace noctara::media
