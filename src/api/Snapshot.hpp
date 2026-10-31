#pragma once

#include "api/Types.hpp"

#include "core/Error.hpp"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace noctara::api {

// A saved copy of one of the account's lists (likes, the library, a playlist's tracks).
struct Snapshot {
    std::vector<Entry> entries;  // tracks read back are marked stale (fetched again before they play)
    QString tag;                 // what it was saved for, e.g. a playlist's last_modified; empty otherwise
    bool complete = true;        // false: saved while the list was still loading
};

// Written in api-v2's own JSON shape (plus "noctara_tag" and "noctara_complete"), so the API parser reads it back.
[[nodiscard]] QByteArray encodeSnapshot(const Snapshot& snapshot);
[[nodiscard]] Result<Snapshot> decodeSnapshot(const QByteArray& json);

// The account's saved lists, kept between runs so they show at once and only need checking against SoundCloud:
// <root>/<account id>/<key>.json, read and written atomically on worker threads (the last save of a key wins).
// They are a cache: "Clear all" removes them, and so does signing out (forget()). Thread-safe.
class SnapshotStore {
public:
    explicit SnapshotStore(QString root);
    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;
    SnapshotStore(SnapshotStore&&) = delete;
    SnapshotStore& operator=(SnapshotStore&&) = delete;
    ~SnapshotStore();

    // Lists belong to this account from now on; 0 (signed out) disables loading and saving. A known account is
    // remembered (<root>/account), so the next start can show its lists before SoundCloud has confirmed who is
    // signed in.
    void setAccount(Id account);
    [[nodiscard]] Id account() const noexcept { return m_account.load(); }
    [[nodiscard]] Id lastAccount() const noexcept { return m_lastAccount.load(); }

    // Answers on `context`'s thread with nothing when there is no usable snapshot (none, unreadable, another
    // account, or the store is disabled).
    void load(const QString& key, QObject* context, std::function<void(std::optional<Snapshot>)> done) const;
    void save(const QString& key, Snapshot snapshot);
    // Removes one account's lists, or everything.
    void forget(Id account);
    void clear();
    // Waits for pending writes (tests, shutdown).
    void flush();

    [[nodiscard]] QString root() const { return m_root; }

private:
    struct Shared;
    [[nodiscard]] QString pathFor(Id account, const QString& key) const;

    QString m_root;
    std::atomic<Id> m_account{0};
    std::atomic<Id> m_lastAccount{0};
    std::shared_ptr<Shared> m_shared;
};

}  // namespace noctara::api
