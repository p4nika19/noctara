#include "drm/DrmBackend.hpp"

#include "api/Endpoints.hpp"

#include "core/Log.hpp"
#include "drm/DrmHost.hpp"

namespace noctara::drm {

using media::BackendState;

namespace {

// The helper's error codes (docs/ipc.md). Network problems keep the track (playing again retries); the others
// skip it.
ErrorCode errorFor(const QString& code)
{
    if (code == u"network") {
        return ErrorCode::ConnectionFailed;
    }
    if (code == u"drm") {
        return ErrorCode::LicenseDenied;
    }
    if (code == u"media") {
        return ErrorCode::DecodeFailed;
    }
    if (code == u"unsupported") {
        return ErrorCode::Unsupported;
    }
    return ErrorCode::ProtectedPlayerFailed;
}

}  // namespace

DrmBackend::DrmBackend(DrmHost& host, QObject* parent)
    : IPlaybackBackend(parent)
    , m_host(host)
{
    connect(&m_host, &DrmHost::received, this, &DrmBackend::onEvent);
    connect(&m_host, &DrmHost::stopped, this, &DrmBackend::onStopped);
    connect(&m_host, &DrmHost::ready, this, [this] {
        if (m_resume && m_source) {
            qCInfo(lcDrm) << "resuming the protected track at" << m_position << "ms";
            open();
        }
    });
    connect(&m_host, &DrmHost::failed, this, [this](const Error& error) {
        if (m_source && m_state != BackendState::Idle && m_state != BackendState::Ended &&
            m_state != BackendState::Failed) {
            fail(error);
        }
    });
}

void DrmBackend::load(const media::PlaybackSource& source, std::int64_t startMs, bool paused)
{
    m_source = source;
    m_position = startMs;
    m_wantPlay = !paused;
    Q_EMIT positionChanged();
    open();
}

void DrmBackend::open()
{
    const std::uint64_t serial = ++m_serial;
    m_loaded = false;
    m_resume = false;
    m_opening = true;
    setState(BackendState::Loading);
    m_host.setBusy(true);
    if (!m_source || !m_source->protectedStream) {
        fail(Error{ErrorCode::NotPlayable, "no protected stream for this source"});
        return;
    }
    m_source->protectedStream(this, [this, serial](Result<api::ProtectedStream> stream) {
        if (serial != m_serial) {
            return;  // replaced or stopped meanwhile
        }
        m_opening = false;
        if (!stream) {
            fail(stream.error());
            return;
        }
        const QJsonObject message{
            {QStringLiteral("type"), QStringLiteral("load")},
            {QStringLiteral("id"), QString::number(serial)},
            {QStringLiteral("url"), stream->url.toString(QUrl::FullyEncoded)},
            {QStringLiteral("licenseUrl"), api::endpoints::widevineLicense().toString(QUrl::FullyEncoded)},
            {QStringLiteral("licenseToken"), stream->licenseToken},
            {QStringLiteral("startMs"), static_cast<double>(m_position)},
            {QStringLiteral("volume"), m_gain},
            {QStringLiteral("autoplay"), m_wantPlay},
        };
        if (m_host.send(message) && serial == m_serial) {
            m_loaded = true;
        }
    });
}

void DrmBackend::setNext(const std::optional<media::PlaybackSource>& source)
{
    // No gapless hand-over between helper tracks; starting the helper early is what saves time.
    if (source && source->info.drm && m_host.status() != DrmHost::Status::Failed) {
        m_host.warm();
    }
}

void DrmBackend::play()
{
    m_wantPlay = true;
    if (!m_source) {
        return;
    }
    if (m_opening) {
        return;  // the load will start playing
    }
    if (!m_loaded || m_state == BackendState::Failed || m_state == BackendState::Ended) {
        if (m_state == BackendState::Ended) {
            m_position = 0;
        }
        open();
        return;
    }
    m_host.setBusy(true);
    (void)m_host.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("play")}});
}

