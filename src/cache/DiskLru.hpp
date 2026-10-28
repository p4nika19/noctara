#pragma once

#include "cache/LruLedger.hpp"
#include "core/Error.hpp"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <cstdint>

namespace noctara::cache {

class FileOps;

// A directory of opaque blobs (images, HTTP responses) with a byte budget and LRU eviction. Blobs are written
// atomically; the index (sizes and access times) is advisory: a lost or stale index is rebuilt from the files,
// and a blob that vanished is simply a miss. Not thread-safe: used on the cache thread only.
class DiskLru {
public:
    DiskLru(QString directory, std::int64_t budget, FileOps& ops);

    // Loads the index (or rebuilds it from the directory). Cheap to call again.
    void load();
    [[nodiscard]] bool loaded() const noexcept { return m_loaded; }

    [[nodiscard]] Result<QByteArray> get(const QByteArray& name);
    [[nodiscard]] bool contains(const QByteArray& name) const { return m_ledger.contains(name); }
    // Stores the blob, then evicts the least recently used others to stay within the budget. A budget of 0
    // disables the tier (nothing is stored).
    [[nodiscard]] Result<void> put(const QByteArray& name, QByteArrayView data);
    void remove(const QByteArray& name);
    void clear();
    void setBudget(std::int64_t bytes);

    [[nodiscard]] std::int64_t bytes() const noexcept { return m_ledger.totalBytes(); }
    [[nodiscard]] std::int64_t budget() const noexcept { return m_budget; }
    [[nodiscard]] qsizetype count() const noexcept { return m_ledger.size(); }
    [[nodiscard]] const QString& directory() const noexcept { return m_directory; }

    // Writes the index if anything changed since the last flush.
    void flushIndex();

private:
    [[nodiscard]] QString pathFor(const QByteArray& name) const;
    void rebuild();
    void evict();

    QString m_directory;
    std::int64_t m_budget;
    FileOps& m_ops;
    LruLedger m_ledger;
    bool m_loaded = false;
    bool m_dirty = false;
};

[[nodiscard]] std::int64_t unixNow();
// Milliseconds since the epoch, strictly increasing within the process: orders LRU accesses without ties.
[[nodiscard]] std::int64_t accessStamp();

}  // namespace noctara::cache
