#include "media/PlaybackController.hpp"

#include "api/Endpoints.hpp"

#include "cache/CacheService.hpp"
#include "core/Log.hpp"
#include "models/CollectionModel.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QCoreApplication>
#include <QRandomGenerator>
#include <QSet>

#include <cmath>
#include <mutex>

namespace noctara::media {

namespace keys = settings::keys;

// Resolves stream URLs for the audio cache (on the cache's thread) through the API client, until the controller
// goes away: after disable() the client is never touched again.
class StreamUrlResolver {
public:
    explicit StreamUrlResolver(api::IApiClient& client)
        : m_client(&client)
    {
    }

    void resolve(const api::Transcoding& transcoding, const QString& authorization, const cache::UrlCallback& done)
    {
        const std::scoped_lock lock(m_mutex);
        if (m_client == nullptr) {
            done(fail(ErrorCode::Cancelled, "player gone"));
            return;
        }
        // The answer goes straight to the cache's callback, which may run on any thread; the application object
        // outlives every request.
        (void)api::fetch(*m_client, api::endpoints::streamUrl(transcoding, authorization), QCoreApplication::instance(),
                         [done](Result<QUrl> url) { done(std::move(url)); });
    }

    void disable()
    {
        const std::scoped_lock lock(m_mutex);
        m_client = nullptr;
    }

private:
    std::mutex m_mutex;
    api::IApiClient* m_client;
};

PlaybackController::PlaybackController(Dependencies dependencies, QObject* parent)
    : QObject(parent)
    , m_settings(dependencies.settings)
    , m_api(dependencies.api)
    , m_cache(dependencies.cache)
    , m_native(dependencies.native)
    , m_protected(dependencies.protectedBackend)
    , m_active(&m_native)
    , m_metered(dependencies.metered ? std::move(dependencies.metered) : [] { return false; })
    , m_store(dependencies.statePath)
    , m_queueModel(m_queue)
    , m_resolver(std::make_shared<StreamUrlResolver>(dependencies.api))
{
    m_session = m_store.load();
    if (!m_settings.boolValue(keys::RememberVolume)) {
        m_session.volume = static_cast<double>(m_settings.intValue(keys::StartupVolume)) / 100.0;
        m_session.muted = false;
    }
    m_queue.setShuffle(m_session.shuffle);
    m_queue.setRepeat(m_session.repeat);
    if (m_cache != nullptr) {
        m_prefetcher = std::make_unique<cache::Prefetcher>(m_cache->audio());
    }

    connectBackend(m_native);
    if (m_protected != nullptr) {
        connectBackend(*m_protected);
    }
    connect(&m_settings, &settings::SettingsStore::valueChanged, this, [this](const QString& key) {
        if (key == QLatin1StringView(keys::Crossfade)) {
            applySettings();
        }
    });
    applySettings();
    applyGain();
    if (m_settings.boolValue(keys::RestoreQueue) && !m_session.queue.empty()) {
        m_restorePending = true;
        restoreQueue(m_session);
    }
}

void PlaybackController::connectBackend(IPlaybackBackend& backend)
{
    // Only the backend of the current item speaks for it.
    const auto active = [this, &backend] {
        return &backend == m_active;
    };
    connect(&backend, &IPlaybackBackend::stateChanged, this, [this, active] {
        if (!active()) {
            return;
        }
        if (m_active->state() == BackendState::Playing) {
            m_failuresInRow = 0;
            if (m_error >= 0) {
                m_error = -1;
            }
        }
        Q_EMIT stateChanged();
    });
    connect(&backend, &IPlaybackBackend::bufferingChanged, this, [this, active] {
        if (active()) {
            Q_EMIT stateChanged();
        }
    });
    connect(&backend, &IPlaybackBackend::positionChanged, this, [this, active] {
        if (!active()) {
            return;
        }
        // Every update repaints the window; a few per second keep the waveform and the clock exact to the pixel
        // and the second. Seeks and track changes notify at once.
        if (!m_positionAge.isValid() || m_positionAge.elapsed() >= kPositionIntervalMs ||
            std::abs(position() - m_lastPosition) > 2 * kPositionIntervalMs) {
            m_positionAge.restart();
            m_lastPosition = position();
            Q_EMIT positionChanged();
        }
        updateCachedRanges(false);
        const qint64 lead = kPrepareLeadMs + (m_settings.intValue(keys::Crossfade) * 1000);
        if (!m_nextPrepared && m_started && m_duration > 0 && m_duration - position() <= lead) {
            prepareFollowing();
        }
    });
    connect(&backend, &IPlaybackBackend::durationKnown, this, [this, active](std::int64_t ms) {
        if (active() && m_duration <= 0 && ms > 0) {
            m_duration = ms;
            Q_EMIT durationChanged();
        }
    });
    connect(&backend, &IPlaybackBackend::advanced, this, [this, active] {
        if (active()) {
            onAdvanced();
        }
    });
    connect(&backend, &IPlaybackBackend::ended, this, [this, active] {
        if (active()) {
            onEnded();
        }
    });
    connect(&backend, &IPlaybackBackend::failed, this, [this, active](const Error& error) {
        if (active()) {
            onFailed(error);
        }
    });
}

IPlaybackBackend& PlaybackController::backendFor(const PlaybackSource& source) noexcept
{
    return source.info.drm && m_protected != nullptr ? *m_protected : m_native;
}

void PlaybackController::activate(IPlaybackBackend& backend)
{
    if (&backend == m_active) {
        return;
    }
    m_active->setNext(std::nullopt);
    m_active->stop();
    m_active = &backend;
}

void PlaybackController::restoreQueue(const SessionState& saved)
{
    (void)m_api.lookupTracks(std::vector<api::Id>(saved.queue.begin(), saved.queue.end()), &m_requests,
                             [this, saved](Result<std::vector<api::Track>> tracks) {
                                 if (!tracks || !m_queue.empty()) {
                                     return;  // offline, or something started playing meanwhile
                                 }
                                 QHash<api::Id, api::Track> byId;
                                 for (api::Track& track : *tracks) {
                                     byId.insert(track.id, std::move(track));
                                 }
                                 std::vector<QueueItem> items;
                                 int current = 0;
                                 for (std::size_t i = 0; i < saved.queue.size(); ++i) {
                                     if (const auto it = byId.constFind(saved.queue[i]); it != byId.constEnd()) {
                                         if (std::cmp_less_equal(i, saved.current)) {
                                             current = static_cast<int>(items.size());
                                         }
                                         items.push_back({.track = *it, .context = saved.context, .uid = 0});
                                     }
                                 }
                                 if (items.empty()) {
                                     m_restorePending = false;  // none of them exists any more
                                     return;
                                 }
                                 const bool sameTrack = items[static_cast<std::size_t>(current)].track.id ==
                                                        saved.queue[static_cast<std::size_t>(saved.current)];
                                 m_queue.restore(std::move(items), current, m_queue.shuffle());
                                 m_resumeAt = sameTrack ? saved.positionMs : 0;
                                 m_queueModel.reload();
                                 publishCurrent();
                                 Q_EMIT stateChanged();
                             });
}

PlaybackController::~PlaybackController()
{
    saveSession();  // with the position as it is now
    m_resolver->disable();
    if (m_prefetcher) {
        m_prefetcher->cancel();
    }
    m_native.stop();
    if (m_protected != nullptr) {
        m_protected->stop();
    }
    m_store.flush();
}

// --- Starting things ---------------------------------------------------------------------------

std::vector<QueueItem> PlaybackController::itemsFrom(std::vector<api::Track> tracks, const QString& context)
{
    std::vector<QueueItem> items;
    items.reserve(tracks.size());
    for (api::Track& track : tracks) {
        items.push_back({.track = std::move(track), .context = context, .uid = 0});
    }
    return items;
}

void PlaybackController::playCollection(models::CollectionModel* model, int row, const QString& context)
{
    if (model == nullptr || row < 0 || row >= model->count()) {
        return;
    }
    const std::vector<api::Entry>& entries = model->entries();
    const api::Item& clicked = entries[static_cast<std::size_t>(row)].item;
    if (const auto* playlist = std::get_if<api::Playlist>(&clicked)) {
        playPlaylist(*playlist, std::nullopt);
        return;
    }
    if (!std::holds_alternative<api::Track>(clicked)) {
        return;
    }
    // The list's tracks become the queue, starting at the one clicked; the queue then follows the list.
    std::vector<api::Track> tracks;
    int start = 0;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (const auto* track = std::get_if<api::Track>(&entries[i].item)) {
            if (std::cmp_equal(i, row)) {
                start = static_cast<int>(tracks.size());
            }
            tracks.push_back(*track);
        }
    }
    playTracks(std::move(tracks), start, context);
    follow(model);
}

