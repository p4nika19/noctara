#pragma once

#include "api/ApiClient.hpp"
#include "api/Endpoints.hpp"
#include "api/Snapshot.hpp"
#include "api/Types.hpp"

#include <QAbstractListModel>
#include <QSet>
#include <QTimer>

#include <functional>
#include <vector>

namespace noctara::models {

// A list of SoundCloud tracks, playlists and users for QML (search results, likes, the feed, a user's uploads,
// a playlist's tracks). Pages load lazily: views call fetchMore() near the end, which follows next_href. A synced
// list (likes, the library) instead loads every page by itself, shows its saved snapshot meanwhile, and then
// changes only the rows that differ (see setSyncedSource()).
// Items repeated by a later page are dropped. Tracks known only by id (a playlist's later tracks) are fetched
// when a view first asks for them, coalesced across rows into a few /tracks?ids= requests. Playlists listed without
// artwork or tracks (the library) get their first track's cover the same way: fetched when a view shows them.
class CollectionModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    // ErrorCode of the last failed load (as int), -1 while there is none. QML: Format.error(errorCode).
    Q_PROPERTY(int errorCode READ errorCode NOTIFY stateChanged)
    // Shown from the cache because the network is unavailable.
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)
    // Every item is known (a synced list finished, or a paged one reached its end).
    Q_PROPERTY(bool complete READ complete NOTIFY stateChanged)

public:
    enum Role {
        KindRole = Qt::UserRole + 1,  // "track", "playlist", "user"
        IdRole,
        TitleRole,      // track or playlist title, or the user's name
        SubtitleRole,   // artist, playlist owner, or the user's city
        ArtworkRole,    // a 300 px variant (cover, or avatar for users)
        ThumbnailRole,  // a 67 px variant
        DurationRole,   // ms (tracks, playlists)
        PlaysRole,
        LikesRole,
        RepostsRole,
        FollowersRole,
        TrackCountRole,  // playlists, users
        GenreRole,
        PermalinkRole,
        CreatedRole,
        AtRole,          // when it was liked, played or posted to the feed
        RepostedByRole,  // the reposter's name in the feed
        ProtectedRole,   // DRM: plays through the helper and is never cached ("not cacheable")
        PreviewRole,     // only a preview is available to this account
        VerifiedRole,
        AlbumRole,
        PendingRole,  // a track known only by id, being fetched
        OwnerIdRole,  // the uploader (tracks), owner (playlists) or the user itself
    };
    Q_ENUM(Role)

    using FirstPage = std::function<api::Endpoint<api::Page<api::Entry>>(int limit)>;

    CollectionModel(api::IApiClient& client, QObject* parent = nullptr);

    // Replaces the contents with the pages of `first` (requesting `pageSize` items at a time) and loads page one.
    void setSource(FirstPage first, int pageSize);
    // Every page of `first`, fetched one after another without waiting for the view. The snapshot `key` in `store`
    // shows at once if there is one; the fresh list then replaces it row by row and is saved. Without a snapshot
    // the rows appear as pages arrive (and progress is saved every few pages).
    void setSyncedSource(FirstPage first, api::SnapshotStore* store, QString key);
    // Fixed contents, such as a playlist's tracks; stubs are hydrated on demand.
    void setEntries(std::vector<api::Entry> entries);
    // Replaces the contents with `fresh`, changing only what differs (rows removed, inserted, data refreshed), so
    // views keep their position. Falls back to a reset when items changed their relative order.
    void applyEntries(std::vector<api::Entry> fresh);
    // Fetches every stub now instead of when it is shown; hydrated() follows when none is left.
    void hydrateAll();
    void clear();
    Q_INVOKABLE void reload();

    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_entries.size()); }
    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] bool hasMore() const noexcept { return !m_next.isEmpty(); }
    [[nodiscard]] int errorCode() const noexcept { return m_error; }
    [[nodiscard]] bool stale() const noexcept { return m_stale; }
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool synced() const noexcept { return m_store != nullptr; }
    [[nodiscard]] const std::vector<api::Entry>& entries() const noexcept { return m_entries; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    static constexpr int kArtworkPixels = 300;
    static constexpr int kSyncPageSize = 200;  // a synced list asks for big pages: fewer round trips
    static constexpr int kSaveEveryPages = 5;  // a first sync saves its progress this often
    static constexpr int kThumbnailPixels = 67;

Q_SIGNALS:
    void countChanged();
    void stateChanged();
    // Items were added, removed or replaced (not just their data refreshed): for the player's queue.
    void listChanged();
    // The contents are about to be replaced by another list's (clear(), a new source, a paged list reloaded from
    // page one) rather than edited: a queue following the old list stops following.
    void sourceReset();
    // hydrateAll() finished: no stubs are left.
    void hydrated();

private:
    void load(const api::Endpoint<api::Page<api::Entry>>& endpoint, bool replace);
    void onPage(Result<api::Page<api::Entry>> page, bool replace, int generation);
    void append(std::vector<api::Entry> entries);
    void requestHydration(api::Id id) const;
    void requestCover(api::Id id) const;
    void fetchCovers();
    void hydrate();
    void onHydrated(Result<std::vector<api::Track>> tracks, const QSet<api::Id>& requested, int generation);
    void setLoading(bool loading);
    void syncPage(const QString& href, int generation);
    void onSyncPage(Result<api::Page<api::Entry>> page, int generation);
    void saveSnapshot(bool complete);
    void finishHydrateAll();

    api::IApiClient& m_client;
    FirstPage m_first;
    int m_pageSize = 50;
    std::vector<api::Entry> m_entries;
    QSet<quint64> m_seen;  // kind + id: ids are unique within tracks, playlists and users, not across them
    QString m_next;
    net::RequestHandle m_request;
    bool m_loading = false;
    int m_error = -1;
    bool m_stale = false;
    int m_generation = 0;  // bumped on every reset: answers to older requests are ignored
    // Hydration: requested from data() (const), sent once per event-loop turn.
    mutable QSet<api::Id> m_wanted;
    mutable bool m_hydrationQueued = false;
    QSet<api::Id> m_requested;
    // Synced lists.
    api::SnapshotStore* m_store = nullptr;
    QString m_key;
    std::vector<api::Entry> m_fresh;  // the pages fetched so far by the running sync
    QSet<quint64> m_freshSeen;
    bool m_showingSnapshot = false;  // the rows are the snapshot's until the sync completes
    bool m_synced = false;           // the last sync completed
    int m_syncPages = 0;
    QTimer m_resave;  // covers found after a sync are saved too, a moment after the last one
    bool m_hydrateAll = false;
    // Covers of playlists listed without one: wanted from data(), fetched a few at a time, each id once.
    mutable QList<api::Id> m_coverQueue;
    mutable QSet<api::Id> m_coverAsked;
    int m_coverRequests = 0;
    static constexpr int kCoverRequests = 4;
};

}  // namespace noctara::models
