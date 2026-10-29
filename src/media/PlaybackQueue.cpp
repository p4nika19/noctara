#include "media/PlaybackQueue.hpp"

#include <QRandomGenerator>

#include <algorithm>
#include <unordered_map>

namespace noctara::media {

std::uint64_t PlaybackQueue::assign(std::vector<QueueItem>& items)
{
    const std::uint64_t first = m_nextUid;
    for (QueueItem& item : items) {
        item.uid = m_nextUid++;
    }
    return first;
}

const QueueItem* PlaybackQueue::currentItem() const noexcept
{
    return m_current >= 0 && m_current < size() ? &m_items[static_cast<std::size_t>(m_current)] : nullptr;
}

void PlaybackQueue::replace(std::vector<QueueItem> items, int start)
{
    assign(items);
    m_items = std::move(items);
    m_current = m_items.empty() ? -1 : std::clamp(start, 0, size() - 1);
    m_original.clear();
    if (m_shuffle && !m_items.empty()) {
        for (const QueueItem& item : m_items) {
            m_original.push_back(item.uid);  // as given, for when shuffle is turned off
        }
        // The chosen item plays first; the rest follows in random order.
        std::rotate(m_items.begin(), m_items.begin() + m_current, m_items.begin() + m_current + 1);
        m_current = 0;
        shuffleAfterCurrent();
    }
}

void PlaybackQueue::restore(std::vector<QueueItem> items, int current, bool shuffle)
{
    assign(items);
    m_items = std::move(items);
    m_current = m_items.empty() ? -1 : std::clamp(current, 0, size() - 1);
    m_shuffle = shuffle;
    m_original.clear();
    if (shuffle) {
        // The order before shuffling is gone; turning shuffle off keeps this one.
        for (const QueueItem& item : m_items) {
            m_original.push_back(item.uid);
        }
    }
}

void PlaybackQueue::append(std::vector<QueueItem> items)
{
    assign(items);
    for (QueueItem& item : items) {
        if (m_shuffle) {
            m_original.push_back(item.uid);
        }
        m_items.push_back(std::move(item));
    }
    if (m_current < 0 && !m_items.empty()) {
        m_current = 0;
    }
}

void PlaybackQueue::insertNext(std::vector<QueueItem> items)
{
    assign(items);
    const auto at = m_items.begin() + (m_current + 1);
    if (m_shuffle) {
        // In the original order, too, right after the current item.
        const std::uint64_t currentUid = m_current >= 0 ? m_items[static_cast<std::size_t>(m_current)].uid : 0;
        auto position = std::ranges::find(m_original, currentUid);
        position = position == m_original.end() ? m_original.begin() : position + 1;
        for (const QueueItem& item : items) {
            position = m_original.insert(position, item.uid) + 1;
        }
    }
    m_items.insert(at, std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    if (m_current < 0 && !m_items.empty()) {
        m_current = 0;
    }
}

bool PlaybackQueue::remove(int index)
{
    if (index < 0 || index >= size()) {
        return false;
    }
    const std::uint64_t uid = m_items[static_cast<std::size_t>(index)].uid;
    std::erase(m_original, uid);
    m_items.erase(m_items.begin() + index);
    if (index < m_current) {
        --m_current;
    } else if (index == m_current && m_current >= size()) {
        m_current = size() - 1;  // the current, last item went: the one before it becomes current
    }
    if (m_items.empty()) {
        m_current = -1;
    }
    return true;
}

bool PlaybackQueue::move(int from, int to)
{
    if (from < 0 || from >= size() || to < 0 || to >= size() || from == to) {
        return false;
    }
    QueueItem item = std::move(m_items[static_cast<std::size_t>(from)]);
    m_items.erase(m_items.begin() + from);
    m_items.insert(m_items.begin() + to, std::move(item));
    if (m_current == from) {
        m_current = to;
    } else if (from < m_current && to >= m_current) {
        --m_current;
    } else if (from > m_current && to <= m_current) {
        ++m_current;
    }
    return true;
}

void PlaybackQueue::clear()
{
    m_items.clear();
    m_original.clear();
    m_current = -1;
}

bool PlaybackQueue::jump(int index)
{
    if (index < 0 || index >= size()) {
        return false;
    }
    m_current = index;
    return true;
}

void PlaybackQueue::shuffleAfterCurrent()
{
    // Uniform, unweighted; fresh randomness on every shuffle.
    std::shuffle(m_items.begin() + (m_current + 1), m_items.end(), *QRandomGenerator::global());
}

void PlaybackQueue::setShuffle(bool shuffle)
{
    if (shuffle == m_shuffle) {
        return;
    }
    m_shuffle = shuffle;
    if (shuffle) {
        m_original.clear();
        for (const QueueItem& item : m_items) {
            m_original.push_back(item.uid);
        }
        // Items already played stay where they are; only what is still to come is shuffled.
        shuffleAfterCurrent();
        return;
    }
    // Back to the original order, the current item staying current.
    std::unordered_map<std::uint64_t, std::size_t> rank;
    for (std::size_t i = 0; i < m_original.size(); ++i) {
        rank.emplace(m_original[i], i);
    }
    const std::uint64_t currentUid = m_current >= 0 ? m_items[static_cast<std::size_t>(m_current)].uid : 0;
    std::ranges::stable_sort(m_items, [&](const QueueItem& a, const QueueItem& b) {
        const auto ra = rank.find(a.uid);
        const auto rb = rank.find(b.uid);
        const std::size_t ka = ra == rank.end() ? rank.size() : ra->second;
        const std::size_t kb = rb == rank.end() ? rank.size() : rb->second;
        return ka < kb;
    });
    m_original.clear();
    if (m_current >= 0) {
        m_current = static_cast<int>(std::ranges::find(m_items, currentUid, &QueueItem::uid) - m_items.begin());
    }
}

std::optional<int> PlaybackQueue::following() const noexcept
{
    if (m_current < 0) {
        return std::nullopt;
    }
    if (m_repeat == Repeat::One) {
        return m_current;
    }
    if (m_current + 1 < size()) {
        return m_current + 1;
    }
    if (m_repeat == Repeat::All) {
        return 0;
    }
    return std::nullopt;
}

std::optional<int> PlaybackQueue::advance(bool skipped)
{
    std::optional<int> next;
    if (skipped && m_repeat == Repeat::One) {
        if (m_current + 1 < size()) {
            next = m_current + 1;
        } else if (!m_items.empty()) {
            next = 0;
        }
    } else {
        next = following();
    }
    if (next) {
        m_current = *next;
    }
    return next;
}

std::optional<int> PlaybackQueue::previous()
{
    if (m_current < 0) {
        return std::nullopt;
    }
    if (m_current > 0) {
        return --m_current;
    }
    if (m_repeat != Repeat::Off && !m_items.empty()) {
        m_current = size() - 1;
        return m_current;
    }
    return std::nullopt;
}

bool PlaybackQueue::hasNext() const noexcept
{
    return m_current >= 0 && (m_current + 1 < size() || m_repeat != Repeat::Off);
}

bool PlaybackQueue::hasPrevious() const noexcept
{
    return m_current > 0 || (m_current == 0 && m_repeat != Repeat::Off);
}

void PlaybackQueue::updateTrack(const api::Track& track)
{
    for (QueueItem& item : m_items) {
        if (item.track.id == track.id) {
            item.track = track;
        }
    }
}

}  // namespace noctara::media
