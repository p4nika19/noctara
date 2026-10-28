#pragma once

#include "api/ApiClient.hpp"
#include "api/Streams.hpp"
#include "api/Types.hpp"

#include "cache/Prefetch.hpp"
#include "core/Error.hpp"
#include "media/PlaybackBackend.hpp"
#include "media/PlaybackQueue.hpp"
#include "media/QueueModel.hpp"
#include "media/SessionState.hpp"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>

namespace noctara::settings {
class SettingsStore;
}

namespace noctara::cache {
class CacheService;
}

namespace noctara::models {
class CollectionModel;
}

namespace noctara::media {

class StreamUrlResolver;

// The QML "Player" singleton: the queue, the current track and its state, whichever backend plays it. Regular
// tracks play natively (decoded from the audio cache); protected tracks belong to the DRM backend, which arrives
// with slice (i) and until then they are reported and skipped. Stream URLs are resolved through the API client
// only when the cache needs them; the next item is prepared for a gapless start and the following ones are
// prefetched. GUI thread.
class PlaybackController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY trackChanged)
    Q_PROPERTY(QString trackId READ trackId NOTIFY trackChanged)
    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY trackChanged)
    Q_PROPERTY(QString artworkUrl READ artworkUrl NOTIFY trackChanged)  // 500 px
    Q_PROPERTY(QString coverUrl READ coverUrl NOTIFY trackChanged)      // 300 px, for the bar
    Q_PROPERTY(QString permalink READ permalink NOTIFY trackChanged)
    Q_PROPERTY(QString context READ context NOTIFY trackChanged)
    Q_PROPERTY(bool preview READ preview NOTIFY trackChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    // Playing or about to (not paused, not stopped).
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    // Waiting for audio: opening a track, a seek, or a slow network.
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    // ErrorCode of the current track's failure, -1 while there is none.
    Q_PROPERTY(int errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY volumeChanged)
    Q_PROPERTY(bool shuffle READ shuffle WRITE setShuffle NOTIFY modesChanged)
    Q_PROPERTY(QString repeat READ repeat WRITE setRepeat NOTIFY modesChanged)  // "off", "all", "one"
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY queueChanged)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY queueChanged)
    Q_PROPERTY(QList<float> waveform READ waveform NOTIFY waveformChanged)
    // Stored parts of the current track: [from0, to0, from1, to1, …] as fractions.
    Q_PROPERTY(QList<qreal> cachedRanges READ cachedRanges NOTIFY cachedRangesChanged)
    Q_PROPERTY(noctara::media::QueueModel* queue READ queueModel CONSTANT)

public:
    struct Dependencies {
        settings::SettingsStore& settings;
        api::IApiClient& api;
        cache::CacheService* cache = nullptr;  // prefetching; optional (tests)
        IPlaybackBackend& native;
        std::function<bool()> metered;  // the connection is metered right now
        QString statePath;              // session.json; empty: not persisted
    };

    explicit PlaybackController(Dependencies dependencies, QObject* parent = nullptr);
    ~PlaybackController() override;
    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;
    PlaybackController(PlaybackController&&) = delete;
    PlaybackController& operator=(PlaybackController&&) = delete;

    // Plays a row of a list: a track (the list's other tracks become the queue) or a playlist (its tracks).
    Q_INVOKABLE void playCollection(noctara::models::CollectionModel* model, int row);
    // Adds a row (a track, or a playlist's tracks) after the current item or at the end of the queue.
    Q_INVOKABLE void enqueueCollection(noctara::models::CollectionModel* model, int row, bool next);
    void playTracks(std::vector<api::Track> tracks, int start, const QString& context);
    // A resolved SoundCloud link: a track, or a playlist's tracks.
    void playItem(const api::Item& item);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void playPause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void next();
    // Restarts the track when more than a few seconds have played, otherwise goes to the previous one.
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(qint64 ms);
    Q_INVOKABLE void seekBy(qint64 deltaMs);
    Q_INVOKABLE void playQueueItem(int index);
    Q_INVOKABLE void removeQueueItem(int index);
    Q_INVOKABLE void moveQueueItem(int from, int to);
    Q_INVOKABLE void clearUpcoming();
    Q_INVOKABLE void toggleMute();
    Q_INVOKABLE void toggleShuffle();
    Q_INVOKABLE void cycleRepeat();

    [[nodiscard]] bool hasTrack() const noexcept { return m_queue.currentItem() != nullptr; }
    [[nodiscard]] const api::Track* currentTrack() const noexcept;
    [[nodiscard]] QString trackId() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] QString artist() const;
    [[nodiscard]] QString artworkUrl() const;
    [[nodiscard]] QString coverUrl() const;
    [[nodiscard]] QString permalink() const;
    [[nodiscard]] QString context() const;
    [[nodiscard]] bool preview() const;
    [[nodiscard]] qint64 duration() const noexcept { return m_duration; }
    [[nodiscard]] qint64 position() const;
    [[nodiscard]] bool playing() const noexcept;
    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] int errorCode() const noexcept { return m_error; }
    [[nodiscard]] double volume() const noexcept { return m_session.volume; }
    void setVolume(double volume);
    [[nodiscard]] bool muted() const noexcept { return m_session.muted; }
    void setMuted(bool muted);
    [[nodiscard]] bool shuffle() const noexcept { return m_queue.shuffle(); }
    void setShuffle(bool shuffle);
    [[nodiscard]] QString repeat() const;
    void setRepeat(const QString& repeat);
    [[nodiscard]] bool canGoNext() const noexcept { return m_queue.hasNext(); }
    [[nodiscard]] bool canGoPrevious() const noexcept { return m_queue.hasPrevious(); }
    [[nodiscard]] const QList<float>& waveform() const noexcept { return m_waveform; }
    [[nodiscard]] const QList<qreal>& cachedRanges() const noexcept { return m_cachedRanges; }
    [[nodiscard]] QueueModel* queueModel() noexcept { return &m_queueModel; }
    [[nodiscard]] const PlaybackQueue& queue() const noexcept { return m_queue; }

    // Loudness curve of the volume slider: what the mixer multiplies by.
    [[nodiscard]] static double gainFor(double volume, bool muted) noexcept;

    static constexpr qint64 kRestartThresholdMs = 3000;
    static constexpr int kAutoplayCount = 20;
    // The next item is prepared (opened and decoded ahead) this long before the current one ends, plus the
    // crossfade; earlier would download it even if the queue changes or playback stops.
    static constexpr qint64 kPrepareLeadMs = 30000;
    // How often the position is announced during normal playback.
    static constexpr qint64 kPositionIntervalMs = 250;

