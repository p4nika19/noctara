#pragma once

#include "media/PlaybackBackend.hpp"

#include <QJsonObject>

#include <cstdint>
#include <optional>

namespace noctara::drm {

class DrmHost;

// Plays protected tracks in the helper: resolves a fresh protected stream for every load, hands it to the helper
// and mirrors the helper's state and position. Nothing of the media passes through Noctara, and nothing is
// cached (cachedRanges() is always empty). There is no gapless hand-over: the controller starts the next track
// when this one ends; setNext() only warms the helper up. A helper that crashed, was restarted for new network
// settings, or was closed while idle resumes the track where it was. GUI thread.
class DrmBackend : public media::IPlaybackBackend {
    Q_OBJECT

public:
    explicit DrmBackend(DrmHost& host, QObject* parent = nullptr);

    void load(const media::PlaybackSource& source, std::int64_t startMs, bool paused) override;
    void setNext(const std::optional<media::PlaybackSource>& source) override;
    void play() override;
    void pause() override;
    void stop() override;
    void seek(std::int64_t ms) override;
    void setGain(double gain) override;
    void setCrossfade(std::int64_t ms) override;

    [[nodiscard]] media::BackendState state() const override { return m_state; }
    [[nodiscard]] std::int64_t position() const override { return m_position; }
    [[nodiscard]] bool buffering() const override { return m_buffering; }
    [[nodiscard]] std::vector<std::pair<double, double>> cachedRanges() const override { return {}; }

private:
    // Resolves a fresh stream and loads it in the helper at m_position (playing if m_wantPlay by then).
    void open();
    void onEvent(const QJsonObject& message);
    void onStopped(bool unexpected);
    void setState(media::BackendState state);
    void setBuffering(bool buffering);
    void fail(const Error& error);
    [[nodiscard]] bool current(const QJsonObject& message) const;

    DrmHost& m_host;
    std::optional<media::PlaybackSource> m_source;
    std::uint64_t m_serial = 0;  // one per open(): stale stream answers and helper events are dropped
    bool m_loaded = false;       // the running helper has the track
    bool m_opening = false;      // the stream is being resolved
    bool m_wantPlay = false;
    bool m_resume = false;  // reload once the helper is ready again
    media::BackendState m_state = media::BackendState::Idle;
    bool m_buffering = false;
    std::int64_t m_position = 0;
    double m_gain = 1.0;
};

}  // namespace noctara::drm