void PlaybackController::shufflePlay(models::CollectionModel* model, const QString& context)
{
    if (model == nullptr) {
        return;
    }
    std::vector<int> rows;
    for (int row = 0; row < model->count(); ++row) {
        if (std::holds_alternative<api::Track>(model->entries()[static_cast<std::size_t>(row)].item)) {
            rows.push_back(row);
        }
    }
    if (rows.empty()) {
        return;
    }
    setShuffle(true);
    const auto pick = QRandomGenerator::global()->bounded(static_cast<quint32>(rows.size()));
    playCollection(model, rows[pick], context);
}

void PlaybackController::enqueueCollection(models::CollectionModel* model, int row, bool next)
{
    if (model == nullptr || row < 0 || row >= model->count()) {
        return;
    }
    const api::Item& item = model->entries()[static_cast<std::size_t>(row)].item;
    if (const auto* playlist = std::get_if<api::Playlist>(&item)) {
        playPlaylist(*playlist, next);
        return;
    }
    if (const auto* track = std::get_if<api::Track>(&item)) {
        const bool wasEmpty = m_queue.empty();
        if (next) {
            m_queue.insertNext(itemsFrom({*track}, {}));
        } else {
            m_queue.append(itemsFrom({*track}, {}));
        }
        queueEdited();
        if (wasEmpty) {
            startCurrent(0, true);
        }
    }
}

