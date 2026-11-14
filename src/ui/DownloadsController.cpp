#include "ui/DownloadsController.hpp"

#include "api/Streams.hpp"

#include "cache/CacheService.hpp"

namespace noctara::ui {

DownloadsController::DownloadsController(cache::CacheService& cache, api::IApiClient& client, QObject* parent)
    : QObject(parent)
    , m_cache(cache)
    , m_tracks(client)
{
    m_follow.setSingleShot(true);
    m_follow.setInterval(kFollowInterval);
    connect(&m_follow, &QTimer::timeout, this, &DownloadsController::refresh);
    // Fires for every stored piece while something downloads: re-read at most once per interval.
    connect(&m_cache, &cache::CacheService::usageChanged, this, [this] {
        if (!m_loaded) {
            return;
        }
        if (m_watching) {
            if (!m_follow.isActive()) {
                m_follow.start();
            }
        } else {
            m_dirty = true;
        }
    });
}

void DownloadsController::setWatching(bool watching)
{
    if (watching == m_watching) {
        return;
    }
    m_watching = watching;
    Q_EMIT watchingChanged();
    if (m_watching && (!m_loaded || m_dirty)) {
        refresh();
    }
}

void DownloadsController::refresh()
{
    m_dirty = false;
    m_follow.stop();
    const int request = ++m_pending;
    m_cache.completeTracks(this, [this, request](const std::vector<cache::TrackInfo>& infos) {
        if (request != m_pending) {
            return;  // a newer answer follows
        }
        std::vector<api::Entry> entries;
        entries.reserve(infos.size());
        for (const cache::TrackInfo& info : infos) {
            entries.push_back({.item = api::trackFromCache(info), .at = info.cachedAt, .repostedBy = {}});
        }
        m_tracks.applyEntries(std::move(entries));
        if (!m_loaded) {
            m_loaded = true;
            Q_EMIT loadedChanged();
        }
    });
}

void DownloadsController::remove(qint64 trackId)
{
    m_cache.removeTrack(QString::number(trackId), this, [this] { refresh(); });
}

}  // namespace noctara::ui
