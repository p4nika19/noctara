#pragma once

#include "cache/AudioCache.hpp"
#include "cache/TrackInfo.hpp"
#include "core/Error.hpp"

#include <QObject>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace noctara::media {

// What a backend plays: the cache's self-describing record of the track (key, protocol, DRM flag, metadata) and
// where its stream URL comes from.
struct PlaybackSource {
    cache::TrackInfo info;
    cache::UrlProvider urls;
};

enum class BackendState : std::uint8_t {
    Idle,     // nothing loaded
    Loading,  // opening the stream; no audio yet
    Playing,
    Paused,
    Ended,  // the last source played to its end
    Failed,
};

// A way to play tracks. The native backend decodes regular tracks from the audio cache; protected tracks belong
// to the DRM backend (the Electron helper, slice i). Lives on the GUI thread.
class IPlaybackBackend : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Replaces what plays with `source`, starting at `startMs`; it plays as soon as audio is there unless `paused`.
    virtual void load(const PlaybackSource& source, std::int64_t startMs, bool paused) = 0;
    // The source to follow the current one without a gap (or crossfaded); nullopt forgets it.
    virtual void setNext(const std::optional<PlaybackSource>& source) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(std::int64_t ms) = 0;
    // Linear gain 0…1 (the caller applies the loudness curve).
    virtual void setGain(double gain) = 0;
    virtual void setCrossfade(std::int64_t ms) = 0;

    [[nodiscard]] virtual BackendState state() const = 0;
    [[nodiscard]] virtual std::int64_t position() const = 0;  // ms, as heard
    // Playing (or about to) but waiting for data.
    [[nodiscard]] virtual bool buffering() const = 0;
    // Stored parts of the current track as fractions of it (for the waveform).
    [[nodiscard]] virtual std::vector<std::pair<double, double>> cachedRanges() const = 0;

Q_SIGNALS:
    void stateChanged();
    void bufferingChanged();
    // About every 100 ms while playing, and after seeks.
    void positionChanged();
    // The duration as the stream itself reports it (the API's figure may be rounded).
    void durationKnown(std::int64_t ms);
    // The next source took over and is now the current one.
    void advanced();
    // The current source played to its end with nothing to follow.
    void ended();
    void failed(const noctara::Error& error);
};

}  // namespace noctara::media
