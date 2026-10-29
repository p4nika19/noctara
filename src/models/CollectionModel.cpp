#include "models/CollectionModel.hpp"

#include <QMetaObject>

#include <algorithm>
#include <ranges>

namespace noctara::models {

namespace {

// Ids are unique per kind only.
quint64 identity(const api::Entry& entry)
{
    return (static_cast<quint64>(api::itemId(entry.item)) << 2U) | static_cast<quint64>(entry.item.index());
}

QVariant count(std::int64_t value)
{
    return value < 0 ? QVariant() : QVariant::fromValue(value);
}

QString artwork(const QString& url, int pixels)
{
    return api::sizedArtwork(url, pixels);
}

QVariant trackData(const api::Track& track, int role)
{
    using Role = CollectionModel::Role;
    switch (role) {
    case Role::KindRole: return QStringLiteral("track");
    case Role::TitleRole: return track.title;
    case Role::SubtitleRole: return track.artist();
    case Role::ArtworkRole: return artwork(track.artwork(), CollectionModel::kArtworkPixels);
    case Role::ThumbnailRole: return artwork(track.artwork(), CollectionModel::kThumbnailPixels);
    case Role::DurationRole: return QVariant::fromValue(track.fullDurationMs);
    case Role::PlaysRole: return count(track.playbackCount);
    case Role::LikesRole: return count(track.likesCount);
    case Role::RepostsRole: return count(track.repostsCount);
    case Role::GenreRole: return track.genre;
    case Role::PermalinkRole: return track.permalinkUrl;
    case Role::CreatedRole: return track.createdAt;
    case Role::ProtectedRole: return track.drm();
    case Role::PreviewRole: return track.previewOnly();
    case Role::VerifiedRole: return track.user.verified;
    case Role::PendingRole: return track.stub;
    case Role::OwnerIdRole: return QVariant::fromValue(track.user.id);
    default: return {};
    }
}

QVariant playlistData(const api::Playlist& playlist, int role)
{
    using Role = CollectionModel::Role;
    switch (role) {
    case Role::KindRole: return QStringLiteral("playlist");
    case Role::TitleRole: return playlist.title;
    case Role::SubtitleRole: return playlist.user.username;
    case Role::ArtworkRole: return artwork(playlist.artwork(), CollectionModel::kArtworkPixels);
    case Role::ThumbnailRole: return artwork(playlist.artwork(), CollectionModel::kThumbnailPixels);
    case Role::DurationRole: return QVariant::fromValue(playlist.durationMs);
    case Role::LikesRole: return count(playlist.likesCount);
    case Role::RepostsRole: return count(playlist.repostsCount);
    case Role::TrackCountRole: return QVariant::fromValue(playlist.trackCount);
    case Role::GenreRole: return playlist.genre;
    case Role::PermalinkRole: return playlist.permalinkUrl;
    case Role::CreatedRole: return playlist.publishedAt.isValid() ? playlist.publishedAt : playlist.createdAt;
    case Role::VerifiedRole: return playlist.user.verified;
    case Role::AlbumRole: return playlist.album;
    case Role::OwnerIdRole: return QVariant::fromValue(playlist.user.id);
    default: return {};
    }
}

QVariant userData(const api::User& user, int role)
{
    using Role = CollectionModel::Role;
    switch (role) {
    case Role::KindRole: return QStringLiteral("user");
    case Role::TitleRole: return user.username;
    case Role::SubtitleRole: return user.city.isEmpty() ? user.fullName : user.city;
    case Role::ArtworkRole: return artwork(user.avatarUrl, CollectionModel::kArtworkPixels);
    case Role::ThumbnailRole: return artwork(user.avatarUrl, CollectionModel::kThumbnailPixels);
    case Role::FollowersRole: return count(user.followersCount);
    case Role::TrackCountRole: return count(user.trackCount);
    case Role::PermalinkRole: return user.permalinkUrl;
    case Role::VerifiedRole: return user.verified;
    case Role::OwnerIdRole: return QVariant::fromValue(user.id);
    default: return {};
    }
}

}  // namespace

CollectionModel::CollectionModel(api::IApiClient& client, QObject* parent)
    : QAbstractListModel(parent)
    , m_client(client)
{
}

void CollectionModel::setSource(FirstPage first, int pageSize)
{
    m_first = std::move(first);
    m_pageSize = pageSize;
    reload();
}

void CollectionModel::setEntries(std::vector<api::Entry> entries)
{
    m_first = nullptr;
    clear();
    append(std::move(entries));
}

void CollectionModel::clear()
{
    m_request.cancel();
    ++m_generation;
    m_next.clear();
    m_requested.clear();
    m_wanted.clear();
    const bool hadRows = !m_entries.empty();
    beginResetModel();
    m_entries.clear();
    m_seen.clear();
    endResetModel();
    m_error = -1;
    m_stale = false;
    setLoading(false);
    if (hadRows) {
        Q_EMIT countChanged();
    }
}

void CollectionModel::reload()
{
    if (!m_first) {
        return;
    }
    // The old rows stay until the first page arrives, so a refresh does not flash an empty list.
    m_request.cancel();
    ++m_generation;
    load(m_first(m_pageSize), true);
}

int CollectionModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

QVariant CollectionModel::data(const QModelIndex& index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid)) {
        return {};
    }
    const api::Entry& entry = m_entries[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole: return QVariant::fromValue(api::itemId(entry.item));
    case AtRole: return entry.at;
    case RepostedByRole: return entry.repostedBy ? entry.repostedBy->username : QString();
    default: break;
    }
    if (const auto* track = std::get_if<api::Track>(&entry.item)) {
        if (track->stub && role != PendingRole && role != KindRole) {
            requestHydration(track->id);
        }
        return trackData(*track, role);
    }
    if (const auto* playlist = std::get_if<api::Playlist>(&entry.item)) {
        return playlistData(*playlist, role);
    }
    return userData(std::get<api::User>(entry.item), role);
}

