#pragma once

#include "models/CollectionModel.hpp"
#include "models/Details.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

#include <deque>
#include <memory>

namespace noctara::api {
class ApiService;
}

namespace noctara::ui {

class AccountController;

// QML "Catalog" singleton: SoundCloud content for the pages. Search: typing searches after a short pause (Enter
// searches at once), and the kind ("all", "tracks", "playlists", "albums", "users") narrows it. The feed, likes,
// history, library and following lists belong to the signed-in account and load on first use (again after a
// sign-in or sign-out); charts are SoundCloud's trending tracks by genre. Playlist and user pages are detail objects,
// kept for a while so going back is instant.
class CatalogController : public QObject {
    Q_OBJECT
    Q_PROPERTY(noctara::models::CollectionModel* searchResults READ searchResults CONSTANT)
    Q_PROPERTY(QString query READ query NOTIFY queryChanged)
    Q_PROPERTY(QString searchKind READ searchKind NOTIFY queryChanged)
    Q_PROPERTY(noctara::models::CollectionModel* feed READ feed CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* likes READ likes CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* history READ history CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* library READ library CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* following READ following CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* charts READ charts CONSTANT)
    Q_PROPERTY(QString chartGenre READ chartGenre NOTIFY chartChanged)
    // [{id, label}] with translated labels.
    Q_PROPERTY(QVariantList genres READ genres NOTIFY genresChanged)
    // Bumped whenever the liked state of a track may have changed: bindings that call isLiked() read it.
    Q_PROPERTY(int likedRevision READ likedRevision NOTIFY likedChanged)

public:
    // `snapshots` keeps the account's likes, library and playlists between runs.
    CatalogController(api::ApiService& api, AccountController& account, api::SnapshotStore& snapshots,
                      QObject* parent = nullptr);

    [[nodiscard]] models::CollectionModel* searchResults() noexcept { return &m_search; }
    [[nodiscard]] models::CollectionModel* feed() noexcept { return &m_feed; }
    [[nodiscard]] models::CollectionModel* likes() noexcept { return &m_likes; }
    [[nodiscard]] models::CollectionModel* history() noexcept { return &m_history; }
    [[nodiscard]] models::CollectionModel* library() noexcept { return &m_library; }
    [[nodiscard]] models::CollectionModel* following() noexcept { return &m_following; }
    [[nodiscard]] models::CollectionModel* charts() noexcept { return &m_charts; }
    [[nodiscard]] const QString& chartGenre() const noexcept { return m_chartGenre; }
    [[nodiscard]] QVariantList genres() const;

    // Loads a list the first time a page shows it: "feed", "likes", "history", "library", "following", "charts".
    Q_INVOKABLE void ensure(const QString& list);
    Q_INVOKABLE void setChartGenre(const QString& genre);
    // Detail pages (owned here).
    Q_INVOKABLE noctara::models::PlaylistDetail* playlist(qint64 id);
    Q_INVOKABLE noctara::models::UserDetail* user(qint64 id);
    Q_INVOKABLE noctara::models::TrackDetail* track(qint64 id);

    // Likes of the signed-in account. toggleLike() shows the new state at once and puts it back if SoundCloud
    // refuses (with a notice).
    [[nodiscard]] int likedRevision() const noexcept { return m_likedRevision; }
    Q_INVOKABLE bool isLiked(qint64 trackId) const;
    Q_INVOKABLE void toggleLike(qint64 trackId);
    // The account's own playlists (not albums, not liked ones), from the library: [{id, title, trackCount,
    // artwork}]. Loads the library on first use.
    Q_INVOKABLE QVariantList ownPlaylists();
    // Adds the track at the end of one of the account's playlists (unless it is already there) or to a new one.
    // Changes to one playlist are made one after another; the outcome is a notice.
    Q_INVOKABLE void addToPlaylist(qint64 playlistId, qint64 trackId);
    Q_INVOKABLE void createPlaylist(const QString& title, bool isPrivate, qint64 trackId);
    [[nodiscard]] const QString& query() const noexcept { return m_query; }
    [[nodiscard]] const QString& searchKind() const noexcept { return m_kind; }

    // Debounced: the search starts once typing pauses.
    Q_INVOKABLE void search(const QString& text, const QString& kind);
    // Starts the pending search immediately (Enter).
    Q_INVOKABLE void searchNow();

    static constexpr std::chrono::milliseconds kTypingPause{350};

    static constexpr int kKeptDetails = 12;

Q_SIGNALS:
    void queryChanged();
    void chartChanged();
    void genresChanged();
    void likedChanged();
    // kind: "info", "success", "warning", "error" (App.showToast).
    void notice(const QString& kind, const QString& message);

private:
    void start();
    void accountChanged();
    void load(const QString& list);
    [[nodiscard]] api::Id accountId();
    // The cached page for `id` (created with `make` when missing), kept among the most recently opened ones.
    template <class Detail, class Make>
    Detail* detail(QHash<qint64, std::shared_ptr<Detail>>& cache, QList<qint64>& recent, qint64 id, Make make);
    void likesChanged();
    void writeNextToPlaylist(api::Id playlistId);
    void finishPlaylistWrite(api::Id playlistId);
    // A playlist changed by the account: the open page and the library show the new version.
    void playlistUpdated(const api::Playlist& playlist);

    api::ApiService& m_api;
    AccountController& m_account;
    api::SnapshotStore& m_snapshots;
    models::CollectionModel m_search;
    models::CollectionModel m_feed;
    models::CollectionModel m_likes;
    models::CollectionModel m_history;
    models::CollectionModel m_library;
    models::CollectionModel m_following;
    models::CollectionModel m_charts;
    QStringList m_loaded;  // lists loaded for the current account
    qint64 m_accountId = -1;
    QString m_chartGenre = QStringLiteral("all-genres");
    QHash<qint64, std::shared_ptr<models::PlaylistDetail>> m_playlists;
    QHash<qint64, std::shared_ptr<models::UserDetail>> m_users;
    QHash<qint64, std::shared_ptr<models::TrackDetail>> m_tracks;
    QList<qint64> m_recentPlaylists;  // least recent first
    QList<qint64> m_recentUsers;
    QList<qint64> m_recentTracks;
    QSet<api::Id> m_likedIds;              // from the likes list
    QHash<api::Id, bool> m_likeOverrides;  // toggled, SoundCloud's answer pending
    int m_likedRevision = 0;
    QHash<api::Id, std::deque<api::Id>> m_playlistWrites;  // playlist -> tracks to add, the first one in progress
    QString m_query;
    QString m_kind = QStringLiteral("all");
    QString m_started;  // query + kind of the search shown
    QTimer m_debounce;
};

}  // namespace noctara::ui
