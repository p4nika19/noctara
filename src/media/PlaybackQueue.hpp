#pragma once

#include "api/Types.hpp"

#include <QString>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace noctara::media {

enum class Repeat : std::uint8_t { Off, All, One };
// "off", "all", "one": the QML property values and the saved session's spelling. Unknown names are Off.
[[nodiscard]] QString repeatName(Repeat repeat);
[[nodiscard]] Repeat repeatFromName(QStringView name);

struct QueueItem {
    api::Track track;
    QString context;        // the playlist or page it was started from
    std::uint64_t uid = 0;  // assigned by the queue; stable while the item is queued
};

// The play queue: items in playing order and the current one. Shuffling reorders the items after the current one
// with a uniform Fisher–Yates shuffle that draws fresh randomness from QRandomGenerator::global() (seeded from the
// operating system's entropy source) every time, and remembers the original order to return to; repeat decides
// what follows the last item.
class PlaybackQueue {
public:
    // Replaces the queue; `start` becomes current. With shuffle on, the others are shuffled around it.
    void replace(std::vector<QueueItem> items, int start);
    // Puts back a queue exactly as it was saved (already in playing order: nothing is shuffled again).
    void restore(std::vector<QueueItem> items, int current, bool shuffle);
    void append(std::vector<QueueItem> items);
    // After the current item.
    void insertNext(std::vector<QueueItem> items);
    // Items the list being played grew by: appended, or with shuffle on each put at a uniformly random place among
    // the upcoming items (the same as shuffling the rest of the queue again together with them). In the original
    // order they go at the end. Returns their uids.
    std::vector<std::uint64_t> addUpcoming(std::vector<QueueItem> items);
    bool remove(int index);
    // Everything after the current item.
    void removeUpcoming();
    bool move(int from, int to);
    void clear();
    // Makes `index` current.
    bool jump(int index);

    void setShuffle(bool shuffle);
    void setRepeat(Repeat repeat) noexcept { m_repeat = repeat; }
    [[nodiscard]] bool shuffle() const noexcept { return m_shuffle; }
    [[nodiscard]] Repeat repeat() const noexcept { return m_repeat; }

    [[nodiscard]] int current() const noexcept { return m_current; }
    [[nodiscard]] const QueueItem* currentItem() const noexcept;
    [[nodiscard]] std::span<const QueueItem> items() const noexcept { return m_items; }
    [[nodiscard]] int size() const noexcept { return static_cast<int>(m_items.size()); }
    [[nodiscard]] bool empty() const noexcept { return m_items.empty(); }

    // What plays after the current item when it ends by itself (Repeat One: itself; Repeat All: wraps around).
    [[nodiscard]] std::optional<int> following() const noexcept;
    // Moves on when the current item ended (automatic) or the user skipped (Repeat One then moves on too).
    std::optional<int> advance(bool skipped);
    // The item before the current one (Repeat All wraps around).
    std::optional<int> previous();
    [[nodiscard]] bool hasNext() const noexcept;
    [[nodiscard]] bool hasPrevious() const noexcept;

    // Replaces a track by id (a stub hydrated, a refreshed copy).
    void updateTrack(const api::Track& track);
    // Clears the stale mark of a track by id (refreshing it failed: it plays as saved).
    void markFresh(api::Id id);

private:
    std::uint64_t assign(std::vector<QueueItem>& items);
    void shuffleAfterCurrent();

    std::vector<QueueItem> m_items;
    std::vector<std::uint64_t> m_original;  // order before shuffling (uids)
    int m_current = -1;
    bool m_shuffle = false;
    Repeat m_repeat = Repeat::Off;
    std::uint64_t m_nextUid = 1;
};

}  // namespace noctara::media