QHash<int, QByteArray> CollectionModel::roleNames() const
{
    return {
        {KindRole, "kind"},
        {IdRole, "itemId"},
        {TitleRole, "title"},
        {SubtitleRole, "subtitle"},
        {ArtworkRole, "artwork"},
        {ThumbnailRole, "thumbnail"},
        {DurationRole, "duration"},
        {PlaysRole, "plays"},
        {LikesRole, "likes"},
        {RepostsRole, "reposts"},
        {FollowersRole, "followers"},
        {TrackCountRole, "trackCount"},
        {GenreRole, "genre"},
        {PermalinkRole, "permalink"},
        {CreatedRole, "created"},
        {AtRole, "at"},
        {RepostedByRole, "repostedBy"},
        {ProtectedRole, "protectedMedia"},
        {PreviewRole, "preview"},
        {VerifiedRole, "verified"},
        {AlbumRole, "album"},
        {PendingRole, "pending"},
        {OwnerIdRole, "ownerId"},
    };
}

bool CollectionModel::canFetchMore(const QModelIndex& parent) const
{
    return !parent.isValid() && hasMore() && !m_loading && m_error < 0;
}

void CollectionModel::fetchMore(const QModelIndex& parent)
{
    if (canFetchMore(parent) && m_first) {
        load(api::endpoints::next(m_first(m_pageSize), m_next), false);
    }
}

void CollectionModel::load(const api::Endpoint<api::Page<api::Entry>>& endpoint, bool replace)
{
    m_error = -1;
    setLoading(true);
    const int generation = m_generation;
    m_request = m_client.send(
        endpoint.request, this, [this, parse = endpoint.parse, replace, generation](Result<api::Payload> payload) {
            const bool stale = payload && payload->stale;
            Result<api::Page<api::Entry>> page = payload ? parse(payload->body) : std::unexpected(payload.error());
            // Runs on the model's thread; dropped if the model is gone.
            return std::function<void()>([this, page = std::move(page), replace, generation, stale] mutable {
                if (generation == m_generation) {
                    m_stale = stale || (!replace && m_stale);
                }
                onPage(std::move(page), replace, generation);
            });
        });
}

