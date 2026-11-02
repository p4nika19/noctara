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
    m_resave.setSingleShot(true);
    m_resave.setInterval(2000);
    connect(&m_resave, &QTimer::timeout, this, [this] {
        if (m_synced) {
            saveSnapshot(true);
        }
    });
}

void CollectionModel::setSource(FirstPage first, int pageSize)
{
    m_first = std::move(first);
    m_pageSize = pageSize;
    m_store = nullptr;
    m_key.clear();
    reload();
}

void CollectionModel::setSyncedSource(FirstPage first, api::SnapshotStore* store, QString key)
{
    m_first = std::move(first);
    m_pageSize = kSyncPageSize;
    m_store = store;
    m_key = std::move(key);
    Q_EMIT sourceReset();
    reload();
}

void CollectionModel::setEntries(std::vector<api::Entry> entries)
{
    m_first = nullptr;
    m_store = nullptr;
    m_key.clear();
    clear();
    append(std::move(entries));
}

bool CollectionModel::complete() const noexcept
{
    if (m_store != nullptr) {
        return m_synced;
    }
    return !m_loading && m_next.isEmpty();
}

void CollectionModel::clear()
{
    Q_EMIT sourceReset();
    m_request.cancel();
    ++m_generation;
    m_next.clear();
    m_requested.clear();
    m_wanted.clear();
    m_coverQueue.clear();
    m_coverAsked.clear();
    m_fresh.clear();
    m_freshSeen.clear();
    m_showingSnapshot = false;
    m_synced = false;
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
        Q_EMIT listChanged();
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
    if (m_store == nullptr) {
        Q_EMIT sourceReset();  // page one replaces every row, later pages included
        load(m_first(m_pageSize), true);
        return;
    }
    // Synced: whatever is shown (the snapshot, or the last sync) stays until the fresh list is complete.
    m_fresh.clear();
    m_freshSeen.clear();
    m_syncPages = 0;
    m_synced = false;
    m_error = -1;
    setLoading(true);
    const int generation = m_generation;
    if (!m_entries.empty()) {
        m_showingSnapshot = true;
        syncPage({}, generation);
        return;
    }
    m_store->load(m_key, this, [this, generation](std::optional<api::Snapshot> snapshot) {
        if (generation != m_generation) {
            return;
        }
        m_showingSnapshot = snapshot && !snapshot->entries.empty();
        if (m_showingSnapshot) {
            applyEntries(std::move(snapshot->entries));
        }
        syncPage({}, generation);
    });
}

void CollectionModel::syncPage(const QString& href, int generation)
{
    const api::Endpoint<api::Page<api::Entry>> first = m_first(kSyncPageSize);
    m_request =
        api::fetch(m_client, href.isEmpty() ? first : api::endpoints::next(first, href), this,
                   [this, generation](Result<api::Page<api::Entry>> page) { onSyncPage(std::move(page), generation); });
}

void CollectionModel::onSyncPage(Result<api::Page<api::Entry>> page, int generation)
{
    if (generation != m_generation) {
        return;
    }
    if (!page) {
        if (page.error().code != ErrorCode::Cancelled) {
            m_error = static_cast<int>(page.error().code);
        }
        if (!m_showingSnapshot && !m_entries.empty()) {
            saveSnapshot(false);  // what arrived so far shows at once next time
        }
        setLoading(false);
        Q_EMIT stateChanged();
        return;
    }
    ++m_syncPages;
    std::vector<api::Entry> added;
    for (api::Entry& entry : page->items) {
        const quint64 key = identity(entry);
        if (!m_freshSeen.contains(key)) {
            m_freshSeen.insert(key);
            added.push_back(entry);
            m_fresh.push_back(std::move(entry));
        }
    }
    if (!m_showingSnapshot) {
        append(std::move(added));
        if (m_syncPages % kSaveEveryPages == 0) {
            saveSnapshot(false);
        }
    } else if (m_syncPages == 1) {
        // What is new at the top (recent likes) shows right away; the rest waits for the complete list.
        std::vector<api::Entry> head;
        for (api::Entry& entry : added) {
            if (m_seen.contains(identity(entry))) {
                break;
            }
            head.push_back(std::move(entry));
        }
        if (!head.empty()) {
            beginInsertRows({}, 0, static_cast<int>(head.size()) - 1);
            for (const api::Entry& entry : head) {
                m_seen.insert(identity(entry));
            }
            m_entries.insert(m_entries.begin(), std::make_move_iterator(head.begin()),
                             std::make_move_iterator(head.end()));
            endInsertRows();
            Q_EMIT countChanged();
            Q_EMIT listChanged();
        }
    }
    if (!page->next.isEmpty()) {
        syncPage(page->next, generation);
        return;
    }
    if (m_showingSnapshot) {
        applyEntries(std::exchange(m_fresh, {}));
    }
    m_fresh.clear();
    m_freshSeen.clear();
    m_showingSnapshot = false;
    m_synced = true;
    saveSnapshot(true);
    setLoading(false);
    Q_EMIT stateChanged();
}