void PlaybackController::playItem(const api::Item& item)
{
    if (const auto* track = std::get_if<api::Track>(&item)) {
        playTracks({*track}, 0, {});
    } else if (const auto* playlist = std::get_if<api::Playlist>(&item)) {
        playPlaylist(*playlist, std::nullopt);
    }
}

void PlaybackController::playPlaylist(const api::Playlist& playlist, std::optional<bool> enqueueNext)
{
    const auto use = [this, enqueueNext](const api::Playlist& full) {
        if (full.tracks.empty()) {
            Q_EMIT problem(static_cast<int>(ErrorCode::NotPlayable));
            return;
        }
        if (!enqueueNext) {
            playTracks(full.tracks, 0, full.title);
            return;
        }
        const bool wasEmpty = m_queue.empty();
        if (*enqueueNext) {
            m_queue.insertNext(itemsFrom(full.tracks, full.title));
        } else {
            m_queue.append(itemsFrom(full.tracks, full.title));
        }
        queueEdited();
        hydrateStubs();
        if (wasEmpty) {
            startCurrent(0, true);
        }
    };
    if (!playlist.tracks.empty() && std::cmp_greater_equal(playlist.tracks.size(), playlist.trackCount)) {
        use(playlist);
        return;
    }
    // Search results and feeds carry only the first few tracks of a playlist.
    (void)api::fetch(m_api, api::endpoints::playlist(playlist.id), &m_requests,
                     [this, use](Result<api::Playlist> full) {
                         if (!full) {
                             Q_EMIT problem(static_cast<int>(full.error().code));
                             return;
                         }
                         use(*full);
                     });
}

void PlaybackController::playTracks(std::vector<api::Track> tracks, int start, const QString& context)
{
    if (tracks.empty()) {
        return;
    }
    unfollow();  // a new queue: playCollection() follows its list again
    m_queue.replace(itemsFrom(std::move(tracks), context), start);
    m_failuresInRow = 0;
    queueEdited();
    hydrateStubs();
    startCurrent(0, false);
}

void PlaybackController::hydrateStubs()
{
    std::vector<api::Id> ids;
    for (const QueueItem& item : m_queue.items()) {
        if (item.track.stub) {
            ids.push_back(item.track.id);
        }
    }
    if (!ids.empty()) {
        hydrate(ids, [] {});
    }
}

void PlaybackController::hydrate(const std::vector<api::Id>& ids, std::function<void()> then)
{
    (void)m_api.lookupTracks(ids, &m_requests,
                             [this, ids, then = std::move(then)](Result<std::vector<api::Track>> tracks) {
                                 if (tracks) {
                                     for (const api::Track& track : *tracks) {
                                         m_queue.updateTrack(track);
                                     }
                                 }
                                 // Saved copies that could not be refreshed play as they are rather than being
                                 // asked for again and again.
                                 for (const api::Id id : ids) {
                                     m_queue.markFresh(id);
                                 }
                                 m_queueModel.refresh();
                                 Q_EMIT trackChanged();
                                 then();
                             });
}

api::StreamPreference PlaybackController::preference() const
{
    const QString quality = m_settings.stringValue(keys::StreamQuality);
    if (quality == u"data_saver" || (quality == u"auto" && m_metered())) {
        return api::StreamPreference::DataSaver;
    }
    return api::StreamPreference::Best;
}

