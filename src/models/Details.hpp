#pragma once

#include "api/ApiClient.hpp"

#include "models/CollectionModel.hpp"

#include <QDateTime>
#include <QObject>

namespace noctara::models {

// A playlist or album page: its header and its tracks (later tracks arrive as ids and are hydrated as the list
// shows them). With a snapshot store the playlist shows at once from its saved copy; when SoundCloud reports the
// same last_modified the saved tracks stay, otherwise the fresh list replaces them, every track is fetched in the
// background, and the result is saved again.
class PlaylistDetail : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(int errorCode READ errorCode NOTIFY changed)
    Q_PROPERTY(qint64 playlistId READ playlistId CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString owner READ owner NOTIFY changed)
    Q_PROPERTY(qint64 ownerId READ ownerId NOTIFY changed)
    Q_PROPERTY(QString artwork READ artwork NOTIFY changed)
    Q_PROPERTY(bool album READ album NOTIFY changed)
    Q_PROPERTY(QString setType READ setType NOTIFY changed)
    Q_PROPERTY(qint64 trackCount READ trackCount NOTIFY changed)
    Q_PROPERTY(qint64 duration READ duration NOTIFY changed)
    Q_PROPERTY(QVariant likes READ likes NOTIFY changed)
    Q_PROPERTY(QDateTime published READ published NOTIFY changed)
    Q_PROPERTY(QString permalink READ permalink NOTIFY changed)
    Q_PROPERTY(noctara::models::CollectionModel* tracks READ tracks CONSTANT)

public:
    PlaylistDetail(api::IApiClient& client, api::Id id, api::SnapshotStore* snapshots = nullptr,
                   QObject* parent = nullptr);

    Q_INVOKABLE void reload();

    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] int errorCode() const noexcept { return m_error; }
    [[nodiscard]] qint64 playlistId() const noexcept { return m_id; }
    [[nodiscard]] QString title() const { return m_playlist.title; }
    [[nodiscard]] QString owner() const { return m_playlist.user.username; }
    [[nodiscard]] qint64 ownerId() const { return m_playlist.user.id; }
    [[nodiscard]] QString artwork() const;
    [[nodiscard]] bool album() const { return m_playlist.album; }
    [[nodiscard]] QString setType() const { return m_playlist.setType; }
    [[nodiscard]] qint64 trackCount() const { return m_playlist.trackCount; }
    [[nodiscard]] qint64 duration() const { return m_playlist.durationMs; }
    [[nodiscard]] QVariant likes() const;
    [[nodiscard]] QDateTime published() const;
    [[nodiscard]] QString permalink() const { return m_playlist.permalinkUrl; }
    [[nodiscard]] CollectionModel* tracks() noexcept { return &m_tracks; }

Q_SIGNALS:
    void changed();

private:
    void save();
    [[nodiscard]] QString snapshotKey() const;

    api::IApiClient& m_client;
    api::Id m_id;
    api::SnapshotStore* m_snapshots;
    api::Playlist m_playlist;
    CollectionModel m_tracks;
    net::RequestHandle m_request;
    QString m_savedTag;      // last_modified of the snapshot shown, if any
    bool m_fetched = false;  // SoundCloud's answer arrived (a late snapshot is then ignored)
    bool m_loading = false;
    int m_error = -1;
};

// A user page: profile header, their tracks and their playlists.
class UserDetail : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(int errorCode READ errorCode NOTIFY changed)
    Q_PROPERTY(qint64 userId READ userId CONSTANT)
    Q_PROPERTY(QString username READ username NOTIFY changed)
    Q_PROPERTY(QString fullName READ fullName NOTIFY changed)
    Q_PROPERTY(QString avatar READ avatar NOTIFY changed)
    Q_PROPERTY(QString location READ location NOTIFY changed)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QVariant followers READ followers NOTIFY changed)
    Q_PROPERTY(QVariant followings READ followings NOTIFY changed)
    Q_PROPERTY(QVariant trackCount READ trackCount NOTIFY changed)
    Q_PROPERTY(bool verified READ verified NOTIFY changed)
    Q_PROPERTY(QString permalink READ permalink NOTIFY changed)
    Q_PROPERTY(noctara::models::CollectionModel* tracks READ tracks CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* playlists READ playlists CONSTANT)

public:
    UserDetail(api::IApiClient& client, api::Id id, int pageSize, QObject* parent = nullptr);

    Q_INVOKABLE void reload();
    // The playlists list loads on first use.
    Q_INVOKABLE void loadPlaylists();

    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] int errorCode() const noexcept { return m_error; }
    [[nodiscard]] qint64 userId() const noexcept { return m_id; }
    [[nodiscard]] QString username() const { return m_user.username; }
    [[nodiscard]] QString fullName() const { return m_user.fullName; }
    [[nodiscard]] QString avatar() const;
    [[nodiscard]] QString location() const;
    [[nodiscard]] QString description() const { return m_user.description; }
    [[nodiscard]] QVariant followers() const;
    [[nodiscard]] QVariant followings() const;
    [[nodiscard]] QVariant trackCount() const;
    [[nodiscard]] bool verified() const { return m_user.verified; }
    [[nodiscard]] QString permalink() const { return m_user.permalinkUrl; }
    [[nodiscard]] CollectionModel* tracks() noexcept { return &m_tracks; }
    [[nodiscard]] CollectionModel* playlists() noexcept { return &m_playlists; }

Q_SIGNALS:
    void changed();

private:
    api::IApiClient& m_client;
    api::Id m_id;
    int m_pageSize;
    api::User m_user;
    CollectionModel m_tracks;
    CollectionModel m_playlists;
    net::RequestHandle m_request;
    bool m_loading = false;
    bool m_playlistsLoaded = false;
    int m_error = -1;
};

}  // namespace noctara::models