void CollectionModel::saveSnapshot(bool complete)
{
    if (m_store != nullptr) {
        m_store->save(m_key, {.entries = m_entries, .tag = {}, .complete = complete});
    }
}

void CollectionModel::applyEntries(std::vector<api::Entry> fresh)
{
    QSet<quint64> keys;
    std::erase_if(fresh, [&keys](const api::Entry& entry) {
        const quint64 key = identity(entry);
        if (keys.contains(key)) {
            return true;
        }
        keys.insert(key);
        return false;
    });
    // Covers fetched for playlists listed without one carry over while the playlist is unchanged.
    QHash<quint64, const api::Playlist*> oldPlaylists;
    for (const api::Entry& entry : m_entries) {
        if (const auto* playlist = std::get_if<api::Playlist>(&entry.item);
            playlist && !playlist->artworkUrl.isEmpty()) {
            oldPlaylists.insert(identity(entry), playlist);
        }
    }
    for (api::Entry& entry : fresh) {
        if (auto* playlist = std::get_if<api::Playlist>(&entry.item); playlist && playlist->coverUnknown()) {
            if (const api::Playlist* old = oldPlaylists.value(identity(entry));
                old != nullptr && old->lastModified == playlist->lastModified) {
                playlist->artworkUrl = old->artworkUrl;
            }
        }
    }
    const int before = count();
    // 1. Rows that are gone, in contiguous blocks from the bottom.
    for (int row = count() - 1; row >= 0;) {
        if (keys.contains(identity(m_entries[static_cast<std::size_t>(row)]))) {
            --row;
            continue;
        }
        int first = row;
        while (first > 0 && !keys.contains(identity(m_entries[static_cast<std::size_t>(first - 1)]))) {
            --first;
        }
        beginRemoveRows({}, first, row);
        m_entries.erase(m_entries.begin() + first, m_entries.begin() + row + 1);
        endRemoveRows();
        row = first - 1;
    }
    // 2. The rows left must keep their relative order; otherwise (a reordered playlist) start over.
    QHash<quint64, qsizetype> position;
    for (qsizetype i = 0; i < std::ssize(fresh); ++i) {
        position.insert(identity(fresh[static_cast<std::size_t>(i)]), i);
    }
    qsizetype last = -1;
    bool ordered = true;
    for (const api::Entry& entry : m_entries) {
        const qsizetype at = position.value(identity(entry));
        ordered = ordered && at > last;
        last = at;
    }
    if (!ordered) {
        beginResetModel();
        m_entries = std::move(fresh);
        m_seen = std::move(keys);
        endResetModel();
    } else {
        // 3. Walk the fresh list: matching rows take the fresh data, missing ones are inserted in runs.
        std::size_t row = 0;
        for (std::size_t i = 0; i < fresh.size();) {
            if (row < m_entries.size() && identity(m_entries[row]) == identity(fresh[i])) {
                // A stub never replaces a track already known in full (a playlist's later tracks).
                const auto* incoming = std::get_if<api::Track>(&fresh[i].item);
                const auto* known = std::get_if<api::Track>(&m_entries[row].item);
                if (incoming == nullptr || !incoming->stub || known == nullptr || known->stub) {
                    m_entries[row] = std::move(fresh[i]);
                }
                ++row;
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < fresh.size() &&
                   (row >= m_entries.size() || identity(m_entries[row]) != identity(fresh[end]))) {
                ++end;
            }
            const auto first = static_cast<int>(row);
            beginInsertRows({}, first, first + static_cast<int>(end - i) - 1);
            m_entries.insert(m_entries.begin() + static_cast<std::ptrdiff_t>(row),
                             std::make_move_iterator(fresh.begin() + static_cast<std::ptrdiff_t>(i)),
                             std::make_move_iterator(fresh.begin() + static_cast<std::ptrdiff_t>(end)));
            endInsertRows();
            row += end - i;
            i = end;
        }
        m_seen = std::move(keys);
        if (!m_entries.empty()) {
            Q_EMIT dataChanged(index(0), index(count() - 1));
        }
    }
    if (count() != before) {
        Q_EMIT countChanged();
    }
    Q_EMIT listChanged();
}

