#pragma once

#include "api/Types.hpp"

#include <QString>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace noctara::media {

enum class Repeat : std::uint8_t { Off, All, One };

struct QueueItem {
    api::Track track;
    QString context;        // the playlist or page it was started from
    std::uint64_t uid = 0;  // assigned by the queue; stable while the item is queued
};

// The play queue: items in playing order and the current one. Shuffling reorders the items after the current one
// (and remembers the original order to return to); repeat decides what follows the last item. Pure logic.
class PlaybackQueue {
public:
    // Replaces the queue; `start` becomes current. With shuffle on, the others are shuffled around it.
    void replace(std::vector<QueueItem> items, int start);
    void append(std::vector<QueueItem> items);
    // After the current item.
    void insertNext(std::vector<QueueItem> items);
    bool remove(int index);
    bool move(int from, int to);
    void clear();
    // Makes `index` current.
    bool jump(int index);

    void setShuffle(bool shuffle, std::uint32_t seed);
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

private:
    std::uint64_t assign(std::vector<QueueItem>& items);
    void shuffleAfterCurrent(std::uint32_t seed);

    std::vector<QueueItem> m_items;
    std::vector<std::uint64_t> m_original;  // order before shuffling (uids)
    int m_current = -1;
    bool m_shuffle = false;
    Repeat m_repeat = Repeat::Off;
    std::uint64_t m_nextUid = 1;
    std::uint32_t m_seed = 0;
};

}  // namespace noctara::media
