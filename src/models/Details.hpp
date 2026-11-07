#pragma once

#include "api/ApiClient.hpp"

#include "models/CollectionModel.hpp"
#include "models/CommentModel.hpp"

#include <QDateTime>
#include <QList>
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

// A track page: its header (artwork, artist, counts, tags, description, waveform), the track itself as a one-row
// list (what Play and the queue use), related tracks, the playlists it is in and its comments. The playlists and
// comments load on first use.
class TrackDetail : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(int errorCode READ errorCode NOTIFY changed)
    Q_PROPERTY(qint64 trackId READ trackId CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString artist READ artist NOTIFY changed)
    Q_PROPERTY(qint64 artistId READ artistId NOTIFY changed)
    Q_PROPERTY(bool artistVerified READ artistVerified NOTIFY changed)
    Q_PROPERTY(QString artwork READ artwork NOTIFY changed)
    Q_PROPERTY(QVariant plays READ plays NOTIFY changed)
    Q_PROPERTY(QVariant likes READ likes NOTIFY changed)
    Q_PROPERTY(QVariant reposts READ reposts NOTIFY changed)
    Q_PROPERTY(QVariant comments READ comments NOTIFY changed)
    Q_PROPERTY(QString genre READ genre NOTIFY changed)
    Q_PROPERTY(QStringList tags READ tags NOTIFY changed)
    Q_PROPERTY(QDateTime released READ released NOTIFY changed)
    Q_PROPERTY(qint64 duration READ duration NOTIFY changed)
    Q_PROPERTY(QString description READ description NOTIFY changed)
    Q_PROPERTY(QString album READ album NOTIFY changed)
    Q_PROPERTY(QString permalink READ permalink NOTIFY changed)
    Q_PROPERTY(bool protectedMedia READ protectedMedia NOTIFY changed)
    Q_PROPERTY(bool preview READ preview NOTIFY changed)
    Q_PROPERTY(bool isPrivate READ isPrivate NOTIFY changed)
    Q_PROPERTY(QList<float> waveform READ waveform NOTIFY changed)
    Q_PROPERTY(noctara::models::CollectionModel* self READ self CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* related READ related CONSTANT)
    Q_PROPERTY(noctara::models::CollectionModel* playlists READ playlists CONSTANT)
    Q_PROPERTY(noctara::models::CommentModel* commentList READ commentList CONSTANT)

public:
    TrackDetail(api::IApiClient& client, api::Id id, int pageSize, QObject* parent = nullptr);

    Q_INVOKABLE void reload();
    // These lists load on first use.
    Q_INVOKABLE void loadPlaylists();
    Q_INVOKABLE void loadComments();

    [[nodiscard]] bool loading() const noexcept { return m_loading; }
    [[nodiscard]] int errorCode() const noexcept { return m_error; }
    [[nodiscard]] qint64 trackId() const noexcept { return m_id; }
    [[nodiscard]] QString title() const { return m_track.title; }
    [[nodiscard]] QString artist() const { return m_track.artist(); }
    [[nodiscard]] qint64 artistId() const { return m_track.user.id; }
    [[nodiscard]] bool artistVerified() const { return m_track.user.verified; }
    [[nodiscard]] QString artwork() const;
    [[nodiscard]] QVariant plays() const;
    [[nodiscard]] QVariant likes() const;
    [[nodiscard]] QVariant reposts() const;
    [[nodiscard]] QVariant comments() const;
    [[nodiscard]] QString genre() const { return m_track.genre; }
    [[nodiscard]] QStringList tags() const;
    [[nodiscard]] QDateTime released() const;
    [[nodiscard]] qint64 duration() const
    {
        return m_track.fullDurationMs > 0 ? m_track.fullDurationMs : m_track.durationMs;
    }
    [[nodiscard]] QString description() const { return m_track.description; }
    [[nodiscard]] QString album() const { return m_track.publisherAlbum; }
    [[nodiscard]] QString permalink() const { return m_track.permalinkUrl; }
    [[nodiscard]] bool protectedMedia() const { return m_track.drm(); }
    [[nodiscard]] bool preview() const { return m_track.previewOnly(); }
    [[nodiscard]] bool isPrivate() const { return m_track.isPrivate; }
    [[nodiscard]] const QList<float>& waveform() const noexcept { return m_waveform; }
    [[nodiscard]] CollectionModel* self() noexcept { return &m_self; }
    [[nodiscard]] CollectionModel* related() noexcept { return &m_related; }
    [[nodiscard]] CollectionModel* playlists() noexcept { return &m_playlists; }
    [[nodiscard]] CommentModel* commentList() noexcept { return &m_comments; }
    [[nodiscard]] const api::Track& track() const noexcept { return m_track; }

Q_SIGNALS:
    void changed();

private:
    void loadWaveform();

    api::IApiClient& m_client;
    api::Id m_id;
    int m_pageSize;
    api::Track m_track;
    QList<float> m_waveform;
    CollectionModel m_self;
    CollectionModel m_related;
    CollectionModel m_playlists;
    CommentModel m_comments;
    net::RequestHandle m_request;
    net::RequestHandle m_waveformRequest;
    bool m_loading = false;
    bool m_playlistsLoaded = false;
    bool m_commentsLoaded = false;
    int m_error = -1;
};

}  // namespace noctara::models