void DrmBackend::pause()
{
    m_wantPlay = false;
    if (m_loaded) {
        (void)m_host.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("pause")}});
    }
    if (m_state == BackendState::Playing) {
        setState(BackendState::Paused);
    }
    setBuffering(false);
    if (!m_opening) {
        m_host.setBusy(false);
    }
}

void DrmBackend::stop()
{
    ++m_serial;
    if (m_loaded && m_host.running()) {
        (void)m_host.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("stop")}});
    }
    m_source.reset();
    m_loaded = false;
    m_opening = false;
    m_resume = false;
    m_wantPlay = false;
    m_position = 0;
    m_host.setBusy(false);
    setBuffering(false);
    setState(BackendState::Idle);
}

void DrmBackend::seek(std::int64_t ms)
{
    m_position = std::max<std::int64_t>(ms, 0);
    if (m_loaded) {
        (void)m_host.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("seek")},
                                      {QStringLiteral("ms"), static_cast<double>(m_position)}});
    }
    Q_EMIT positionChanged();
}

void DrmBackend::setGain(double gain)
{
    m_gain = std::clamp(gain, 0.0, 1.0);
    if (m_loaded) {
        (void)m_host.send(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("volume")}, {QStringLiteral("value"), m_gain}});
    }
}

void DrmBackend::setCrossfade(std::int64_t /*ms*/)
{
    // Tracks in the helper do not overlap.
}

bool DrmBackend::current(const QJsonObject& message) const
{
    return m_source && message.value(u"id").toString() == QString::number(m_serial);
}

void DrmBackend::onEvent(const QJsonObject& message)
{
    if (!current(message)) {
        return;
    }
    const QString type = message.value(u"type").toString();
    if (type == u"state") {
        const QString state = message.value(u"state").toString();
        if (state == u"loading" || state == u"buffering") {
            setBuffering(true);
        } else if (state == u"playing") {
            setBuffering(false);
            m_host.setBusy(true);
            setState(BackendState::Playing);
        } else if (state == u"paused") {
            setBuffering(false);
            m_host.setBusy(false);
            setState(BackendState::Paused);
        } else if (state == u"ended") {
            setBuffering(false);
            m_host.setBusy(false);
            setState(BackendState::Ended);
            Q_EMIT ended();
        }
    } else if (type == u"position") {
        m_position = static_cast<std::int64_t>(message.value(u"ms").toDouble());
        if (const auto duration = static_cast<std::int64_t>(message.value(u"durationMs").toDouble()); duration > 0) {
            Q_EMIT durationKnown(duration);
        }
        Q_EMIT positionChanged();
    } else if (type == u"error") {
        const QString code = message.value(u"code").toString();
        fail(Error{errorFor(code),
                   QStringLiteral("helper: %1 %2").arg(code, message.value(u"detail").toString()).toStdString()});
    }
}

void DrmBackend::onStopped(bool unexpected)
{
    m_loaded = false;
    if (!m_source || m_opening || m_state == BackendState::Idle || m_state == BackendState::Ended ||
        m_state == BackendState::Failed) {
        return;
    }
    if (unexpected) {
        // Crashed, hung or restarted: the track comes back where it was once the helper is ready again (or on play).
        m_resume = true;
        setBuffering(m_wantPlay);
    }
}

void DrmBackend::fail(const Error& error)
{
    ++m_serial;
    m_loaded = false;
    m_opening = false;
    m_resume = false;
    m_host.setBusy(false);
    setBuffering(false);
    setState(BackendState::Failed);
    Q_EMIT failed(error);
}

void DrmBackend::setState(BackendState state)
{
    if (state != m_state) {
        m_state = state;
        Q_EMIT stateChanged();
    }
}

void DrmBackend::setBuffering(bool buffering)
{
    if (buffering != m_buffering) {
        m_buffering = buffering;
        Q_EMIT bufferingChanged();
    }
}

}  // namespace noctara::drm
