#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

namespace noctara::media {

// Decoded audio on its way from a voice's decoder thread (the producer) to the audio callback (the consumer):
// interleaved float frames in a fixed ring. The producer waits while the ring is full; the consumer only takes a
// short lock and never waits. The ring also knows the stream position of the next frame it hands out, so the
// played position follows what was actually consumed.
class PcmRing {
public:
    PcmRing(int channels, std::int64_t capacityFrames);

    // Producer. Waits for room until everything is written or `stop` is requested (then returns false).
    bool write(std::span<const float> samples, const std::stop_token& stop);
    // Producer: the stream ends after the frames written so far.
    void finish();
    // Producer: drops everything and continues at stream frame `frame` (after a seek); also ends a hold().
    void reset(std::int64_t frame);
    // Any thread: the consumer gets nothing until the next reset() (a seek is on its way).
    void hold();

    // Consumer: copies up to out.size() / channels frames; returns the number of frames copied.
    std::int64_t read(std::span<float> out);

    [[nodiscard]] std::int64_t available() const;
    [[nodiscard]] bool finished() const;
    [[nodiscard]] bool drained() const;  // finished and nothing left
    [[nodiscard]] bool held() const;
    [[nodiscard]] std::int64_t position() const;  // stream frame of the next frame read
    [[nodiscard]] std::int64_t origin() const;    // the frame of the last reset()
    [[nodiscard]] int channels() const noexcept { return m_channels; }
    [[nodiscard]] std::int64_t capacity() const noexcept { return m_capacity; }

private:
    const int m_channels;
    const std::int64_t m_capacity;  // frames
    mutable std::mutex m_mutex;
    std::condition_variable_any m_space;
    std::vector<float> m_data;
    std::int64_t m_head = 0;  // frame index (mod capacity) of the next frame to read
    std::int64_t m_size = 0;  // frames stored
    std::int64_t m_position = 0;
    std::int64_t m_origin = 0;
    bool m_finished = false;
    bool m_held = false;
};

}  // namespace noctara::media
