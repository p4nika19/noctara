#pragma once

#include <QByteArray>
#include <QHash>

#include <cstdint>
#include <vector>

namespace noctara::cache {

// Byte accounting and least-recently-used ordering for a disk tier. Pure bookkeeping: callers delete the files
// of the victims it picks. Not thread-safe; each tier uses it from its own thread.
class LruLedger {
public:
    struct Item {
        std::int64_t bytes = 0;
        std::int64_t lastAccess = 0;  // any monotonic clock (seconds since the epoch on disk)
        int pins = 0;                 // pinned items (open entries) are never evicted
    };

    void upsert(const QByteArray& name, std::int64_t bytes, std::int64_t lastAccess);
    void addBytes(const QByteArray& name, std::int64_t delta);
    void touch(const QByteArray& name, std::int64_t now);
    void pin(const QByteArray& name);
    void unpin(const QByteArray& name);
    void remove(const QByteArray& name);
    void clear();

    [[nodiscard]] bool contains(const QByteArray& name) const { return m_items.contains(name); }
    [[nodiscard]] const Item* find(const QByteArray& name) const;
    [[nodiscard]] std::int64_t totalBytes() const noexcept { return m_total; }
    [[nodiscard]] qsizetype size() const noexcept { return m_items.size(); }
    [[nodiscard]] const QHash<QByteArray, Item>& items() const noexcept { return m_items; }

    // Unpinned items, oldest first, until at least `bytesToFree` would be freed (all of them for a negative
    // value). `keep` is never chosen (the entry being written).
    [[nodiscard]] std::vector<QByteArray> victims(std::int64_t bytesToFree, const QByteArray& keep = {}) const;
    // Victims that bring the total down to `budget`.
    [[nodiscard]] std::vector<QByteArray> overBudget(std::int64_t budget, const QByteArray& keep = {}) const;

private:
    QHash<QByteArray, Item> m_items;
    std::int64_t m_total = 0;
};

}  // namespace noctara::cache