void CollectionModel::hydrateAll()
{
    for (const api::Entry& entry : m_entries) {
        if (const auto* track = std::get_if<api::Track>(&entry.item);
            track != nullptr && track->stub && !m_requested.contains(track->id)) {
            m_wanted.insert(track->id);
        }
    }
    m_hydrateAll = true;
    if (m_wanted.isEmpty()) {
        finishHydrateAll();
        return;
    }
    hydrate();
}

void CollectionModel::finishHydrateAll()
{
    const bool stubsLeft = std::ranges::any_of(m_entries, [](const api::Entry& entry) {
        const auto* track = std::get_if<api::Track>(&entry.item);
        return track != nullptr && track->stub;
    });
    if (m_hydrateAll && !stubsLeft) {
        m_hydrateAll = false;
        Q_EMIT hydrated();
    }
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
        if ((role == ArtworkRole || role == ThumbnailRole) && playlist->coverUnknown()) {
            requestCover(playlist->id);
        }
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
    // A synced list fetches every page by itself.
    return m_store == nullptr && !parent.isValid() && hasMore() && !m_loading && m_error < 0;
}

void CollectionModel::fetchMore(const QModelIndex& parent)
{
    if (canFetchMore(parent) && m_first && m_store == nullptr) {
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
            if (page->items.empty()) {
                Q_EMIT listChanged();  // otherwise append() reports the replaced rows
            }
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
    Q_EMIT listChanged();
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

void CollectionModel::requestCover(api::Id id) const
{
    if (m_coverAsked.contains(id)) {
        return;
    }
    m_coverAsked.insert(id);
    m_coverQueue.append(id);
    // data() is const; requests start right after.
    QMetaObject::invokeMethod(const_cast<CollectionModel*>(this),  // NOLINT(cppcoreguidelines-pro-type-const-cast)
                              &CollectionModel::fetchCovers, Qt::QueuedConnection);
}

void CollectionModel::fetchCovers()
{
    while (m_coverRequests < kCoverRequests && !m_coverQueue.isEmpty()) {
        const api::Id id = m_coverQueue.takeFirst();
        ++m_coverRequests;
        (void)api::fetch(m_client, api::endpoints::playlist(id), this, [this, id](Result<api::Playlist> playlist) {
            --m_coverRequests;
            const QString cover = playlist ? playlist->artwork() : QString();
            for (int row = 0; row < count() && !cover.isEmpty(); ++row) {
                auto* listed = std::get_if<api::Playlist>(&m_entries[static_cast<std::size_t>(row)].item);
                if (listed != nullptr && listed->id == id) {
                    listed->artworkUrl = cover;  // remembered with the list (and in its snapshot)
                    Q_EMIT dataChanged(index(row), index(row), {ArtworkRole, ThumbnailRole});
                    if (m_store != nullptr && m_synced) {
                        m_resave.start();
                    }
                }
            }
            fetchCovers();
        });
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
        if (!tracks) {
            m_requested.subtract(requested);  // asked again when shown, or by the next hydrateAll()
        }
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
            Q_EMIT listChanged();
        }
    }
    finishHydrateAll();
}

void CollectionModel::setLoading(bool loading)
{
    if (loading != m_loading) {
        m_loading = loading;
        Q_EMIT stateChanged();
    }
}

}  // namespace noctara::models