std::optional<PlaybackSource> PlaybackController::sourceFor(const QueueItem& item, ErrorCode* why) const
{
    const api::Track& track = item.track;
    const auto refuse = [why](ErrorCode code) {
        if (why != nullptr) {
            *why = code;
        }
        return std::nullopt;
    };
    if (!track.streamable || track.policy == api::Policy::Block) {
        return refuse(ErrorCode::NotPlayable);
    }
    if (track.drm()) {
        if (m_protected == nullptr || !m_settings.boolValue(keys::ProtectedEnabled)) {
            return refuse(ErrorCode::ProtectedPlayback);
        }
        const std::optional<api::Transcoding> transcoding = api::pickProtectedTranscoding(track, preference());
        if (!transcoding) {
            return refuse(ErrorCode::NotPlayable);
        }
        PlaybackSource source;
        source.info = api::toTrackInfo(track, *transcoding, item.context);
        // Resolved afresh for every load: the signed URL and the license token are never kept.
        source.protectedStream = [api = &m_api, transcoding = *transcoding, authorization = track.trackAuthorization](
                                     QObject* context, std::function<void(Result<api::ProtectedStream>)> done) {
            (void)api::fetch(*api, api::endpoints::protectedStream(transcoding, authorization), context,
                             std::move(done));
        };
        return source;
    }
    const std::optional<api::Transcoding> transcoding = api::pickTranscoding(track, preference());
    if (!transcoding) {
        return refuse(ErrorCode::NotPlayable);
    }
    PlaybackSource source;
    source.info = api::toTrackInfo(track, *transcoding, item.context);
    source.urls = [resolver = m_resolver, transcoding = *transcoding,
                   authorization = track.trackAuthorization](bool /*refresh*/, const cache::UrlCallback& done) {
        // Never cached, so every call yields a freshly signed URL.
        resolver->resolve(transcoding, authorization, done);
    };
    return source;
}

void PlaybackController::startCurrent(std::int64_t startMs, bool paused)
{
    m_resumeAt = 0;
    ++m_trackSerial;
    m_ended = false;
    m_started = false;
    m_pendingPlay = !paused;
    const QueueItem* item = m_queue.currentItem();
    if (item == nullptr) {
        stop();
        return;
    }
    publishCurrent();
    if (item->track.stub || item->track.stale) {
        // Known only by id (a playlist's later tracks), or a saved copy whose authorization may be old: fetched
        // first.
        const std::uint64_t serial = m_trackSerial;
        const api::Id id = item->track.id;
        hydrate({id}, [this, serial, id, startMs] {
            if (serial != m_trackSerial) {
                return;
            }
            const QueueItem* now = m_queue.currentItem();
            if (now == nullptr || now->track.id != id) {
                return;
            }
            if (now->track.stub) {
                skipAfterError(ErrorCode::NotPlayable);
                return;
            }
            startCurrent(startMs, !m_pendingPlay);  // paused or stopped meanwhile: it loads paused
        });
        Q_EMIT stateChanged();
        return;
    }
    ErrorCode why = ErrorCode::NotPlayable;
    const std::optional<PlaybackSource> source = sourceFor(*item, &why);
    if (!source) {
        skipAfterError(why);
        return;
    }
    m_started = true;
    IPlaybackBackend& backend = backendFor(*source);
    activate(backend);
    applyGain();
    backend.load(*source, startMs, paused);
    prefetchUpcoming();
    Q_EMIT stateChanged();
}

void PlaybackController::publishCurrent()
{
    saveSession();
    m_error = -1;
    m_nextPrepared = false;
    const api::Track* track = currentTrack();
    m_duration = track != nullptr ? track->durationMs : 0;
    Q_EMIT trackChanged();
    Q_EMIT durationChanged();
    Q_EMIT positionChanged();
    Q_EMIT queueChanged();
    loadWaveform();
    updateCachedRanges(true);
}

void PlaybackController::prepareFollowing()
{
    if (!m_started) {
        return;
    }
    m_nextPrepared = true;
    const std::optional<int> following = m_queue.following();
    if (!following) {
        m_active->setNext(std::nullopt);
        return;
    }
    const QueueItem& item = m_queue.items()[static_cast<std::size_t>(*following)];
    if (item.track.stub || item.track.stale) {
        const std::uint64_t serial = m_trackSerial;
        const api::Id id = item.track.id;
        hydrate({id}, [this, serial, id] {
            if (serial != m_trackSerial) {
                return;
            }
            // Still a stub: the lookup left it out (deleted, private) or failed. Asking again would only repeat
            // that; onEnded() reports and skips it.
            const std::optional<int> now = m_queue.following();
            if (now) {
                const api::Track& track = m_queue.items()[static_cast<std::size_t>(*now)].track;
                if (track.id == id && track.stub) {
                    m_active->setNext(std::nullopt);
                    return;
                }
            }
            prepareFollowing();
        });
        return;
    }
    // Unplayable items are left to onEnded(), which reports and skips them. An item for the other backend starts
    // when this one ends; that backend gets it early only to warm up (the helper starts ahead of a protected one).
    std::optional<PlaybackSource> source = sourceFor(item);
    if (source && &backendFor(*source) != m_active) {
        m_active->setNext(std::nullopt);
        backendFor(*source).setNext(source);
        return;
    }
    m_active->setNext(source);
}

