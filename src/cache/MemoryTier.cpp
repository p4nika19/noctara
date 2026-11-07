#include "cache/MemoryTier.hpp"

#include <algorithm>

namespace noctara::cache {

MemoryTier::MemoryTier(std::int64_t budget)
    : m_budget(std::max<std::int64_t>(0, budget))
{
}

void MemoryTier::setBudget(std::int64_t bytes)
{
    const std::scoped_lock lock(m_mutex);
    m_budget = std::max<std::int64_t>(0, bytes);
    evictLocked(m_budget);
}

void MemoryTier::insert(const PieceKey& key, const QByteArray& data)
{
    const auto size = static_cast<std::int64_t>(data.size());
    const std::scoped_lock lock(m_mutex);
    if (size > m_budget) {
        return;
    }
    if (const auto it = m_index.find(key); it != m_index.end()) {
        eraseLocked(it->second);
    }
    evictLocked(m_budget - size);
    const bool hot = m_active.contains(key.entry);
    List& list = hot ? m_hot : m_cold;
    list.push_front(Node{key, data, hot});
    m_index.emplace(key, list.begin());
    m_stats.bytes += size;
    ++m_stats.pieces;
}

std::optional<QByteArray> MemoryTier::find(const PieceKey& key)
{
    const std::scoped_lock lock(m_mutex);
    const auto it = m_index.find(key);
    if (it == m_index.end()) {
        ++m_stats.misses;
        return std::nullopt;
    }
    ++m_stats.hits;
    List& list = it->second->hot ? m_hot : m_cold;
    list.splice(list.begin(), list, it->second);
    return it->second->data;
}

bool MemoryTier::contains(const PieceKey& key) const
{
    const std::scoped_lock lock(m_mutex);
    return m_index.contains(key);
}

void MemoryTier::setActive(std::uint64_t entry, bool active)
{
    const std::scoped_lock lock(m_mutex);
    if (active ? !m_active.insert(entry).second : m_active.erase(entry) == 0) {
        return;
    }
    // Move the entry's pieces between the lists, keeping their relative order.
    List& from = active ? m_cold : m_hot;
    List& to = active ? m_hot : m_cold;
    List moved;  // collected in order, then put in front as one group
    for (auto it = from.begin(); it != from.end();) {
        const auto next = std::next(it);
        if (it->key.entry == entry) {
            it->hot = active;
            moved.splice(moved.end(), from, it);
        }
        it = next;
    }
    to.splice(to.begin(), moved);
}

void MemoryTier::dropEntry(std::uint64_t entry)
{
    const std::scoped_lock lock(m_mutex);
    for (List* list : {&m_cold, &m_hot}) {
        for (auto it = list->begin(); it != list->end();) {
            const auto next = std::next(it);
            if (it->key.entry == entry) {
                eraseLocked(it);
            }
            it = next;
        }
    }
}

void MemoryTier::trimTo(std::int64_t bytes)
{
    const std::scoped_lock lock(m_mutex);
    evictLocked(std::max<std::int64_t>(0, bytes));
}

void MemoryTier::clear()
{
    const std::scoped_lock lock(m_mutex);
    m_cold.clear();
    m_hot.clear();
    m_index.clear();
    m_stats.bytes = 0;
    m_stats.pieces = 0;
}

MemoryTier::Stats MemoryTier::stats() const
{
    const std::scoped_lock lock(m_mutex);
    Stats stats = m_stats;
    stats.budget = m_budget;
    return stats;
}

void MemoryTier::evictLocked(std::int64_t target)
{
    while (m_stats.bytes > target && !(m_cold.empty() && m_hot.empty())) {
        List& list = m_cold.empty() ? m_hot : m_cold;
        eraseLocked(std::prev(list.end()));
        ++m_stats.evictions;
    }
}

void MemoryTier::eraseLocked(List::iterator it)
{
    m_stats.bytes -= static_cast<std::int64_t>(it->data.size());
    --m_stats.pieces;
    m_index.erase(it->key);
    (it->hot ? m_hot : m_cold).erase(it);
}

}  // namespace noctara::cache
