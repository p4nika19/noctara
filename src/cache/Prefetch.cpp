#include "cache/Prefetch.hpp"

#include <algorithm>

namespace noctara::cache {

namespace {

constexpr std::int64_t kRamPrefetchBytes = 1024LL * 1024;  // enough for an instant start

}  // namespace

int effectivePrefetchCount(int configured, bool metered, bool audioCacheEnabled) noexcept
{
    if (metered || configured <= 0) {
        return 0;
    }
    return audioCacheEnabled ? configured : std::min(configured, 1);
}

std::int64_t prefetchBytes(bool diskAvailable) noexcept
{
    return diskAvailable ? -1 : kRamPrefetchBytes;
}

Prefetcher::Prefetcher(AudioCache& cache, QObject* parent)
    : QObject(parent)
    , m_cache(cache)
{
}

void Prefetcher::setQueue(std::vector<PrefetchItem> upcoming, int count, std::int64_t maxBytes)
{
    cancel();
    const std::uint64_t round = m_round;
    int started = 0;
    for (PrefetchItem& item : upcoming) {
        if (started >= count) {
            break;
        }
        if (item.info.drm) {
            continue;
        }
        ++started;
        m_cache.open(
            item.info, std::move(item.urls), this,
            [this, round, maxBytes](Result<AudioReaderPtr> reader) {
                if (reader && round == m_round) {
                    (*reader)->prefetch(maxBytes);
                    m_readers.push_back(std::move(*reader));
                }
            },
            true);
    }
}

void Prefetcher::cancel()
{
    ++m_round;
    m_readers.clear();  // closing the readers stops their background fetches
}

}  // namespace noctara::cache
