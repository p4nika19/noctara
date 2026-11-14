#pragma once

#include "models/CollectionModel.hpp"

#include <QObject>
#include <QTimer>

#include <chrono>

namespace noctara::cache {
class CacheService;
}

namespace noctara::ui {

// QML "Downloads" singleton: the tracks stored in full in the audio cache (the Downloaded page), most recently
// cached first, playable offline. The list loads when the page first shows and follows the cache while the page
// is on screen (`watching`); rows change one by one, so the view keeps its place.
class DownloadsController : public QObject {
    Q_OBJECT
    Q_PROPERTY(noctara::models::CollectionModel* tracks READ tracks CONSTANT)
    // The cache has answered at least once.
    Q_PROPERTY(bool loaded READ loaded NOTIFY loadedChanged)
    // The page is on screen: changes in the cache show up as they happen.
    Q_PROPERTY(bool watching READ watching WRITE setWatching NOTIFY watchingChanged)

public:
    DownloadsController(cache::CacheService& cache, api::IApiClient& client, QObject* parent = nullptr);

    [[nodiscard]] models::CollectionModel* tracks() noexcept { return &m_tracks; }
    [[nodiscard]] bool loaded() const noexcept { return m_loaded; }
    [[nodiscard]] bool watching() const noexcept { return m_watching; }
    void setWatching(bool watching);

    Q_INVOKABLE void refresh();
    // Deletes the stored copy of a track (every quality of it).
    Q_INVOKABLE void remove(qint64 trackId);

    static constexpr std::chrono::milliseconds kFollowInterval{1500};

Q_SIGNALS:
    void loadedChanged();
    void watchingChanged();

private:
    cache::CacheService& m_cache;
    models::CollectionModel m_tracks;
    QTimer m_follow;  // coalesces the cache's changes while watching
    bool m_loaded = false;
    bool m_watching = false;
    bool m_dirty = false;  // the cache changed while nobody was watching
    int m_pending = 0;     // answers on their way; only the last one counts
};

}  // namespace noctara::ui
