#pragma once

#include <QByteArray>

#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace noctara::cache {

// Identifies one stored piece of encoded audio: a fixed-size chunk of a progressive stream or one HLS segment.
// The generation changes when an entry's file is rewritten (tagging), so pieces of the old layout never mix
// with the new one.
struct PieceKey {
    std::uint64_t entry = 0;
    std::uint32_t generation = 0;
    std::uint32_t piece = 0;

    friend bool operator==(const PieceKey&, const PieceKey&) = default;
};

struct PieceKeyHash {
    std::size_t operator()(const PieceKey& key) const noexcept
    {
        return static_cast<std::size_t>(key.entry ^ (std::uint64_t{key.generation} << 32U) ^ key.piece);
    }
};

// Tier 1 of the audio cache: a byte-budgeted store of encoded pieces in RAM. Pieces are immutable implicitly
// shared QByteArrays, so a hit hands out the buffer without copying. Pieces of active entries (the current and
// prefetched tracks) are evicted only after every piece of inactive ones. Thread-safe.
class MemoryTier {
public:
    struct Stats {
        std::int64_t bytes = 0;
        std::int64_t budget = 0;
        std::int64_t pieces = 0;
        std::int64_t hits = 0;
        std::int64_t misses = 0;
        std::int64_t evictions = 0;
    };

    explicit MemoryTier(std::int64_t budget);

    void setBudget(std::int64_t bytes);
    void insert(const PieceKey& key, const QByteArray& data);
    [[nodiscard]] std::optional<QByteArray> find(const PieceKey& key);
    [[nodiscard]] bool contains(const PieceKey& key) const;
    void setActive(std::uint64_t entry, bool active);
    void dropEntry(std::uint64_t entry);
    // Memory pressure: shrinks to `bytes` now (inactive pieces first); the budget itself is unchanged.
    void trimTo(std::int64_t bytes);
    void clear();
    [[nodiscard]] Stats stats() const;

private:
    struct Node {
        PieceKey key;
        QByteArray data;
        bool hot = false;
    };
    using List = std::list<Node>;

    void evictLocked(std::int64_t target);
    void eraseLocked(List::iterator it);

    mutable std::mutex m_mutex;
    List m_cold;  // front = most recently used
    List m_hot;
    std::unordered_map<PieceKey, List::iterator, PieceKeyHash> m_index;
    std::unordered_set<std::uint64_t> m_active;
    std::int64_t m_budget;
    Stats m_stats;
};

}  // namespace noctara::cache