void PlaybackController::prefetchUpcoming()
{
    if (!m_prefetcher || m_cache == nullptr) {
        return;
    }
    const int count = m_cache->prefetchCount(m_metered());
    std::vector<cache::PrefetchItem> upcoming;
    for (int i = m_queue.current() + 1; i < m_queue.size() && std::cmp_less(upcoming.size(), count); ++i) {
        const QueueItem& item = m_queue.items()[static_cast<std::size_t>(i)];
        if (item.track.stub || item.track.stale) {
            continue;  // fetched when it is prepared
        }
        // Protected tracks are never prefetched (or cached).
        if (std::optional<PlaybackSource> source = sourceFor(item); source && !source->info.drm) {
            upcoming.push_back({.info = std::move(source->info), .urls = std::move(source->urls)});
        }
    }
    m_prefetcher->setQueue(std::move(upcoming), count, cache::prefetchBytes(m_cache->audioDiskEnabled()));
}

// --- Backend events ----------------------------------------------------------------------------

void PlaybackController::onAdvanced()
{
    // The prepared item took over without a gap: the queue follows.
    const int previous = m_queue.current();
    (void)m_queue.advance(false);
    m_queueModel.currentMoved(previous);
    ++m_trackSerial;
    m_failuresInRow = 0;
    publishCurrent();
    prefetchUpcoming();
    Q_EMIT seeked(0);
}

void PlaybackController::onEnded()
{
    // Nothing was prepared to follow (the end of the queue, or an item that could not be prepared).
    const int previous = m_queue.current();
    if (m_queue.advance(false)) {
        m_queueModel.currentMoved(previous);
        startCurrent(0, false);
        return;
    }
    if (m_settings.boolValue(keys::Autoplay)) {
        autoplay();
        return;
    }
    m_ended = true;
    Q_EMIT stateChanged();
}

void PlaybackController::onFailed(const Error& error)
{
    qCInfo(lcMedia).noquote() << "playback failed:" << QString::fromStdString(error.describe());
    switch (error.code) {
    case ErrorCode::DecodeFailed:
    case ErrorCode::NotCacheable:
    case ErrorCode::NotFound:
    case ErrorCode::Forbidden:
    case ErrorCode::Unsupported:
    case ErrorCode::LicenseDenied:
    case ErrorCode::ProtectedPlayback:
    case ErrorCode::ProtectedPlayerMissing:
    case ErrorCode::ProtectedPlayerInstalling:
    case ErrorCode::ProtectedPlayerFailed:
    case ErrorCode::ProtectedNetworkUnsupported: skipAfterError(error.code); return;
    default:
        // A network problem: the track stays; playing again retries from where it stopped.
        m_error = static_cast<int>(error.code);
        Q_EMIT stateChanged();
        Q_EMIT problem(m_error);
    }
}

void PlaybackController::skipAfterError(ErrorCode code)
{
    m_error = static_cast<int>(code);
    m_started = false;
    m_pendingPlay = false;
    Q_EMIT problem(m_error);
    Q_EMIT stateChanged();
    // Keep going, but never in circles through a queue where nothing plays.
    if (++m_failuresInRow >= std::max(m_queue.size(), 1)) {
        m_active->stop();
        return;
    }
    const int previous = m_queue.current();
    if (m_queue.advance(true)) {
        m_queueModel.currentMoved(previous);
        startCurrent(0, false);
    } else {
        m_active->stop();
    }
}

void PlaybackController::autoplay()
{
    const api::Track* last = currentTrack();
    if (last == nullptr || m_autoplaying) {
        return;
    }
    m_autoplaying = true;
    (void)api::fetch(m_api, api::endpoints::related(last->id, kAutoplayCount), &m_requests,
                     [this](Result<api::Page<api::Entry>> page) {
                         m_autoplaying = false;
                         QSet<api::Id> queued;
                         for (const QueueItem& item : m_queue.items()) {
                             queued.insert(item.track.id);
                         }
                         std::vector<api::Track> tracks;
                         if (page) {
                             for (api::Entry& entry : page->items) {
                                 if (auto* track = std::get_if<api::Track>(&entry.item);
                                     track != nullptr && !queued.contains(track->id)) {
                                     tracks.push_back(std::move(*track));
                                 }
                             }
                         }
                         if (tracks.empty()) {
                             m_ended = true;
                             Q_EMIT stateChanged();
                             return;
                         }
                         const int previous = m_queue.current();
                         m_queue.append(itemsFrom(std::move(tracks), {}));
                         (void)m_queue.advance(true);
                         queueEdited();
                         m_queueModel.currentMoved(previous);
                         startCurrent(0, false);
                     });
}

