#include "api/Snapshot.hpp"

#include "api/Parse.hpp"

#include "core/FileIo.hpp"
#include "core/Log.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtConcurrentRun>

#include <condition_variable>

namespace noctara::api {

namespace {

void put(QJsonObject& object, const char* key, const QString& value)
{
    if (!value.isEmpty()) {
        object.insert(QLatin1StringView(key), value);
    }
}

void putCount(QJsonObject& object, const char* key, std::int64_t value)
{
    if (value >= 0) {
        object.insert(QLatin1StringView(key), static_cast<double>(value));
    }
}

void putTime(QJsonObject& object, const char* key, const QDateTime& value)
{
    if (value.isValid()) {
        object.insert(QLatin1StringView(key), value.toUTC().toString(Qt::ISODateWithMs));
    }
}

QJsonObject userRefJson(const UserRef& user)
{
    QJsonObject object{{QStringLiteral("id"), static_cast<double>(user.id)},
                       {QStringLiteral("kind"), QStringLiteral("user")}};
    put(object, "username", user.username);
    put(object, "permalink", user.permalink);
    put(object, "avatar_url", user.avatarUrl);
    object.insert(QStringLiteral("verified"), user.verified);
    return object;
}

QString policyName(Policy policy)
{
    switch (policy) {
    case Policy::Snip: return QStringLiteral("SNIP");
    case Policy::Block: return QStringLiteral("BLOCK");
    case Policy::Monetize: return QStringLiteral("MONETIZE");
    case Policy::Allow: break;
    }
    return QStringLiteral("ALLOW");
}

QJsonObject trackJson(const Track& track)
{
    QJsonObject object{{QStringLiteral("id"), static_cast<double>(track.id)},
                       {QStringLiteral("kind"), QStringLiteral("track")},
                       {QStringLiteral("policy"), policyName(track.policy)}};
    if (track.stub) {
        return object;  // like a playlist's later tracks: no title
    }
    object.insert(QStringLiteral("title"), track.title);
    object.insert(QStringLiteral("user"), userRefJson(track.user));
    put(object, "genre", track.genre);
    put(object, "description", track.description);
    object.insert(QStringLiteral("duration"), static_cast<double>(track.durationMs));
    object.insert(QStringLiteral("full_duration"), static_cast<double>(track.fullDurationMs));
    putTime(object, "created_at", track.createdAt);
    putTime(object, "release_date", track.releasedAt);
    put(object, "last_modified", track.lastModified);
    put(object, "permalink_url", track.permalinkUrl);
    put(object, "artwork_url", track.artworkUrl);
    put(object, "waveform_url", track.waveformUrl);
    putCount(object, "playback_count", track.playbackCount);
    putCount(object, "likes_count", track.likesCount);
    putCount(object, "reposts_count", track.repostsCount);
    putCount(object, "comment_count", track.commentCount);
    object.insert(QStringLiteral("streamable"), track.streamable);
    put(object, "track_authorization", track.trackAuthorization);
    if (!track.publisherArtist.isEmpty() || !track.publisherAlbum.isEmpty()) {
        QJsonObject publisher;
        put(publisher, "artist", track.publisherArtist);
        put(publisher, "album_title", track.publisherAlbum);
        object.insert(QStringLiteral("publisher_metadata"), publisher);
    }
    QJsonArray transcodings;
    for (const Transcoding& t : track.transcodings) {
        QJsonObject entry{{QStringLiteral("url"), t.url},
                          {QStringLiteral("preset"), t.preset},
                          {QStringLiteral("duration"), static_cast<double>(t.durationMs)},
                          {QStringLiteral("snipped"), t.snipped},
                          {QStringLiteral("format"), QJsonObject{{QStringLiteral("protocol"), t.protocolName},
                                                                 {QStringLiteral("mime_type"), t.mimeType}}}};
        put(entry, "quality", t.quality);
        transcodings.append(entry);
    }
    object.insert(QStringLiteral("media"), QJsonObject{{QStringLiteral("transcodings"), transcodings}});
    return object;
}

QJsonObject playlistJson(const Playlist& playlist)
{
    QJsonObject object{{QStringLiteral("id"), static_cast<double>(playlist.id)},
                       {QStringLiteral("kind"), QStringLiteral("playlist")},
                       {QStringLiteral("title"), playlist.title},
                       {QStringLiteral("user"), userRefJson(playlist.user)},
                       {QStringLiteral("is_album"), playlist.album},
                       {QStringLiteral("duration"), static_cast<double>(playlist.durationMs)},
                       {QStringLiteral("track_count"), static_cast<double>(playlist.trackCount)}};
    put(object, "genre", playlist.genre);
    put(object, "set_type", playlist.setType);
    put(object, "permalink_url", playlist.permalinkUrl);
    put(object, "artwork_url", playlist.artworkUrl);
    putCount(object, "likes_count", playlist.likesCount);
    putCount(object, "reposts_count", playlist.repostsCount);
    putTime(object, "created_at", playlist.createdAt);
    putTime(object, "published_at", playlist.publishedAt);
    put(object, "last_modified", playlist.lastModified);
    if (!playlist.tracks.empty()) {
        QJsonArray tracks;
        for (const Track& track : playlist.tracks) {
            tracks.append(trackJson(track));
        }
        object.insert(QStringLiteral("tracks"), tracks);
    }
    return object;
}

QJsonObject userJson(const User& user)
{
    QJsonObject object = userRefJson(user.ref());
    put(object, "full_name", user.fullName);
    put(object, "permalink_url", user.permalinkUrl);
    put(object, "city", user.city);
    put(object, "country_code", user.countryCode);
    put(object, "description", user.description);
    putCount(object, "followers_count", user.followersCount);
    putCount(object, "followings_count", user.followingsCount);
    putCount(object, "track_count", user.trackCount);
    putCount(object, "playlist_count", user.playlistCount);
    putCount(object, "likes_count", user.likesCount);
    return object;
}

// {"track": …, "created_at": …}, plus "type": "track-repost" and the reposter for feed reposts.
QJsonObject entryJson(const Entry& entry)
{
    QJsonObject object;
    const char* kind = "track";
    if (const auto* track = std::get_if<Track>(&entry.item)) {
        object.insert(QStringLiteral("track"), trackJson(*track));
    } else if (const auto* playlist = std::get_if<Playlist>(&entry.item)) {
        object.insert(QStringLiteral("playlist"), playlistJson(*playlist));
        kind = "playlist";
    } else {
        object.insert(QStringLiteral("user"), userJson(std::get<User>(entry.item)));
        kind = "user";
    }
    putTime(object, "created_at", entry.at);
    if (entry.repostedBy) {
        object.insert(QStringLiteral("type"), QStringLiteral("%1-repost").arg(QLatin1StringView(kind)));
        object.insert(QStringLiteral("user"), userRefJson(*entry.repostedBy));
    }
    return object;
}

void markStale(Track& track)
{
    track.stale = !track.stub;
}

}  // namespace

QByteArray encodeSnapshot(const Snapshot& snapshot)
{
    QJsonArray collection;
    for (const Entry& entry : snapshot.entries) {
        collection.append(entryJson(entry));
    }
    QJsonObject root{{QStringLiteral("collection"), collection},
                     {QStringLiteral("noctara_complete"), snapshot.complete}};
    put(root, "noctara_tag", snapshot.tag);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

Result<Snapshot> decodeSnapshot(const QByteArray& json)
{
    Result<parse::SnapshotFields> fields = parse::snapshot(json);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    Snapshot snapshot{.entries = std::move(fields->page.items), .tag = fields->tag, .complete = fields->complete};
    for (Entry& entry : snapshot.entries) {
        if (auto* track = std::get_if<Track>(&entry.item)) {
            markStale(*track);
        } else if (auto* playlist = std::get_if<Playlist>(&entry.item)) {
            for (Track& listed : playlist->tracks) {
                markStale(listed);
            }
        }
    }
    return snapshot;
}

// Writes run one at a time; `pending` lets flush() wait for them.
struct SnapshotStore::Shared {
    std::mutex mutex;
    std::condition_variable idle;
    int pending = 0;
};

SnapshotStore::SnapshotStore(QString root)
    : m_root(std::move(root))
    , m_shared(std::make_shared<Shared>())
{
    QFile remembered(m_root + QStringLiteral("/account"));
    if (!m_root.isEmpty() && remembered.open(QIODevice::ReadOnly)) {
        m_lastAccount.store(remembered.read(32).trimmed().toLongLong());  // a few bytes, once at start
    }
}

void SnapshotStore::setAccount(Id account)
{
    m_account.store(account);
    if (account <= 0 || account == m_lastAccount.load() || m_root.isEmpty()) {
        return;
    }
    m_lastAccount.store(account);
    {
        const std::scoped_lock lock(m_shared->mutex);
        ++m_shared->pending;
    }
    // Writes whatever is current when it runs, under the store's lock: quick changes never land out of order.
    (void)QtConcurrent::run([this, shared = m_shared] {
        const std::scoped_lock lock(shared->mutex);
        const Id current = m_lastAccount.load();
        const QString path = m_root + QStringLiteral("/account");
        if (current > 0 && ensureDirectory(m_root) &&
            !writeFileAtomic(path, QByteArray::number(current), 0600, Durability::Atomic)) {
            qCWarning(lcApi).noquote() << "cannot remember the account of the saved lists";
        }
        --shared->pending;
        shared->idle.notify_all();
    });
}

SnapshotStore::~SnapshotStore()
{
    flush();
}

QString SnapshotStore::pathFor(Id account, const QString& key) const
{
    static const QRegularExpression unsafe(QStringLiteral("[^a-z0-9-]"));
    QString name = key.toLower();
    name.replace(unsafe, QStringLiteral("-"));
    return QStringLiteral("%1/%2/%3.json").arg(m_root).arg(account).arg(name);
}

void SnapshotStore::load(const QString& key, QObject* context, std::function<void(std::optional<Snapshot>)> done) const
{
    const Id account = m_account.load();
    if (account == 0 || m_root.isEmpty()) {
        QMetaObject::invokeMethod(context, [done = std::move(done)] { done(std::nullopt); }, Qt::QueuedConnection);
        return;
    }
    (void)QtConcurrent::run([path = pathFor(account, key), shared = m_shared] -> std::optional<Snapshot> {
        QByteArray data;
        {
            const std::scoped_lock lock(shared->mutex);  // never half a file being replaced
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                return std::nullopt;
            }
            data = file.readAll();
        }
        Result<Snapshot> snapshot = decodeSnapshot(data);
        if (!snapshot) {
            qCWarning(lcApi).noquote() << "ignoring a damaged saved list" << path << ":"
                                       << QString::fromStdString(snapshot.error().describe());
            return std::nullopt;
        }
        return std::move(*snapshot);
    }).then(context, [this, account, done = std::move(done)](std::optional<Snapshot> snapshot) {
        // Signed out or switched accounts meanwhile: not this account's list any more.
        done(account == m_account.load() ? std::move(snapshot) : std::nullopt);
    });
}

void SnapshotStore::save(const QString& key, Snapshot snapshot)
{
    const Id account = m_account.load();
    if (account == 0 || m_root.isEmpty()) {
        return;
    }
    {
        const std::scoped_lock lock(m_shared->mutex);
        ++m_shared->pending;
    }
    (void)QtConcurrent::run([path = pathFor(account, key), shared = m_shared, snapshot = std::move(snapshot)] {
        const QByteArray json = encodeSnapshot(snapshot);
        const std::scoped_lock lock(shared->mutex);
        if (Result<void> made = ensureDirectory(QFileInfo(path).path()); !made) {
            qCWarning(lcApi).noquote() << "cannot save lists:" << QString::fromStdString(made.error().describe());
        } else if (Result<void> written = writeFileAtomic(path, json, 0600, Durability::Atomic); !written) {
            qCWarning(lcApi).noquote() << "cannot save" << path << ":"
                                       << QString::fromStdString(written.error().describe());
        }
        --shared->pending;
        shared->idle.notify_all();
    });
}

void SnapshotStore::forget(Id account)
{
    if (m_root.isEmpty()) {
        return;
    }
    flush();
    const std::scoped_lock lock(m_shared->mutex);
    (void)QDir(QStringLiteral("%1/%2").arg(m_root).arg(account)).removeRecursively();
    if (account == m_lastAccount.load()) {
        m_lastAccount.store(0);
        (void)QFile::remove(m_root + QStringLiteral("/account"));
    }
}

void SnapshotStore::clear()
{
    if (m_root.isEmpty()) {
        return;  // QDir("") would be the working directory
    }
    flush();
    const std::scoped_lock lock(m_shared->mutex);
    (void)QDir(m_root).removeRecursively();
    m_lastAccount.store(0);
}

void SnapshotStore::flush()
{
    std::unique_lock lock(m_shared->mutex);
    m_shared->idle.wait(lock, [this] { return m_shared->pending == 0; });
}

}  // namespace noctara::api
