#pragma once

#include "media/AudioOutput.hpp"
#include "media/Mixer.hpp"
#include "media/PlaybackBackend.hpp"
#include "media/Voice.hpp"

#include <QTimer>

#include <memory>
#include <optional>

namespace noctara::cache {
class AudioCache;
}

namespace noctara::media {

// Plays regular (non-DRM) tracks: each source is opened in the audio cache and decoded by a Voice; the Mixer feeds
// the audio device. The next source is opened ahead of time, so it starts without a gap (or crossfades). The
// device is opened on the first track and suspended while paused or ended; it follows the system's default
// output. GUI thread.
class NativeBackend : public IPlaybackBackend {
    Q_OBJECT

public:
    NativeBackend(cache::AudioCache& cache, IAudioOutput& output, QObject* parent = nullptr);
    ~NativeBackend() override;
    NativeBackend(const NativeBackend&) = delete;
    NativeBackend& operator=(const NativeBackend&) = delete;
    NativeBackend(NativeBackend&&) = delete;
    NativeBackend& operator=(NativeBackend&&) = delete;

    void load(const PlaybackSource& source, std::int64_t startMs, bool paused) override;
    void setNext(const std::optional<PlaybackSource>& source) override;
    void play() override;
    void pause() override;
    void stop() override;
    void seek(std::int64_t ms) override;
    void setGain(double gain) override;
    void setCrossfade(std::int64_t ms) override;

    [[nodiscard]] BackendState state() const override { return m_state; }
    [[nodiscard]] std::int64_t position() const override;
    [[nodiscard]] bool buffering() const override { return m_buffering; }
    [[nodiscard]] std::vector<std::pair<double, double>> cachedRanges() const override;

    // Decoded audio kept ahead of the device beyond the crossfade.
    static constexpr std::int64_t kRingMs = 2000;
    static constexpr std::chrono::milliseconds kTick{100};

private:
    struct Slot {
        std::unique_ptr<Voice> voice;
        PlaybackSource source;     // to open it again after a failure
        std::uint64_t serial = 0;  // matches the open request; a newer request makes older answers stale
        bool opening = false;
    };

    void open(Slot& slot, const PlaybackSource& source);
    void attach(Slot& slot, const cache::AudioReaderPtr& reader);
    // Opens the current source again after its cache entry was reset under `voice`.
    void reopenCurrent(const Voice* voice, const Error& error);
    // Starts the device. A device format other than the voices' decodes them again, from `resumeAt` (default: the
    // position).
    [[nodiscard]] bool ensureOutput(std::optional<std::int64_t> resumeAt = std::nullopt);
    void restartOutput();
    // Takes over the moves the mixer made to the next voice; `report` emits advanced() for each. True if any.
    bool syncAdvances(bool report = true);
    void armNext();
    void dropNext();
    [[nodiscard]] Slot* slotFor(std::uint64_t serial);
    void tick();
    void setState(BackendState state);
    void setBuffering(bool buffering);
    void fail(const Error& error);
    [[nodiscard]] std::int64_t ringFrames() const;

    cache::AudioCache& m_cache;
    IAudioOutput& m_output;
    std::optional<AudioFormat> m_format;
    std::unique_ptr<Mixer> m_mixer;
    Slot m_current;
    Slot m_next;
    std::uint64_t m_serial = 0;
    std::uint64_t m_advancesSeen = 0;
    BackendState m_state = BackendState::Idle;
    bool m_paused = false;
    bool m_buffering = false;
    bool m_suspended = false;
    bool m_reopened = false;          // the current source was opened again after a reset (once per load)
    std::int64_t m_startMs = 0;       // where the current source starts (until it delivers)
    std::int64_t m_seekMs = -1;       // a seek on its way
    std::int64_t m_lastPosition = 0;  // for positionChanged
    std::int64_t m_crossfadeMs = 0;
    double m_gain = 1.0;
    QTimer m_ticker;
};

}  // namespace noctara::media