// --- Controls ----------------------------------------------------------------------------------

void PlaybackController::play()
{
    if (!hasTrack()) {
        return;
    }
    if (m_ended) {
        m_queue.jump(m_queue.current());
        startCurrent(0, false);
        return;
    }
    if (!m_started) {
        startCurrent(position(), false);
        return;
    }
    m_active->play();
}

void PlaybackController::pause()
{
    m_pendingPlay = false;
    m_active->pause();
    saveSession();
    Q_EMIT stateChanged();
}

void PlaybackController::playPause()
{
    if (playing()) {
        pause();
    } else {
        play();
    }
}

void PlaybackController::stop()
{
    m_active->stop();
    m_started = false;
    m_pendingPlay = false;
    Q_EMIT stateChanged();
    Q_EMIT positionChanged();
}

void PlaybackController::next()
{
    const int previous = m_queue.current();
    if (m_queue.advance(true)) {
        m_queueModel.currentMoved(previous);
        startCurrent(0, !playing() && m_started);
    } else if (hasTrack() && m_settings.boolValue(keys::Autoplay)) {
        autoplay();
    }
}

void PlaybackController::previous()
{
    if (position() > kRestartThresholdMs) {
        seek(0);
        return;
    }
    const int before = m_queue.current();
    if (m_queue.previous()) {
        m_queueModel.currentMoved(before);
        startCurrent(0, !playing() && m_started);
    } else {
        seek(0);
    }
}

void PlaybackController::seek(qint64 ms)
{
    if (!hasTrack()) {
        return;
    }
    ms = std::clamp<qint64>(ms, 0, m_duration > 0 ? m_duration : ms);
    if (m_ended) {
        m_ended = false;
        startCurrent(ms, false);
    } else if (!m_started) {
        startCurrent(ms, !m_pendingPlay);
    } else {
        m_active->seek(ms);
    }
    Q_EMIT positionChanged();
    Q_EMIT seeked(ms);
}

void PlaybackController::seekBy(qint64 deltaMs)
{
    seek(position() + deltaMs);
}

void PlaybackController::playQueueItem(int index)
{
    const int previous = m_queue.current();
    if (m_queue.jump(index)) {
        m_queueModel.currentMoved(previous);
        startCurrent(0, false);
    }
}

void PlaybackController::removeQueueItem(int index)
{
    const bool wasCurrent = index == m_queue.current();
    if (!m_queue.remove(index)) {
        return;
    }
    queueEdited();
    if (wasCurrent) {
        if (hasTrack()) {
            startCurrent(0, !playing());
        } else {
            stop();
            publishCurrent();
        }
    }
}

void PlaybackController::moveQueueItem(int from, int to)
{
    if (m_queue.move(from, to)) {
        queueEdited();
    }
}

void PlaybackController::clearUpcoming()
{
    unfollow();  // the list's later tracks would otherwise come back
    while (m_queue.size() > m_queue.current() + 1) {
        (void)m_queue.remove(m_queue.size() - 1);
    }
    queueEdited();
}

void PlaybackController::follow(models::CollectionModel* model)
{
    unfollow();
    if (model == nullptr || m_queue.empty()) {
        return;
    }
    m_followed = model;
    m_followContext = m_queue.items().front().context;
    for (const QueueItem& item : m_queue.items()) {
        m_followKnown.insert(item.track.id);
        m_followUids.insert(item.uid);
    }
    // Coalesced: a sync changes the list several times in a row.
    m_followConnection = connect(model, &models::CollectionModel::listChanged, this, [this] {
        if (!m_reconcileQueued) {
            m_reconcileQueued = true;
            QMetaObject::invokeMethod(this, &PlaybackController::reconcileFollowed, Qt::QueuedConnection);
        }
    });
    // Repointed to other contents (a new search, another chart genre, a refresh from page one, another account):
    // those are not the list the queue came from.
    m_followResetConnection =
        connect(model, &models::CollectionModel::sourceReset, this, &PlaybackController::unfollow);
}

void PlaybackController::unfollow()
{
    disconnect(m_followConnection);
    disconnect(m_followResetConnection);
    m_followed.clear();
    m_followKnown.clear();
    m_followUids.clear();
}

