#include "media/NativeBackend.hpp"

#include "cache/AudioCache.hpp"
#include "core/Log.hpp"

namespace noctara::media {

NativeBackend::NativeBackend(cache::AudioCache& cache, IAudioOutput& output, QObject* parent)
    : IPlaybackBackend(parent)
    , m_cache(cache)
    , m_output(output)
{
    m_ticker.setInterval(kTick);
    connect(&m_ticker, &QTimer::timeout, this, &NativeBackend::tick);
    m_output.setLostHandler([this] { restartOutput(); });
}

NativeBackend::~NativeBackend()
{
    m_output.setLostHandler({});
    m_output.stop();  // no render calls from here on; the voices and the mixer go with the members
}

std::int64_t NativeBackend::ringFrames() const
{
    const int rate = m_format ? m_format->sampleRate : 48000;
    return static_cast<std::int64_t>(rate) * (kRingMs + m_crossfadeMs) / 1000;
}

bool NativeBackend::ensureOutput(std::optional<std::int64_t> resumeAt)
{
    if (m_output.running()) {
        return true;
    }
    Result<AudioFormat> format = m_output.preferredFormat();
    if (!format) {
        fail(format.error());
        return false;
    }
    cache::AudioReaderPtr current;
    cache::AudioReaderPtr next;
    if (!m_format || *m_format != *format) {
        // Voices decode for one format: a device with another one decodes again, from where playback was.
        const std::int64_t at = resumeAt.value_or(position());
        current = m_current.voice ? m_current.voice->reader() : nullptr;
        next = m_next.voice ? m_next.voice->reader() : nullptr;
        if (m_mixer) {
            m_mixer->setVoices(nullptr, nullptr);
        }
        m_current.voice.reset();
        m_next.voice.reset();
        if (current) {
            m_startMs = at;
        }
        m_mixer = std::make_unique<Mixer>(*format);
        m_format = *format;
        m_advancesSeen = 0;
        m_mixer->setGain(static_cast<float>(m_gain));
        m_mixer->setCrossfadeFrames(m_crossfadeMs * format->sampleRate / 1000);
        m_mixer->setPaused(m_paused);
    }
    if (Result<void> started =
            m_output.start(*m_format, [mixer = m_mixer.get()](std::span<float> out) { mixer->render(out); });
        !started) {
        fail(started.error());
        return false;
    }
    m_suspended = false;
    if (current) {
        attach(m_current, current);
    }
    if (next) {
        attach(m_next, next);
    }
    return true;
}

void NativeBackend::restartOutput()
{
    const std::int64_t at = position();  // what was heard: the device's latency is gone once it stops
    m_output.stop();
    if (!ensureOutput(at)) {
        return;
    }
    if (m_paused || m_state == BackendState::Ended) {
        m_output.suspend();
        m_suspended = true;
    }
}

NativeBackend::Slot* NativeBackend::slotFor(std::uint64_t serial)
{
    if (m_current.serial == serial) {
        return &m_current;
    }
    if (m_next.serial == serial) {
        return &m_next;
    }
    return nullptr;
}

void NativeBackend::open(Slot& slot, const PlaybackSource& source)
{
    slot.voice.reset();
    slot.source = source;
    slot.serial = ++m_serial;
    slot.opening = true;
    m_cache.open(source.info, source.urls, this, [this, serial = slot.serial](Result<cache::AudioReaderPtr> reader) {
        Slot* target = slotFor(serial);
        if (target == nullptr) {
            return;  // superseded
        }
        target->opening = false;
        if (!reader) {
            if (target == &m_current) {
                fail(reader.error());
            } else {
                qCInfo(lcMedia).noquote()
                    << "the next track could not be prepared:" << QString::fromStdString(reader.error().describe());
                m_next = {};
            }
            return;
        }
        attach(*target, *reader);
    });
}

void NativeBackend::attach(Slot& slot, const cache::AudioReaderPtr& reader)
{
    if (!ensureOutput() || !m_format) {
        return;
    }
    const bool current = &slot == &m_current;
    slot.voice = std::make_unique<Voice>(reader, *m_format, current ? m_startMs : 0, ringFrames());
    const Voice* voice = slot.voice.get();
    connect(voice, &Voice::ready, this, [this, voice](std::optional<std::int64_t> duration) {
        if (m_current.voice.get() != voice) {
            return;
        }
        if (duration) {
            Q_EMIT durationKnown(*duration);
        }
        if (m_state == BackendState::Loading) {
            setState(m_paused ? BackendState::Paused : BackendState::Playing);
        }
    });
    connect(voice, &Voice::failed, this, [this, voice](const Error& error) {
        if (m_current.voice.get() == voice && error.code == ErrorCode::ContentChanged) {
            // The cache reset the entry: this voice's reader stays bound to the old content, so the source is
            // opened again (later: a voice is not destroyed from inside its own signal).
            QMetaObject::invokeMethod(
                this, [this, voice, error] { reopenCurrent(voice, error); }, Qt::QueuedConnection);
            return;
        }
        if (m_current.voice.get() == voice) {
            fail(error);
            return;
        }
        // Dropped later: a voice is not destroyed from inside its own signal.
        QMetaObject::invokeMethod(
            this,
            [this, voice] {
                if (m_next.voice.get() == voice) {
                    qCInfo(lcMedia) << "the next track failed to start; it is opened again when it plays";
                    dropNext();
                }
            },
            Qt::QueuedConnection);
    });
    if (current) {
        m_mixer->setVoices(voice->ring(), m_next.voice ? m_next.voice->ring() : nullptr);
        m_advancesSeen = m_mixer->advances();
    } else {
        armNext();
    }
}

void NativeBackend::reopenCurrent(const Voice* voice, const Error& error)
{
    if (m_current.voice.get() != voice) {
        return;
    }
    const std::int64_t at = position();
    if (m_mixer) {
        m_mixer->setVoices(nullptr, m_next.voice ? m_next.voice->ring() : nullptr);
        m_advancesSeen = m_mixer->advances();
    }
    m_current.voice.reset();
    m_startMs = at;
    m_seekMs = -1;
    if (m_reopened) {
        fail(error);  // changed again: play() opens it once more
        return;
    }
    m_reopened = true;
    open(m_current, PlaybackSource(m_current.source));
}

bool NativeBackend::syncAdvances(bool report)
{
    if (!m_mixer) {
        return false;
    }
    const std::uint64_t advances = m_mixer->advances();
    const bool any = m_advancesSeen < advances;
    while (m_advancesSeen < advances) {
        ++m_advancesSeen;
        // The mixer reads the next voice now; the old one is no longer used.
        m_current = std::move(m_next);
        m_next = {};
        m_startMs = 0;
        m_seekMs = -1;
        if (report) {
            Q_EMIT advanced();
        }
    }
    return any;
}

void NativeBackend::armNext()
{
    while (m_mixer && !m_mixer->replaceNext(m_next.voice ? m_next.voice->ring() : nullptr, m_advancesSeen)) {
        syncAdvances();
    }
}

void NativeBackend::dropNext()
{
    syncAdvances();
    while (m_mixer && !m_mixer->replaceNext(nullptr, m_advancesSeen)) {
        syncAdvances();
    }
    m_next = {};
}

void NativeBackend::load(const PlaybackSource& source, std::int64_t startMs, bool paused)
{
    m_paused = paused;
    // The caller has chosen what plays now: a move the mixer made since the last tick is not reported, or it would
    // move the caller past the track it is loading.
    const bool movedOn = syncAdvances(false);
    m_seekMs = -1;
    m_startMs = startMs;
    m_reopened = false;
    const bool playingIt = movedOn && startMs == 0 && m_current.source.info.key == source.info.key &&
                           (m_current.voice || m_current.opening);
    const bool preloaded =
        playingIt || (m_next.source.info.key == source.info.key && (m_next.voice || m_next.opening) && startMs == 0);
    if (playingIt) {
        // The mixer already plays it (gaplessly after the previous one): it goes on as it is.
    } else if (preloaded) {
        // The track prepared to follow: take it over as it is.
        if (m_mixer) {
            m_mixer->setVoices(m_next.voice ? m_next.voice->ring() : nullptr, nullptr);
            m_advancesSeen = m_mixer->advances();
        }
        m_current = std::move(m_next);
        m_next = {};
    } else {
        if (m_mixer) {
            m_mixer->setVoices(nullptr, nullptr);
            m_advancesSeen = m_mixer->advances();
        }
        m_current = {};
        m_current.source = source;  // play() opens it again if the device cannot start below
        m_next = {};
    }
    const bool ready = m_current.voice && m_current.voice->isReady();
    setState(ready ? (paused ? BackendState::Paused : BackendState::Playing) : BackendState::Loading);
    if (!ensureOutput()) {
        return;
    }
    m_mixer->setPaused(paused);
    if (!paused && m_suspended) {
        m_output.resume();
        m_suspended = false;
    }
    if (!preloaded) {
        open(m_current, source);
    }
    Q_EMIT positionChanged();
    m_ticker.start();
}

void NativeBackend::setNext(const std::optional<PlaybackSource>& source)
{
    syncAdvances();
    if (!source) {
        dropNext();
        return;
    }
    if (m_next.source.info.key == source->info.key && (m_next.voice || m_next.opening)) {
        return;
    }
    dropNext();
    if (!m_format || source->info.drm) {
        return;  // nothing plays yet, or not a source for this backend
    }
    open(m_next, *source);
}

void NativeBackend::play()
{
    if (m_state == BackendState::Idle) {
        return;
    }
    m_paused = false;
    if (m_state == BackendState::Failed || m_state == BackendState::Ended) {
        // Again from where it stopped (a network failure), or from the start (after the end).
        const std::int64_t at = m_state == BackendState::Ended ? 0 : position();
        setState(BackendState::Loading);
        if (m_current.voice) {
            seek(at);
        } else if (!m_current.opening && !m_current.source.info.key.trackId.isEmpty()) {
            m_startMs = at;
            if (ensureOutput()) {
                open(m_current, PlaybackSource(m_current.source));
            }
        }
    }
    if (!ensureOutput()) {
        return;
    }
    m_mixer->setPaused(false);
    if (m_suspended) {
        m_output.resume();
        m_suspended = false;
    }
    if (m_state == BackendState::Paused) {
        setState(m_current.voice && m_current.voice->isReady() ? BackendState::Playing : BackendState::Loading);
    }
    m_ticker.start();
}

void NativeBackend::pause()
{
    if (m_state != BackendState::Playing && m_state != BackendState::Loading) {
        return;
    }
    m_paused = true;
    if (m_mixer) {
        m_mixer->setPaused(true);
    }
    setState(BackendState::Paused);
    setBuffering(false);
}

void NativeBackend::stop()
{
    ++m_serial;
    if (m_mixer) {
        m_mixer->setVoices(nullptr, nullptr);
        m_advancesSeen = m_mixer->advances();
    }
    m_current = {};
    m_next = {};
    m_output.stop();
    m_suspended = false;
    m_ticker.stop();
    m_startMs = 0;
    m_seekMs = -1;
    setBuffering(false);
    setState(BackendState::Idle);
}

void NativeBackend::seek(std::int64_t ms)
{
    ms = std::max<std::int64_t>(ms, 0);
    syncAdvances();
    if (!m_current.voice) {
        m_startMs = ms;  // still opening: start there
        Q_EMIT positionChanged();
        return;
    }
    m_seekMs = ms;
    if (m_mixer) {
        m_mixer->interrupt();
    }
    m_current.voice->seek(ms);
    if (m_state == BackendState::Ended || m_state == BackendState::Failed) {
        // After a failure the voice decodes again from the target: Loading until it delivers (tick()).
        const BackendState playing = m_state == BackendState::Failed ? BackendState::Loading : BackendState::Playing;
        setState(m_paused ? BackendState::Paused : playing);
        if (!m_paused && m_suspended) {
            m_output.resume();
            m_suspended = false;
        }
    }
    Q_EMIT positionChanged();
    m_ticker.start();
}

void NativeBackend::setGain(double gain)
{
    m_gain = std::clamp(gain, 0.0, 1.0);
    if (m_mixer) {
        m_mixer->setGain(static_cast<float>(m_gain));
    }
}

void NativeBackend::setCrossfade(std::int64_t ms)
{
    m_crossfadeMs = std::max<std::int64_t>(ms, 0);
    if (m_mixer && m_format) {
        m_mixer->setCrossfadeFrames(m_crossfadeMs * m_format->sampleRate / 1000);
    }
}

std::int64_t NativeBackend::position() const
{
    if (m_seekMs >= 0) {
        return m_seekMs;
    }
    if (!m_current.voice) {
        return m_startMs;
    }
    const PcmRing& ring = *m_current.voice->ring();
    if (ring.held()) {
        return m_startMs;
    }
    const std::int64_t latency = m_output.running() ? m_output.latencyFrames() : 0;
    return m_current.voice->toMs(std::max(ring.origin(), ring.position() - latency));
}

std::vector<std::pair<double, double>> NativeBackend::cachedRanges() const
{
    return m_current.voice ? m_current.voice->reader()->cachedFractions() : std::vector<std::pair<double, double>>{};
}

void NativeBackend::tick()
{
    syncAdvances();
    if (m_seekMs >= 0 && m_current.voice && !m_current.voice->ring()->held()) {
        m_seekMs = -1;
    }
    if (m_mixer && m_mixer->ended() && m_state == BackendState::Playing) {
        setState(BackendState::Ended);
        m_output.suspend();
        m_suspended = true;
        Q_EMIT ended();
    }
    if (m_paused && m_mixer && m_mixer->silent() && !m_suspended) {
        m_output.suspend();
        m_suspended = true;
    }
    if (m_state == BackendState::Loading && m_current.voice && m_current.voice->isReady() &&
        !m_current.voice->ring()->held()) {
        setState(m_paused ? BackendState::Paused : BackendState::Playing);  // after a retry
    }
    const bool active = m_state == BackendState::Playing || m_state == BackendState::Loading;
    const bool waiting = !m_current.voice || m_current.voice->waiting() || (m_mixer && m_mixer->starved());
    setBuffering(active && waiting);
    if (const std::int64_t now = position(); now != m_lastPosition) {
        m_lastPosition = now;
        Q_EMIT positionChanged();
    }
    const bool idle = m_state == BackendState::Idle || m_state == BackendState::Ended ||
                      m_state == BackendState::Failed || (m_state == BackendState::Paused && m_suspended);
    if (idle) {
        m_ticker.stop();
    }
}

void NativeBackend::setState(BackendState state)
{
    if (m_state != state) {
        qCDebug(lcMedia) << "backend state" << static_cast<int>(m_state) << "->" << static_cast<int>(state);
        m_state = state;
        Q_EMIT stateChanged();
    }
}

void NativeBackend::setBuffering(bool buffering)
{
    if (m_buffering != buffering) {
        m_buffering = buffering;
        Q_EMIT bufferingChanged();
    }
}

void NativeBackend::fail(const Error& error)
{
    setBuffering(false);
    setState(BackendState::Failed);
    m_ticker.stop();
    Q_EMIT failed(error);
}

}  // namespace noctara::media