void CollectionModel::onPage(Result<api::Page<api::Entry>> page, bool replace, int generation)
{
    if (generation != m_generation) {
        return;
    }
    if (!page) {
        if (page.error().code != ErrorCode::Cancelled) {
            m_error = static_cast<int>(page.error().code);
        }
        setLoading(false);
        Q_EMIT stateChanged();
        return;
    }
    if (replace) {
        const bool hadRows = !m_entries.empty();
        beginResetModel();
        m_entries.clear();
        m_seen.clear();
        m_requested.clear();
        endResetModel();
        if (hadRows) {
            Q_EMIT countChanged();
        }
    }
    m_next = page->next;
    append(std::move(page->items));
    setLoading(false);
    Q_EMIT stateChanged();
}

void CollectionModel::append(std::vector<api::Entry> entries)
{
    std::erase_if(entries, [this](const api::Entry& entry) {
        const quint64 key = identity(entry);
        if (m_seen.contains(key)) {
            return true;
        }
        m_seen.insert(key);
        return false;
    });
    if (entries.empty()) {
        return;
    }
    const int first = count();
    beginInsertRows({}, first, first + static_cast<int>(entries.size()) - 1);
    m_entries.insert(m_entries.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    endInsertRows();
    Q_EMIT countChanged();
}

void CollectionModel::requestHydration(api::Id id) const
{
    if (m_requested.contains(id) || m_wanted.contains(id)) {
        return;
    }
    m_wanted.insert(id);
    if (!m_hydrationQueued) {
        m_hydrationQueued = true;
        // data() is const; the request itself changes the model's bookkeeping, so it runs right after.
        QMetaObject::invokeMethod(const_cast<CollectionModel*>(this),  // NOLINT(cppcoreguidelines-pro-type-const-cast)
                                  &CollectionModel::hydrate, Qt::QueuedConnection);
    }
}

void CollectionModel::hydrate()
{
    m_hydrationQueued = false;
    if (m_wanted.isEmpty()) {
        return;
    }
    QSet<api::Id> batch = std::exchange(m_wanted, {});
    m_requested.unite(batch);
    std::vector<api::Id> ids(batch.cbegin(), batch.cend());
    std::ranges::sort(ids);
    const int generation = m_generation;
    m_client.lookupTracks(std::move(ids), this,
                          [this, batch = std::move(batch), generation](Result<std::vector<api::Track>> tracks) {
                              onHydrated(std::move(tracks), batch, generation);
                          });
}

void CollectionModel::onHydrated(Result<std::vector<api::Track>> tracks, const QSet<api::Id>& requested, int generation)
{
    if (generation != m_generation || !tracks) {
        return;  // stubs stay pending; a reload asks again
    }
    QHash<api::Id, api::Track*> byId;
    for (api::Track& track : *tracks) {
        byId.insert(track.id, &track);
    }
    // Requested stubs that came back are replaced; those SoundCloud no longer has (deleted, private) are removed.
    for (int row = count() - 1; row >= 0; --row) {
        api::Entry& entry = m_entries[static_cast<std::size_t>(row)];
        const auto* stub = std::get_if<api::Track>(&entry.item);
        if (stub == nullptr || !stub->stub || !requested.contains(stub->id)) {
            continue;
        }
        if (api::Track* const full = byId.value(stub->id)) {
            entry.item = std::move(*full);
            Q_EMIT dataChanged(index(row), index(row));
        } else {
            beginRemoveRows({}, row, row);
            m_seen.remove(identity(entry));
            m_entries.erase(m_entries.begin() + row);
            endRemoveRows();
            Q_EMIT countChanged();
        }
    }
}

void CollectionModel::setLoading(bool loading)
{
    if (loading != m_loading) {
        m_loading = loading;
        Q_EMIT stateChanged();
    }
}

}  // namespace noctara::models