void PlaybackController::reconcileFollowed()
{
    m_reconcileQueued = false;
    if (!m_followed) {
        unfollow();
        return;
    }
    QSet<api::Id> listed;
    std::vector<const api::Track*> order;  // the list's tracks in its order
    for (const api::Entry& entry : m_followed->entries()) {
        if (const auto* track = std::get_if<api::Track>(&entry.item)) {
            listed.insert(track->id);
            order.push_back(track);
        }
    }
    bool edited = false;
    // Tracks the list lost leave the queue, except the one playing.
    for (int i = m_queue.size() - 1; i >= 0; --i) {
        const QueueItem& item = m_queue.items()[static_cast<std::size_t>(i)];
        if (i != m_queue.current() && m_followUids.contains(item.uid) && !listed.contains(item.track.id)) {
            m_followUids.remove(item.uid);
            (void)m_queue.remove(i);
            edited = true;
        }
    }
    // Tracks it gained join it. In list order, those above the current track are behind it already.
    const api::Track* current = currentTrack();
    const auto currentAt =
        current == nullptr
            ? order.end()
            : std::ranges::find_if(order, [current](const api::Track* track) { return track->id == current->id; });
    std::vector<QueueItem> added;
    for (auto it = order.begin(); it != order.end(); ++it) {
        const api::Track& track = **it;
        if (m_followKnown.contains(track.id)) {
            continue;
        }
        m_followKnown.insert(track.id);
        if (!m_queue.shuffle() && currentAt != order.end() && it < currentAt) {
            continue;
        }
        added.push_back({.track = track, .context = m_followContext, .uid = 0});
    }
    if (!added.empty()) {
        for (const std::uint64_t uid : m_queue.addUpcoming(std::move(added))) {
            m_followUids.insert(uid);
        }
        edited = true;
    }
    if (edited) {
        queueEdited();
    }
}

void PlaybackController::queueEdited()
{
    saveSession();
    m_queueModel.reload();
    Q_EMIT queueChanged();
    if (m_nextPrepared) {
        prepareFollowing();
    }
    prefetchUpcoming();
}

void PlaybackController::setVolume(double volume)
{
    if (!std::isfinite(volume)) {
        return;  // std::clamp lets NaN through; an MPRIS client can send one
    }
    volume = std::clamp(volume, 0.0, 1.0);
    if (qFuzzyCompare(volume + 1.0, m_session.volume + 1.0) && (!m_session.muted || volume <= 0.0)) {
        return;
    }
    m_session.volume = volume;
    if (volume > 0.0) {
        m_session.muted = false;
    }
    applyGain();
    Q_EMIT volumeChanged();
    saveSession();
}

void PlaybackController::setMuted(bool muted)
{
    if (muted == m_session.muted) {
        return;
    }
    m_session.muted = muted;
    applyGain();
    Q_EMIT volumeChanged();
    saveSession();
}

void PlaybackController::toggleMute()
{
    setMuted(!m_session.muted);
}

void PlaybackController::setShuffle(bool shuffle)
{
    if (shuffle == m_queue.shuffle()) {
        return;
    }
    const int previous = m_queue.current();
    m_queue.setShuffle(shuffle);
    queueEdited();
    m_queueModel.currentMoved(previous);
    Q_EMIT modesChanged();
    saveSession();
}

void PlaybackController::toggleShuffle()
{
    setShuffle(!m_queue.shuffle());
}

QString PlaybackController::repeat() const
{
    switch (m_queue.repeat()) {
    case Repeat::All: return QStringLiteral("all");
    case Repeat::One: return QStringLiteral("one");
    case Repeat::Off: break;
    }
    return QStringLiteral("off");
}

void PlaybackController::setRepeat(const QString& repeat)
{
    const Repeat mode = repeat == u"all" ? Repeat::All : (repeat == u"one" ? Repeat::One : Repeat::Off);
    if (mode == m_queue.repeat()) {
        return;
    }
    m_queue.setRepeat(mode);
    if (m_nextPrepared) {
        prepareFollowing();
    }
    Q_EMIT modesChanged();
    Q_EMIT queueChanged();
    saveSession();
}

void PlaybackController::cycleRepeat()
{
    switch (m_queue.repeat()) {
    case Repeat::Off: setRepeat(QStringLiteral("all")); break;
    case Repeat::All: setRepeat(QStringLiteral("one")); break;
    case Repeat::One: setRepeat(QStringLiteral("off")); break;
    }
}

// --- State -------------------------------------------------------------------------------------

const api::Track* PlaybackController::currentTrack() const noexcept
{
    const QueueItem* item = m_queue.currentItem();
    return item != nullptr ? &item->track : nullptr;
}

QString PlaybackController::trackId() const
{
    const api::Track* track = currentTrack();
    return track != nullptr ? QString::number(track->id) : QString();
}

