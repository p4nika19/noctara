#pragma once

#include "api/ApiClient.hpp"
#include "api/Endpoints.hpp"
#include "api/Types.hpp"

#include <QAbstractListModel>
#include <QSet>

#include <functional>
#include <vector>

namespace noctara::models {

// A list of SoundCloud tracks, playlists and users for QML (search results, likes, the feed, a user's uploads,
// a playlist's tracks). Pages load lazily: views call fetchMore() near the end, which follows next_href.
// Items repeated by a later page are dropped. Tracks known only by id (a playlist's later tracks) are fetched
// when a view first asks for them, coalesced across rows into a few /tracks?ids= requests.
class CollectionModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY stateChanged)
    // ErrorCode of the last failed load (as int), -1 while there is none. QML: Format.error(errorCode).
    Q_PROPERTY(int errorCode READ errorCode NOTIFY stateChanged)
    // Shown from the cache because the network is unavailable.
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)

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
    };
    Q_ENUM(Role)

    using FirstPage = std::function<api::Endpoint<api::Page<api::Entry>>(int limit)>;

    CollectionModel(api::IApiClient& client, QObject* parent = nullptr);

    // Replaces the contents with the pages of `first` (requesting `pageSize` items at a time) and loads page one.
    void setSource(FirstPage first, int pageSize);
    // Fixed contents, such as a playlist's tracks; stubs are hydrated on demand.
    void setEntries(std::vector<api::Entry> entries);
    void clear();
    Q_INVOKABLE void reload();

    [[nodiscard]] int count() const noexcept { return static_cast<int>(m_entries.size()); }
    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] bool hasMore() const noexcept { return !m_next.isEmpty(); }
    [[nodiscard]] int errorCode() const noexcept { return m_error; }
    [[nodiscard]] bool stale() const noexcept { return m_stale; }
    [[nodiscard]] const std::vector<api::Entry>& entries() const noexcept { return m_entries; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;

    static constexpr int kArtworkPixels = 300;
    static constexpr int kThumbnailPixels = 67;

Q_SIGNALS:
    void countChanged();
    void stateChanged();

private:
    void load(const api::Endpoint<api::Page<api::Entry>>& endpoint, bool replace);
    void onPage(Result<api::Page<api::Entry>> page, bool replace, int generation);
    void append(std::vector<api::Entry> entries);
    void requestHydration(api::Id id) const;
    void hydrate();
    void onHydrated(Result<std::vector<api::Track>> tracks, const QSet<api::Id>& requested, int generation);
    void setLoading(bool loading);

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
};

}  // namespace noctara::models
