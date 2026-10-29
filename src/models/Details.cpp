#include "models/Details.hpp"

#include "api/Endpoints.hpp"

namespace noctara::models {

namespace {

QVariant count(std::int64_t value)
{
    return value < 0 ? QVariant() : QVariant::fromValue<qint64>(value);
}

}  // namespace

PlaylistDetail::PlaylistDetail(api::IApiClient& client, api::Id id, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_id(id)
    , m_tracks(client)
{
    reload();
}

void PlaylistDetail::reload()
{
    m_request.cancel();
    m_loading = true;
    m_error = -1;
    Q_EMIT changed();
    m_request = api::fetch(m_client, api::endpoints::playlist(m_id), this, [this](Result<api::Playlist> playlist) {
        m_loading = false;
        if (!playlist) {
            m_error = static_cast<int>(playlist.error().code);
            Q_EMIT changed();
            return;
        }
        m_playlist = std::move(*playlist);
        std::vector<api::Entry> entries;
        entries.reserve(m_playlist.tracks.size());
        for (const api::Track& track : m_playlist.tracks) {
            entries.push_back({.item = track, .at = {}, .repostedBy = std::nullopt});
        }
        m_tracks.setEntries(std::move(entries));
        Q_EMIT changed();
    });
}

QString PlaylistDetail::artwork() const
{
    const QString url = m_playlist.artwork();
    return url.isEmpty() ? QString() : api::sizedArtwork(url, 500);
}

QVariant PlaylistDetail::likes() const
{
    return count(m_playlist.likesCount);
}

QDateTime PlaylistDetail::published() const
{
    return m_playlist.publishedAt.isValid() ? m_playlist.publishedAt : m_playlist.createdAt;
}

UserDetail::UserDetail(api::IApiClient& client, api::Id id, int pageSize, QObject* parent)
    : QObject(parent)
    , m_client(client)
    , m_id(id)
    , m_pageSize(pageSize)
    , m_tracks(client)
    , m_playlists(client)
{
    reload();
}

void UserDetail::reload()
{
    m_request.cancel();
    m_loading = true;
    m_error = -1;
    Q_EMIT changed();
    m_request = api::fetch(m_client, api::endpoints::user(m_id), this, [this](Result<api::User> user) {
        m_loading = false;
        if (!user) {
            m_error = static_cast<int>(user.error().code);
        } else {
            m_user = std::move(*user);
        }
        Q_EMIT changed();
    });
    m_tracks.setSource([id = m_id](int limit) { return api::endpoints::userTracks(id, limit); }, m_pageSize);
    if (m_playlistsLoaded) {
        m_playlistsLoaded = false;
        loadPlaylists();
    }
}

void UserDetail::loadPlaylists()
{
    if (m_playlistsLoaded) {
        return;
    }
    m_playlistsLoaded = true;
    m_playlists.setSource([id = m_id](int limit) { return api::endpoints::userPlaylists(id, limit); }, m_pageSize);
}

QString UserDetail::avatar() const
{
    return m_user.avatarUrl.isEmpty() ? QString() : api::sizedArtwork(m_user.avatarUrl, 500);
}

QString UserDetail::location() const
{
    return m_user.city;
}

QVariant UserDetail::followers() const
{
    return count(m_user.followersCount);
}

QVariant UserDetail::followings() const
{
    return count(m_user.followingsCount);
}

QVariant UserDetail::trackCount() const
{
    return count(m_user.trackCount);
}

}  // namespace noctara::models
