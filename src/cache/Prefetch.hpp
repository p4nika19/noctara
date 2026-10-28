#pragma once

#include "cache/AudioCache.hpp"

#include <QObject>

#include <cstdint>
#include <vector>

namespace noctara::cache {

// How many upcoming queue items to warm: none on a metered connection, at most one when the audio cache is off
// (it then only lives in RAM), otherwise the configured count.
[[nodiscard]] int effectivePrefetchCount(int configured, bool metered, bool audioCacheEnabled) noexcept;
// How much of each item to fetch: everything when it can be kept on disk, the start otherwise.
[[nodiscard]] std::int64_t prefetchBytes(bool diskAvailable) noexcept;

struct PrefetchItem {
    TrackInfo info;
    UrlProvider urls;
};

// Warms the next queue items into the audio cache without disturbing the current stream: their fetches are
// background requests (skipped on metered connections), and a new queue cancels the previous prefetch.
// DRM tracks are skipped (they are never cached; warming the helper is the DRM backend's business).
class Prefetcher : public QObject {
    Q_OBJECT

public:
    explicit Prefetcher(AudioCache& cache, QObject* parent = nullptr);

    void setQueue(std::vector<PrefetchItem> upcoming, int count, std::int64_t maxBytes);
    void cancel();
    [[nodiscard]] std::size_t active() const noexcept { return m_readers.size(); }

private:
    AudioCache& m_cache;
    std::vector<AudioReaderPtr> m_readers;
    std::uint64_t m_round = 0;
};

}  // namespace noctara::cache