Q_SIGNALS:
    void trackChanged();
    void durationChanged();
    void positionChanged();
    // A jump in position (seek, restart), for MPRIS.
    void seeked(qint64 ms);
    void stateChanged();
    void volumeChanged();
    void modesChanged();
    void queueChanged();
    void waveformChanged();
    void cachedRangesChanged();
    // Something the user should know about (ErrorCode as int): shown as a toast.
    void problem(int code);

private:
    void startCurrent(std::int64_t startMs, bool paused);
    void publishCurrent();
    void prepareFollowing();
    void prefetchUpcoming();
    void playPlaylist(const api::Playlist& playlist, std::optional<bool> enqueueNext);
    void hydrateStubs();
    void hydrate(const std::vector<api::Id>& ids, std::function<void()> then);
    [[nodiscard]] std::optional<PlaybackSource> sourceFor(const QueueItem& item, ErrorCode* why = nullptr) const;
    [[nodiscard]] api::StreamPreference preference() const;
    void onAdvanced();
    void onEnded();
    void onFailed(const Error& error);
    void skipAfterError(ErrorCode code);
    void autoplay();
    void loadWaveform();
    void updateCachedRanges(bool force);
    void applyGain();
    void applySettings();
    void saveSession();
    void queueEdited();
    [[nodiscard]] static std::vector<QueueItem> itemsFrom(std::vector<api::Track> tracks, const QString& context);

    settings::SettingsStore& m_settings;
    api::IApiClient& m_api;
    cache::CacheService* m_cache;
    IPlaybackBackend& m_native;
    std::function<bool()> m_metered;
    SessionStore m_store;
    SessionState m_session;
    PlaybackQueue m_queue;
    QueueModel m_queueModel;
    std::unique_ptr<cache::Prefetcher> m_prefetcher;
    std::shared_ptr<StreamUrlResolver> m_resolver;
    QObject m_requests;  // context of API answers; deleted with the controller, which drops them
    std::uint64_t m_trackSerial = 0;
    bool m_started = false;      // the current item was handed to a backend
    bool m_pendingPlay = false;  // meant to play while the current item is still being prepared (hydrated)
    bool m_ended = false;        // the queue played to its end
    bool m_nextPrepared = false;
    int m_failuresInRow = 0;
    qint64 m_duration = 0;
    int m_error = -1;
    QList<float> m_waveform;
    QList<qreal> m_cachedRanges;
    QElapsedTimer m_rangesAge;
    QElapsedTimer m_positionAge;
    qint64 m_lastPosition = 0;
    net::RequestHandle m_waveformRequest;
    bool m_autoplaying = false;
};

}  // namespace noctara::media
