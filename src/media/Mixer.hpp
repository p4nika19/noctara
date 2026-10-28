#pragma once

#include "media/Decoder.hpp"
#include "media/PcmRing.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace noctara::media {

// Renders the playing voice into the device buffer on the audio thread. A voice armed as "next" takes over the
// moment the current one runs out (gapless), or blends in over its last frames when a crossfade is set (equal
// power). Volume and pause changes are ramped so they never click; a voice resuming after a gap (seek, network
// stall) fades in briefly. Control calls come from the backend's thread; render() from the audio thread.
class Mixer {
public:
    explicit Mixer(AudioFormat format);

    // Audio thread: fills `out` (interleaved frames of the format).
    void render(std::span<float> out);

    // Replaces both voices at once (a new track, or stop with nullptrs).
    void setVoices(std::shared_ptr<PcmRing> current, std::shared_ptr<PcmRing> next);
    // Replaces the next voice, unless the armed one took over after `seenAdvances` (then nothing changes and the
    // caller catches up with advances() first).
    [[nodiscard]] bool replaceNext(std::shared_ptr<PcmRing> ring, std::uint64_t seenAdvances);
    // The current voice jumped (a seek): a crossfade in progress is abandoned.
    void interrupt();
    void setCrossfadeFrames(std::int64_t frames);
    void setGain(float gain);  // linear
    void setPaused(bool paused);

    // How often a next voice took over so far.
    [[nodiscard]] std::uint64_t advances() const noexcept { return m_advances.load(); }
    // The current voice ran out with nothing to follow.
    [[nodiscard]] bool ended() const noexcept { return m_ended.load(); }
    // Paused and faded out: the device may be suspended.
    [[nodiscard]] bool silent() const noexcept { return m_silent.load(); }
    // The current voice could not deliver in time during the last render.
    [[nodiscard]] bool starved() const noexcept { return m_starved.load(); }
    [[nodiscard]] AudioFormat format() const noexcept { return m_format; }

    static constexpr int kRampMs = 15;  // pause/resume, volume and fade-in after a gap

private:
    void renderBlock(float* out, std::int64_t frames);
    // Reads from the current voice (crossfading into the next one near its end); returns frames delivered.
    std::int64_t pull(float* out, std::int64_t frames);
    void promote();

    const AudioFormat m_format;
    const std::int64_t m_rampFrames;
    std::mutex m_mutex;
    std::shared_ptr<PcmRing> m_current;
    std::shared_ptr<PcmRing> m_next;
    std::int64_t m_crossfadeFrames = 0;
    std::int64_t m_fadeLength = 0;  // > 0 while a crossfade runs
    float m_targetGain = 1.0F;
    float m_gain = 1.0F;
    bool m_paused = false;
    float m_pauseGain = 1.0F;
    float m_fadeIn = 1.0F;   // after a gap
    std::vector<float> m_a;  // scratch for the current and next voice
    std::vector<float> m_b;
    std::atomic<std::uint64_t> m_advances{0};
    std::atomic<bool> m_ended{false};
    std::atomic<bool> m_silent{false};
    std::atomic<bool> m_starved{false};
};

}  // namespace noctara::media