QString PlaybackController::title() const
{
    const api::Track* track = currentTrack();
    return track != nullptr ? track->title : QString();
}

QString PlaybackController::artist() const
{
    const api::Track* track = currentTrack();
    return track != nullptr ? track->artist() : QString();
}

QString PlaybackController::artworkUrl() const
{
    const api::Track* track = currentTrack();
    return track != nullptr && !track->artwork().isEmpty() ? api::sizedArtwork(track->artwork(), 500) : QString();
}

QString PlaybackController::coverUrl() const
{
    const api::Track* track = currentTrack();
    return track != nullptr && !track->artwork().isEmpty()
               ? api::sizedArtwork(track->artwork(), models::CollectionModel::kArtworkPixels)
               : QString();
}

QString PlaybackController::permalink() const
{
    const api::Track* track = currentTrack();
    return track != nullptr ? track->permalinkUrl : QString();
}

qint64 PlaybackController::artistId() const
{
    const api::Track* track = currentTrack();
    return track != nullptr ? track->user.id : 0;
}

QString PlaybackController::context() const
{
    const QueueItem* item = m_queue.currentItem();
    return item != nullptr ? item->context : QString();
}

bool PlaybackController::preview() const
{
    const api::Track* track = currentTrack();
    return track != nullptr && track->previewOnly();
}

qint64 PlaybackController::position() const
{
    if (m_ended) {
        return m_duration;
    }
    return m_started ? m_active->position() : m_resumeAt;
}

bool PlaybackController::playing() const noexcept
{
    if (!m_started) {
        return m_pendingPlay;
    }
    const BackendState state = m_active->state();
    return !m_ended && (state == BackendState::Playing || state == BackendState::Loading);
}

bool PlaybackController::loading() const noexcept
{
    if (!m_started) {
        return m_pendingPlay && hasTrack();
    }
    return m_active->state() == BackendState::Loading || m_active->buffering();
}

double PlaybackController::gainFor(double volume, bool muted) noexcept
{
    // Perceived loudness follows roughly the cube of the amplitude, like the system's own volume sliders.
    return muted ? 0.0 : volume * volume * volume;
}

void PlaybackController::applyGain()
{
    const double gain = gainFor(m_session.volume, m_session.muted);
    m_native.setGain(gain);
    if (m_protected != nullptr) {
        m_protected->setGain(gain);
    }
}

void PlaybackController::applySettings()
{
    m_native.setCrossfade(m_settings.intValue(keys::Crossfade) * 1000);
}

void PlaybackController::saveSession()
{
    m_session.shuffle = m_queue.shuffle();
    m_session.repeat = m_queue.repeat();
    if (!m_queue.empty()) {
        m_restorePending = false;
    }
    // Until the saved queue came back (or another took its place), it is kept as it was saved.
    if (!m_restorePending) {
        m_session.queue.clear();
        // Around the current item when the queue is long.
        const int first = std::max(0, m_queue.current() - static_cast<int>(SessionState::kMaxQueue / 2));
        for (int i = first; i < m_queue.size() && m_session.queue.size() < SessionState::kMaxQueue; ++i) {
            m_session.queue.push_back(m_queue.items()[static_cast<std::size_t>(i)].track.id);
        }
        m_session.current = m_queue.current() - first;
        m_session.positionMs = position();
        m_session.context = context();
    }
    m_store.save(m_session);
}

void PlaybackController::loadWaveform()
{
    m_waveformRequest.cancel();
    if (!m_waveform.isEmpty()) {
        m_waveform.clear();
        Q_EMIT waveformChanged();
    }
    const api::Track* track = currentTrack();
    if (track == nullptr || track->waveformUrl.isEmpty()) {
        return;
    }
    m_waveformRequest = api::fetch(m_api, api::endpoints::waveform(track->waveformUrl), &m_requests,
                                   [this, serial = m_trackSerial](Result<api::Waveform> waveform) {
                                       if (serial != m_trackSerial || !waveform) {
                                           return;
                                       }
                                       m_waveform = QList<float>(waveform->samples.begin(), waveform->samples.end());
                                       Q_EMIT waveformChanged();
                                   });
}

void PlaybackController::updateCachedRanges(bool force)
{
    if (!force && m_rangesAge.isValid() && m_rangesAge.elapsed() < 1000) {
        return;
    }
    m_rangesAge.restart();
    QList<qreal> ranges;
    if (m_started) {
        for (const auto& [from, to] : m_active->cachedRanges()) {
            ranges << from << to;
        }
    }
    if (ranges != m_cachedRanges) {
        m_cachedRanges = std::move(ranges);
        Q_EMIT cachedRangesChanged();
    }
}

}  // namespace noctara::media
