#include "cache/AudioCache.hpp"

#include "cache/AudioPayload.hpp"
#include "cache/DiskLru.hpp"
#include "cache/FileOps.hpp"
#include "cache/Hash.hpp"
#include "cache/Tagger.hpp"
#include "core/FileIo.hpp"
#include "core/Log.hpp"
#include "core/UniqueFd.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QtConcurrentRun>

#include <cerrno>
#include <fcntl.h>
#include <format>

namespace noctara::cache {

namespace {

constexpr int kLookaheadPieces = 16;  // a reader this far ahead of the running fetch waits for it
constexpr int kHlsLookahead = 2;
constexpr int kMaxRetries = 3;
constexpr std::int64_t kMiB = 1024LL * 1024;
constexpr std::int64_t kSpaceCheckInterval = 4 * kMiB;
constexpr std::int64_t kFloorSlack = 64 * kMiB;
constexpr qsizetype kRetiredGenerations = 16;
constexpr QByteArrayView kIndexHeader = "noctara-audio-index 1\n";
constexpr std::chrono::milliseconds kWaitSlice{50};

// How far to fill an entry: everything for playback readers, the largest prefetch limit otherwise.
std::int64_t fillLimit(const QHash<std::uint64_t, std::int64_t>& readers)
{
    std::int64_t limit = 0;
    for (const std::int64_t value : readers) {
        limit = value < 0 || limit < 0 ? -1 : std::max(limit, value);
    }
    return limit;
}

QString path(const QString& dir, const char* file)
{
    return dir + u'/' + QLatin1StringView(file);
}

QByteArray orDash(const QByteArray& value)
{
    return value.isEmpty() ? QByteArray("-") : value;
}

QByteArray undash(const QByteArray& value)
{
    return value == "-" ? QByteArray() : value;
}

// Statuses a signed media URL answers once it expired: worth asking the UrlProvider for a fresh one.
bool urlExpired(int status)
{
    return status == 401 || status == 403 || status == 404 || status == 410;
}

QString tagSignature(const TrackInfo& info)
{
    return info.sourceUpdatedAt + u'|' + QString::fromLatin1(info.coverHash);
}

std::optional<std::int64_t> contentRangeTotal(const QByteArray& header, std::int64_t& first)
{
    // "bytes 100-199/1000" or "bytes 100-199/*"
    const qsizetype space = header.indexOf(' ');
    const qsizetype dash = header.indexOf('-', space);
    const qsizetype slash = header.indexOf('/', dash);
    if (space < 0 || dash < 0 || slash < 0) {
        return std::nullopt;
    }
    first = header.sliced(space + 1, dash - space - 1).trimmed().toLongLong();
    bool ok = false;
    const std::int64_t total = header.sliced(slash + 1).trimmed().toLongLong(&ok);
    return ok ? std::optional(total) : std::optional<std::int64_t>(-1);
}

// Rebuilds the piece map of a finished progressive file (after tagging changed its layout).
Result<PieceMap> mapOfFile(const QString& file)
{
    const UniqueFd fd(::open(QFile::encodeName(file).constData(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return fail(errorCodeFromErrno(errno), "cannot open the tagged file");
    }
    const std::int64_t size = QFileInfo(file).size();
    PieceMap map = PieceMap::progressive(size);
    for (int piece = 0; piece < map.pieceCount(); ++piece) {
        Result<QByteArray> data = readAt(fd.get(), map.pieceOffset(piece), map.pieceSize(piece));
        if (!data || data->size() != map.pieceSize(piece)) {
            return fail(ErrorCode::IoError, "short read while indexing the tagged file");
        }
        map.mark(piece, static_cast<std::uint32_t>(data->size()), checksum(*data));
    }
    return map;
}

}  // namespace

// One generation of an entry's stored layout. Guarded by EntryState::mutex.
struct Generation {
    std::uint32_t id = 0;
    PieceMap map;
    std::shared_ptr<UniqueFd> fd;  // progressive: the data file for reading (null: not created or RAM only)
    bool reset = false;            // the source changed: readers bound to this generation must reopen
};

struct EntryState {
    TrackKey key;
    QByteArray name;
    std::uint64_t id = 0;
    QString dir;  // empty: RAM only. Cleared under `mutex` when the cache moves; readers copy it under the lock

    mutable std::mutex mutex;
    std::condition_variable changed;
    TrackInfo info;
    std::shared_ptr<Generation> current;
    std::optional<HlsPlaylist> playlist;
    std::optional<Error> failure;  // why missing pieces cannot be fetched right now
    std::uint64_t failureSeq = 0;  // bumped per failure; each reader reports a failure once, then retries
};

struct AudioCache::Fetch {
    std::uint64_t id = 0;
    net::RequestHandle handle;
    int start = 0;
    int next = 0;
    int end = -1;           // exclusive; -1 = to the end of the stream
    std::int64_t skip = 0;  // leading bytes to drop (the server ignored the range)
    QByteArray buffer;
};

struct AudioCache::Live {
    std::shared_ptr<EntryState> entry;
    UrlProvider urls;
    QUrl url;
    bool urlRefreshed = false;
    bool resolving = false;
    bool refreshPending = false;
    std::vector<std::function<void(Live&)>> afterUrl;
    std::optional<HlsPlaylist> sourcePlaylist;  // with the server's (signed) segment URLs
    std::optional<Fetch> fetch;
    int resumeAt = 0;
    int retries = 0;
    QHash<std::uint64_t, std::int64_t> readers;  // reader -> background fill limit in bytes (-1 = all)
    QSet<std::uint64_t> foreground;
    QSet<int> volatilePieces;  // held in RAM only (disk full): never persisted as stored
    UniqueFd writeFd;          // progressive data.part
    int coverFetches = 0;
    bool tagging = false;
    QString tagAttemptFor;  // tag signature of the running (or last) tagging pass
    QString tagFailedFor;   // signature whose pass failed: not retried until the metadata changes
    bool resetAfterTagging = false;
    bool doomed = false;  // cleared while in use: files go when the last reader closes
};

// --- AudioReader ---------------------------------------------------------------------------------

AudioReader::AudioReader(std::shared_ptr<EntryState> entry, std::shared_ptr<Generation> generation, MemoryTier& memory,
                         AudioCache* cache, MailboxPtr mailbox, std::uint64_t id)
    : m_entry(std::move(entry))
    , m_generation(std::move(generation))
    , m_memory(memory)
    , m_cache(cache)
    , m_mailbox(std::move(mailbox))
    , m_id(id)
{
}

AudioReader::~AudioReader()
{
    m_mailbox->post([cache = m_cache, name = m_entry->name, id = m_id] { cache->readerClosed(name, id); });
}

TrackInfo AudioReader::info() const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_entry->info;
}

StreamProtocol AudioReader::protocol() const
{
    return info().protocol;
}

std::int64_t AudioReader::contentLength() const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_generation->map.contentLength();
}

int AudioReader::pieceCount() const
{
    const std::scoped_lock lock(m_entry->mutex);
    if (m_generation->map.layout() == PieceMap::Layout::Hls && !m_entry->playlist) {
        return -1;
    }
    return m_generation->map.pieceCount();
}

std::int64_t AudioReader::pieceOffset(int piece) const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_generation->map.pieceOffset(piece);
}

int AudioReader::pieceAt(std::int64_t offset) const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_generation->map.pieceAt(offset);
}

std::optional<HlsPlaylist> AudioReader::playlist() const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_entry->playlist;
}

bool AudioReader::complete() const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_generation->map.complete();
}

std::vector<std::pair<std::int64_t, std::int64_t>> AudioReader::cachedRanges() const
{
    const std::scoped_lock lock(m_entry->mutex);
    return m_generation->map.ranges();
}

std::vector<std::pair<double, double>> AudioReader::cachedFractions() const
{
    std::vector<std::pair<double, double>> out;
    const auto add = [&out](double from, double to) {
        if (!out.empty() && from <= out.back().second + 1e-9) {
            out.back().second = std::max(out.back().second, to);
        } else if (to > from) {
            out.emplace_back(from, to);
        }
    };
    const std::scoped_lock lock(m_entry->mutex);
    const PieceMap& map = m_generation->map;
    if (map.layout() == PieceMap::Layout::Progressive) {
        const auto length = static_cast<double>(map.contentLength());
        if (length <= 0) {
            return out;
        }
        for (const auto& [first, last] : map.ranges()) {
            add(static_cast<double>(first) / length, static_cast<double>(last) / length);
        }
        return out;
    }
    if (!m_entry->playlist || m_entry->playlist->totalDuration() <= 0) {
        return out;
    }
    const HlsPlaylist& playlist = *m_entry->playlist;
    const double total = playlist.totalDuration();
    const int first = playlist.init ? 1 : 0;
    double start = 0;
    for (std::size_t i = 0; i < playlist.segments.size(); ++i) {
        const double end = start + playlist.segments[i].duration;
        if (map.has(first + static_cast<int>(i))) {
            add(start / total, end / total);
        }
        start = end;
    }
    return out;
}

void AudioReader::request(int piece)
{
    if (m_requested.exchange(piece) == piece) {
        return;
    }
    m_mailbox->post([cache = m_cache, name = m_entry->name, piece] { cache->want(name, piece); });
}

AudioReader::Piece AudioReader::tryRead(int piece)
{
    const PieceKey key{m_entry->id, m_generation->id, static_cast<std::uint32_t>(piece)};
    if (std::optional<QByteArray> hit = m_memory.find(key)) {
        return {Status::Ready, std::move(*hit), {}};
    }
    std::shared_ptr<UniqueFd> fd;
    std::int64_t offset = 0;
    std::int64_t size = 0;
    std::uint64_t sum = 0;
    bool present = false;
    QString dir;
    const bool hls = info().protocol == StreamProtocol::Hls;
    {
        const std::scoped_lock lock(m_entry->mutex);
        if (m_generation->reset) {
            return {Status::Failed, {}, Error{ErrorCode::ContentChanged, "the source changed"}};
        }
        const PieceMap& map = m_generation->map;
        // An HLS entry knows its pieces only once the playlist is in.
        const bool countKnown = map.layout() == PieceMap::Layout::Progressive || m_entry->playlist.has_value();
        if (countKnown && map.pieceCount() >= 0 && piece >= map.pieceCount()) {
            return {Status::Failed, {}, Error{ErrorCode::InvalidArgument, "piece past the end"}};
        }
        present = map.has(piece);
        if (present) {
            offset = map.layout() == PieceMap::Layout::Progressive ? map.pieceOffset(piece) : 0;
            size = map.pieceSize(piece);
            sum = map.checksum(piece);
            fd = m_generation->fd;
            dir = m_entry->dir;
        } else if (m_entry->failure && m_generation == m_entry->current &&
                   m_entry->failureSeq != m_seenFailure.load()) {
            m_seenFailure.store(m_entry->failureSeq);
            m_requested.store(-1);
            return {Status::Failed, {}, *m_entry->failure};
        }
    }
    if (present) {
        Result<QByteArray> data = [&] -> Result<QByteArray> {
            if (hls) {
                if (dir.isEmpty()) {
                    return fail(ErrorCode::NotFound, "RAM-only piece evicted");
                }
                return readWholeFile(dir + QStringLiteral("/seg/") + QString::fromLatin1(hlsPieceName(piece)));
            }
            if (!fd || !fd->valid()) {
                return fail(ErrorCode::NotFound, "RAM-only piece evicted");
            }
            return readAt(fd->get(), offset, size);
        }();
        if (data && data->size() == size && checksum(*data) == sum) {
            m_memory.insert(key, *data);
            return {Status::Ready, std::move(*data), {}};
        }
        // Evicted from RAM (RAM-only entries), truncated or corrupt on disk: drop the piece and fetch it again.
        const std::scoped_lock lock(m_entry->mutex);
        if (m_generation->map.checksum(piece) == sum) {
            m_generation->map.unmark(piece);
        }
        if (m_generation != m_entry->current) {
            return {Status::Failed, {}, Error{ErrorCode::ContentChanged, "the stored file changed"}};
        }
    }
    request(piece);
    return {Status::Pending, {}, {}};
}

Result<QByteArray> AudioReader::read(int piece, const std::stop_token& stop, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        Piece result = tryRead(piece);
        if (result.status == Status::Ready) {
            return std::move(result.data);
        }
        if (result.status == Status::Failed) {
            return std::unexpected(std::move(result.error));
        }
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "read cancelled");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            m_requested.store(-1);
            return fail(ErrorCode::Timeout, "piece did not arrive in time");
        }
        std::unique_lock lock(m_entry->mutex);
        if (!m_generation->map.has(piece) && m_entry->failureSeq == m_seenFailure.load() && !m_generation->reset) {
            m_entry->changed.wait_for(lock, kWaitSlice);
        }
    }
}

Result<QByteArray> AudioReader::readBytes(std::int64_t offset, std::int64_t size, const std::stop_token& stop,
                                          std::chrono::milliseconds timeout)
{
    QByteArray out;
    out.reserve(static_cast<qsizetype>(size));
    std::int64_t position = offset;
    while (position < offset + size) {
        const int piece = pieceAt(position);
        Result<QByteArray> data = read(piece, stop, timeout);
        if (!data) {
            if (data.error().code == ErrorCode::InvalidArgument) {
                break;  // past the end
            }
            return data;
        }
        const std::int64_t within = position - pieceOffset(piece);
        if (within >= data->size()) {
            break;
        }
        const std::int64_t take = std::min<std::int64_t>(data->size() - within, offset + size - position);
        out.append(QByteArrayView(*data).sliced(static_cast<qsizetype>(within), static_cast<qsizetype>(take)));
        position += take;
    }
    return out;
}

void AudioReader::prefetch(std::int64_t maxBytes)
{
    m_mailbox->post(
        [cache = m_cache, name = m_entry->name, id = m_id, maxBytes] { cache->setPrefetch(name, id, maxBytes); });
}

// --- AudioCache: lifetime, configuration, index --------------------------------------------------

AudioCache::AudioCache(net::INetworkTransport& transport, MemoryTier& memory, FileOps& ops, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_memory(memory)
    , m_ops(ops)
    , m_mailbox(std::make_shared<Mailbox>(this))
{
    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(1000);
    connect(&m_flushTimer, &QTimer::timeout, this, &AudioCache::flush);
}

AudioCache::~AudioCache()
{
    m_mailbox->close();
    for (auto it = m_live.cbegin(); it != m_live.cend(); ++it) {
        cancelFetch(**it);
        if ((*it)->doomed) {
            removeEntryFiles(it.key());  // cleared while in use: it never gets to retire now
        }
    }
    flush();
}

void AudioCache::setLayout(std::optional<CacheLayout> layout)
{
    flush();
    // Entries still being read go on in RAM only: their readers need further fetches, and the old location
    // is no longer ours to write to. Pieces already stored there stay readable through the open file.
    for (auto it = m_live.begin(); it != m_live.end();) {
        Live& l = **it;
        if (l.readers.isEmpty()) {
            cancelFetch(l);
            it = m_live.erase(it);
            continue;
        }
        l.writeFd.reset();
        l.volatilePieces.clear();
        l.doomed = false;
        {
            const std::scoped_lock lock(l.entry->mutex);
            l.entry->dir.clear();
        }
        ++it;
    }
    m_ledger.clear();
    m_entryCovers.clear();
    m_entryComplete.clear();
    m_retired.clear();
    m_audioDir = layout ? layout->audio() : QString();
    m_coversDir = layout ? layout->covers() : QString();
    m_layout = std::move(layout);
    m_indexLoaded = false;
}

void AudioCache::setConfig(const AudioCacheConfig& config)
{
    m_config = config;
    if (m_indexLoaded && m_config.diskEnabled) {
        evictFor(0, {});
    }
}

QString AudioCache::entryDir(const QByteArray& name) const
{
    return m_audioDir + u'/' + QString::fromLatin1(name.first(2)) + u'/' + QString::fromLatin1(name);
}

AudioCache::Live* AudioCache::live(const QByteArray& name)
{
    const auto it = m_live.find(name);
    return it == m_live.end() ? nullptr : it->get();
}

void AudioCache::loadIndex()
{
    if (m_indexLoaded || !m_layout) {
        return;
    }
    m_indexLoaded = true;
    const auto record = [this](const QByteArray& name, std::int64_t bytes, std::int64_t access, bool complete,
                               const QByteArray& cover, const QByteArray& thumb, const QUrl& coverUrl,
                               const QUrl& thumbUrl) {
        if (!m_ledger.contains(name)) {  // entries opened meanwhile are already accounted for
            m_ledger.upsert(name, bytes, access);
            m_entryComplete.insert(name, complete);
            m_entryCovers.insert(name, Covers{cover, thumb, coverUrl, thumbUrl});
        }
    };
    const Result<QByteArray> index = readWholeFile(path(m_layout->audio(), "index"));
    if (index && index->startsWith(kIndexHeader)) {
        for (const QByteArray& line : index->sliced(kIndexHeader.size()).split('\n')) {
            const QList<QByteArray> f = line.split(' ');
            if (f.size() == 8 && f[0].size() == 32) {
                record(f[0], f[1].toLongLong(), f[2].toLongLong(), f[3] == "1", undash(f[4]), undash(f[5]),
                       QUrl::fromEncoded(undash(f[6])), QUrl::fromEncoded(undash(f[7])));
            }
        }
    } else {
        // Rebuild from the entries themselves; anything without a valid sidecar is not ours to keep.
        for (const QString& bucket : QDir(m_layout->audio()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (bucket.size() != 2) {
                continue;
            }
            const QString bucketDir = m_layout->audio() + u'/' + bucket;
            for (const QString& entry : QDir(bucketDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString dir = bucketDir + u'/' + entry;
                const Result<QByteArray> json = readWholeFile(path(dir, "meta.json"));
                const Result<TrackInfo> info =
                    json ? TrackInfo::fromJson(*json) : Result<TrackInfo>(std::unexpected(json.error()));
                if (!info || info->key.name() != entry.toLatin1()) {
                    removeTree(dir);
                    continue;
                }
                record(entry.toLatin1(), directorySize(dir),
                       QFileInfo(path(dir, "meta.json")).lastModified().toMSecsSinceEpoch(), info->complete,
                       info->coverHash, info->thumbHash, info->artworkUrl, info->artworkThumbUrl);
            }
        }
        scheduleFlush({});
    }
    collectCovers();
    if (m_config.diskEnabled) {
        evictFor(0, {});
    }
    Q_EMIT usageChanged();
}

void AudioCache::scheduleFlush(const QByteArray& name)
{
    if (!name.isEmpty() && !m_dirty.contains(name)) {
        m_dirty.append(name);
    }
    m_indexDirty = true;
    if (!m_flushTimer.isActive()) {
        m_flushTimer.start();
    }
}

void AudioCache::flush()
{
    for (const QByteArray& name : std::exchange(m_dirty, {})) {
        if (const Live* l = live(name)) {
            persistMap(*l->entry);
        }
    }
    if (!m_layout || !m_indexLoaded || !m_indexDirty) {
        return;
    }
    m_indexDirty = false;
    QByteArray index = kIndexHeader.toByteArray();
    for (auto it = m_ledger.items().cbegin(); it != m_ledger.items().cend(); ++it) {
        const Covers covers = m_entryCovers.value(it.key());
        index += it.key() + ' ' + QByteArray::number(it->bytes) + ' ' + QByteArray::number(it->lastAccess) + ' ' +
                 (m_entryComplete.value(it.key()) ? "1 " : "0 ") + orDash(covers.cover) + ' ' + orDash(covers.thumb) +
                 ' ' + orDash(covers.coverUrl.toEncoded()) + ' ' + orDash(covers.thumbUrl.toEncoded()) + '\n';
    }
    if (auto written = m_ops.writeFile(path(m_layout->audio(), "index"), index); !written) {
        qCDebug(lcCache) << "audio index not written:" << written.error().describe();
    }
}

// --- Opening entries ---------------------------------------------------------------------------------

void AudioCache::open(const TrackInfo& info, UrlProvider urls, QObject* context, OpenCallback done, bool background)
{
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto channel = std::make_shared<net::ChannelPtr>(net::makeChannel(context, cancelled, thread()));
    const auto callback = std::make_shared<OpenCallback>(std::move(done));
    m_mailbox->post([this, info, urls = std::move(urls), channel, callback, background] {
        openOnThread(info, urls, channel, callback, background);
    });
}

void AudioCache::openOnThread(const TrackInfo& info, const UrlProvider& urls,
                              const std::shared_ptr<net::ChannelPtr>& channel,
                              const std::shared_ptr<OpenCallback>& done, bool background)
{
    const auto deliver = [&](Result<AudioReaderPtr> result) {
        (*channel)->post([done, result = std::move(result)] { (*done)(result); });
        channel->reset();  // deleted here, on the cache thread
    };
    // DRM policy: protected media never reaches any tier, not even as a sidecar.
    if (info.drm) {
        deliver(fail(ErrorCode::NotCacheable, "DRM-protected track"));
        return;
    }
    if (info.key.trackId.isEmpty()) {
        deliver(fail(ErrorCode::InvalidArgument, "track without an id"));
        return;
    }
    const QByteArray name = info.key.name();
    Live* l = live(name);
    if (l == nullptr) {
        Result<std::shared_ptr<EntryState>> entry = createEntry(info);
        if (!entry) {
            deliver(std::unexpected(entry.error()));
            return;
        }
        const auto created = std::make_shared<Live>();
        created->entry = std::move(*entry);
        l = m_live.insert(name, created)->get();
    } else {
        EntryState& entry = *l->entry;
        bool changed = false;
        bool mediaChanged = false;
        {
            const std::scoped_lock lock(entry.mutex);
            mediaChanged =
                !info.mediaEtag.isEmpty() && !entry.info.mediaEtag.isEmpty() && info.mediaEtag != entry.info.mediaEtag;
            if (!entry.info.sameMetadata(info)) {
                entry.info.refreshFrom(info);
                changed = true;
            }
        }
        if (mediaChanged) {
            if (l->tagging) {
                l->resetAfterTagging = true;
            } else {
                resetStored(entry, "media changed");
                const std::scoped_lock lock(entry.mutex);
                entry.info.mediaEtag = info.mediaEtag;
            }
        } else if (changed) {
            writeSidecar(entry);
        }
    }
    l->urls = urls;
    const std::uint64_t readerId = m_nextReader++;
    std::shared_ptr<Generation> generation;
    {
        const std::scoped_lock lock(l->entry->mutex);
        generation = l->entry->current;
    }
    const auto reader = std::make_shared<AudioReader>(l->entry, generation, m_memory, this, m_mailbox, readerId);
    readerOpened(*l, readerId, background);
    deliver(reader);
}

Result<std::shared_ptr<EntryState>> AudioCache::createEntry(const TrackInfo& info)
{
    auto entry = std::make_shared<EntryState>();
    entry->key = info.key;
    entry->name = info.key.name();
    entry->id = info.key.id();
    if (const auto retired = m_retired.find(entry->name); retired != m_retired.end()) {
        // Reopened soon after closing: keep the generation, so the RAM tier's pieces still match.
        entry->current = retired->generation;
        entry->dir = retired->dir;
        entry->info = retired->info;
        entry->playlist = retired->playlist;
        entry->info.refreshFrom(info);
        m_retired.erase(retired);
        if (!entry->dir.isEmpty()) {
            writeSidecar(*entry);
        }
        return entry;
    }
    if (m_layout && m_config.diskEnabled) {
        entry->dir = entryDir(entry->name);
        if (auto stored = loadStored(*entry, info); !stored) {
            qCInfo(lcCache) << "audio entry kept in RAM only:" << stored.error().describe();
            entry->dir.clear();
        } else {
            return entry;
        }
    }
    entry->info = info;
    entry->info.cachedAt = QDateTime::currentDateTimeUtc();
    auto generation = std::make_shared<Generation>();
    generation->id = m_nextGeneration++;
    generation->map = info.protocol == StreamProtocol::Hls ? PieceMap::hls(0) : PieceMap::progressive(-1);
    entry->current = std::move(generation);
    return entry;
}

Result<void> AudioCache::loadStored(EntryState& entry, const TrackInfo& fresh)
{
    const QString& dir = entry.dir;
    TrackInfo stored;
    bool reuse = false;
    if (const Result<QByteArray> json = readWholeFile(path(dir, "meta.json")); json) {
        if (Result<TrackInfo> parsed = TrackInfo::fromJson(*json); parsed && parsed->key == fresh.key) {
            stored = std::move(*parsed);
            const bool mediaChanged =
                !fresh.mediaEtag.isEmpty() && !stored.mediaEtag.isEmpty() && fresh.mediaEtag != stored.mediaEtag;
            reuse = !mediaChanged && stored.protocol == fresh.protocol;
        }
    }
    if (!reuse) {
        // Unknown, corrupt, anonymous or outdated media: start over.
        removeTree(dir);
        m_ledger.remove(entry.name);
        stored = fresh;
        stored.cachedAt = QDateTime::currentDateTimeUtc();
        stored.contentLength = -1;
        stored.complete = false;
    } else if (!stored.sameMetadata(fresh)) {
        stored.refreshFrom(fresh);
    }
    if (stored.mediaEtag.isEmpty()) {
        stored.mediaEtag = fresh.mediaEtag;
    }
    if (auto created = ensureDirectory(dir); !created) {
        return created;
    }
    // The sidecar comes first: no audio byte is ever stored without it.
    entry.info = stored;
    if (auto written = m_ops.writeFile(path(dir, "meta.json"), stored.toJson()); !written) {
        return written;
    }
    QFile::remove(path(dir, "data.tagging"));

    auto generation = std::make_shared<Generation>();
    generation->id = m_nextGeneration++;
    Result<PieceMap> map = reuse ? [&] -> Result<PieceMap> {
        const Result<QByteArray> data = readWholeFile(path(dir, "map"));
        return data ? PieceMap::parse(*data) : Result<PieceMap>(std::unexpected(data.error()));
    }()
        : Result<PieceMap>(fail(ErrorCode::NotFound));

    if (stored.protocol == StreamProtocol::Progressive) {
        const QString finished = path(dir, "data");
        const QString partial = path(dir, "data.part");
        const QString file = QFileInfo::exists(finished) ? finished : partial;
        const std::int64_t fileSize = QFileInfo(file).size();
        if (!map || map->layout() != PieceMap::Layout::Progressive || !QFileInfo::exists(file)) {
            QFile::remove(finished);
            QFile::remove(partial);
            map = PieceMap::progressive(stored.contentLength);
        } else {
            // Pieces the file cannot hold were never written (crash before the data reached the disk).
            const int count = map->pieceCount() < 0 ? map->pieceAt(fileSize) + 1 : map->pieceCount();
            for (int piece = 0; piece < count; ++piece) {
                if (map->has(piece) && map->pieceOffset(piece) + map->pieceSize(piece) > fileSize) {
                    map->unmark(piece);
                }
            }
            if (file == finished && !map->complete()) {
                QFile::rename(finished, partial);  // a finished file that lost pieces is partial again
            }
            generation->fd = std::make_shared<UniqueFd>(::open(
                QFile::encodeName(QFileInfo::exists(finished) ? finished : partial).constData(), O_RDONLY | O_CLOEXEC));
        }
    } else {
        const Result<QByteArray> text = readWholeFile(path(dir, "playlist.m3u8"));
        Result<HlsPlaylist> playlist = text ? parseHls(*text, QUrl(QStringLiteral("file:///")))
                                            : Result<HlsPlaylist>(std::unexpected(text.error()));
        if (playlist) {
            entry.playlist = *playlist;
            if (!map || map->layout() != PieceMap::Layout::Hls || map->pieceCount() != playlist->pieceCount()) {
                map = PieceMap::hls(playlist->pieceCount());
            }
        } else {
            removeTree(path(dir, "seg"));
            QFile::remove(path(dir, "playlist.m3u8"));
            map = PieceMap::hls(0);
        }
    }
    generation->map = std::move(*map);
    entry.current = std::move(generation);
    entry.info.complete = entry.current->map.complete() && entry.info.complete;
    m_ledger.upsert(entry.name, directorySize(dir), accessStamp());
    m_entryComplete.insert(entry.name, entry.info.complete);
    m_entryCovers.insert(entry.name, Covers{entry.info.coverHash, entry.info.thumbHash, entry.info.artworkUrl,
                                            entry.info.artworkThumbUrl});
    scheduleFlush({});
    return {};
}

void AudioCache::resetStored(EntryState& entry, const char* reason)
{
    qCInfo(lcCache) << "audio entry reset:" << reason;
    if (Live* l = live(entry.name)) {
        cancelFetch(*l);
        l->writeFd.reset();
        l->volatilePieces.clear();
        l->sourcePlaylist.reset();
    }
    {
        const std::scoped_lock lock(entry.mutex);
        entry.current->reset = true;
        auto generation = std::make_shared<Generation>();
        generation->id = m_nextGeneration++;
        generation->map = entry.info.protocol == StreamProtocol::Hls ? PieceMap::hls(0) : PieceMap::progressive(-1);
        entry.current = std::move(generation);
        entry.playlist.reset();
        entry.info.contentLength = -1;
        entry.info.complete = false;
        entry.info.payloadHash.clear();
        entry.info.taggedFor.clear();
        entry.info.mediaEtag.clear();  // recorded again from the next response
        entry.failure.reset();
    }
    entry.changed.notify_all();
    m_memory.dropEntry(entry.id);
    if (!entry.dir.isEmpty()) {
        for (const char* file : {"data", "data.part", "map", "playlist.m3u8"}) {
            QFile::remove(path(entry.dir, file));
        }
        removeTree(path(entry.dir, "seg"));
        writeSidecar(entry);
        m_ledger.upsert(entry.name, directorySize(entry.dir), accessStamp());
        m_entryComplete.insert(entry.name, false);
        scheduleFlush(entry.name);
    }
}

void AudioCache::readerOpened(Live& live, std::uint64_t readerId, bool background)
{
    live.readers.insert(readerId, background ? 0 : -1);
    if (!background) {
        live.foreground.insert(readerId);
    }
    m_memory.setActive(live.entry->id, true);
    if (!live.entry->dir.isEmpty()) {
        m_ledger.pin(live.entry->name);
        m_ledger.touch(live.entry->name, accessStamp());
    }
    bool complete = false;
    bool finished = false;
    {
        const std::scoped_lock lock(live.entry->mutex);
        complete = live.entry->current->map.complete();
        finished = live.entry->info.complete;
    }
    fetchCovers(live);
    if (complete && !finished) {
        finalize(live);  // e.g. the last piece arrived just before a crash
    } else if (complete) {
        maybeTag(live);  // tags may be outdated (metadata or cover changed)
    }
    if (!background) {
        continueFill(live);
    }
}

void AudioCache::readerClosed(const QByteArray& name, std::uint64_t readerId)
{
    Live* l = live(name);
    if (l == nullptr) {
        return;
    }
    l->readers.remove(readerId);
    l->foreground.remove(readerId);
    if (!l->entry->dir.isEmpty()) {
        m_ledger.unpin(name);
        m_ledger.touch(name, accessStamp());
        scheduleFlush(name);
    }
    if (!l->readers.isEmpty()) {
        return;
    }
    cancelFetch(*l);
    m_memory.setActive(l->entry->id, false);
    retire(name);
}

void AudioCache::retire(const QByteArray& name)
{
    Live* l = live(name);
    if (l == nullptr || !l->readers.isEmpty() || l->tagging || l->coverFetches > 0 || l->resolving) {
        return;
    }
    cancelFetch(*l);  // dropping the handle alone would leave the transfer running
    persistMap(*l->entry);
    m_dirty.removeAll(name);
    const std::shared_ptr<EntryState> entry = l->entry;
    const bool doomed = l->doomed;
    m_live.remove(name);
    if (doomed) {
        removeEntryFiles(name);
        m_memory.dropEntry(entry->id);
        collectCovers();  // the covers it kept referenced while in use
        Q_EMIT usageChanged();
        return;
    }
    const std::scoped_lock lock(entry->mutex);
    if (m_retired.size() >= kRetiredGenerations) {
        m_retired.erase(m_retired.begin());
    }
    m_retired.insert(name, Retired{entry->current, entry->dir, entry->info, entry->playlist});
}

// --- Fetching ------------------------------------------------------------------------------------------

void AudioCache::want(const QByteArray& name, int piece)
{
    Live* l = live(name);
    if (l == nullptr) {
        return;
    }
    EntryState& entry = *l->entry;
    {
        const std::scoped_lock lock(entry.mutex);
        if (entry.current->map.has(piece)) {
            return;
        }
        entry.failure.reset();
    }
    if (l->resolving) {
        // The fetch waiting for the URL starts from here instead (one fetch, not one per request).
        l->resumeAt = piece;
        l->retries = 0;
        return;
    }
    if (l->fetch) {
        const Fetch& fetch = *l->fetch;
        const int ahead = entry.info.protocol == StreamProtocol::Hls ? kHlsLookahead : kLookaheadPieces;
        // Pieces behind fetch.next were delivered already: one missing again (evicted) needs a new fetch.
        if (piece >= fetch.next && piece < fetch.next + ahead && (fetch.end < 0 || piece < fetch.end)) {
            return;  // the running fetch brings it (single flight)
        }
    }
    l->retries = 0;
    startFetch(*l, piece);
}

void AudioCache::setPrefetch(const QByteArray& name, std::uint64_t readerId, std::int64_t maxBytes)
{
    Live* l = live(name);
    if (l == nullptr || !l->readers.contains(readerId)) {
        return;
    }
    l->readers[readerId] = maxBytes;
    continueFill(*l);
}

void AudioCache::continueFill(Live& live)
{
    if (live.fetch || live.resolving || live.readers.isEmpty()) {
        return;
    }
    const std::int64_t limit = fillLimit(live.readers);
    std::optional<int> target;
    {
        const std::scoped_lock lock(live.entry->mutex);
        const PieceMap& map = live.entry->current->map;
        if (live.entry->failure) {
            return;
        }
        if (m_diskStalled && !live.entry->dir.isEmpty() && live.foreground.isEmpty()) {
            return;  // no room on disk: do not download ahead for nobody
        }
        if (map.layout() == PieceMap::Layout::Hls && !live.entry->playlist) {
            target = 0;
        } else {
            for (const int from : {live.resumeAt, 0}) {
                if (target = map.firstMissing(from); target) {
                    break;
                }
            }
            while (target && live.volatilePieces.contains(*target)) {
                target = map.firstMissing(*target + 1);
            }
        }
        if (target && limit >= 0) {
            const std::int64_t offset = map.layout() == PieceMap::Layout::Progressive
                                            ? map.pieceOffset(*target)
                                            : static_cast<std::int64_t>(*target) * PieceMap::kChunkSize;
            if (offset >= limit) {
                target.reset();
            }
        }
    }
    if (target) {
        startFetch(live, *target);
    }
}

void AudioCache::cancelFetch(Live& live)
{
    if (live.fetch) {
        live.fetch->handle.cancel();
        live.fetch.reset();
    }
}

void AudioCache::startFetch(Live& live, int piece)
{
    cancelFetch(live);
    live.resumeAt = piece;
    if (live.entry->info.protocol == StreamProtocol::Hls) {
        if (!live.sourcePlaylist) {
            withUrl(live, false, [this](Live& l) { fetchPlaylist(l); });
            return;
        }
        launchSegment(live, piece);
        return;
    }
    withUrl(live, false, [this](Live& l) { launchProgressive(l, l.resumeAt); });
}

void AudioCache::withUrl(Live& live, bool refresh, std::function<void(Live&)> then)
{
    if (live.url.isValid() && !refresh) {
        then(live);
        return;
    }
    live.afterUrl.push_back(std::move(then));
    if (live.resolving) {
        return;
    }
    if (!live.urls) {
        failWaiters(live, Error{ErrorCode::Offline, "no stream URL available"});
        return;
    }
    live.resolving = true;
    live.url.clear();
    live.urls(refresh, [mailbox = m_mailbox, cache = this, name = live.entry->name](Result<QUrl> url) {
        mailbox->post([cache, name, url = std::move(url)] {
            Live* l = cache->live(name);
            if (l == nullptr) {
                return;
            }
            l->resolving = false;
            const auto pending = std::exchange(l->afterUrl, {});
            if (!url || !url->isValid()) {
                cache->failWaiters(*l, url ? Error{ErrorCode::NotFound, "empty stream URL"} : url.error());
                cache->retire(name);
                return;
            }
            l->url = *url;
            if (!l->readers.isEmpty()) {  // all closed meanwhile: nobody needs the download any more
                for (const auto& then : pending) {
                    if (Live* again = cache->live(name)) {
                        then(*again);
                    }
                }
            }
            cache->retire(name);
        });
    });
}

void AudioCache::launchProgressive(Live& live, int piece)
{
    EntryState& entry = *live.entry;
    std::int64_t first = 0;
    std::int64_t last = -1;
    int end = -1;
    {
        const std::scoped_lock lock(entry.mutex);
        const PieceMap& map = entry.current->map;
        if (map.has(piece) || (map.pieceCount() >= 0 && piece >= map.pieceCount())) {
            return;
        }
        first = map.pieceOffset(piece);
        end = map.runEnd(piece);
        if (const std::int64_t limit = fillLimit(live.readers); limit >= 0) {
            // Only prefetching readers: stop at the limit, but always bring the piece asked for.
            const auto limitEnd =
                static_cast<int>(std::max<std::int64_t>((limit + map.chunkSize() - 1) / map.chunkSize(), piece + 1));
            end = end < 0 ? limitEnd : std::min(end, limitEnd);
        }
        if (end >= 0 && map.pieceCount() >= 0) {
            last = std::min(map.pieceOffset(end), map.contentLength()) - 1;
        } else if (end >= 0) {
            last = map.pieceOffset(end) - 1;
        }
    }
    net::Request request;
    request.url = live.url;
    request.range = net::ByteRange{first, last};
    request.background = live.foreground.isEmpty();
    request.cache = net::CacheMode::Bypass;
    Fetch fetch;
    fetch.id = m_nextFetch++;
    fetch.start = piece;
    fetch.next = piece;
    fetch.end = end;
    const QByteArray name = entry.name;
    const std::uint64_t id = fetch.id;
    net::Callbacks callbacks;
    callbacks.onResponse = [this, name, id](const net::Response& response) {
        onProgressiveResponse(name, id, response);
    };
    callbacks.onData = [this, name, id](const QByteArray& data) {
        onProgressiveData(name, id, data);
    };
    callbacks.onFinished = [this, name, id](Result<net::Response> result) {
        onFetchFinished(name, id, std::move(result));
    };
    cancelFetch(live);  // never orphan a running transfer
    live.fetch = std::move(fetch);
    ++m_networkRequests;
    live.fetch->handle = m_transport.send(std::move(request), this, std::move(callbacks));
}

void AudioCache::onProgressiveResponse(const QByteArray& name, std::uint64_t fetchId, const net::Response& response)
{
    Live* l = live(name);
    if (l == nullptr || !l->fetch || l->fetch->id != fetchId || !response.successful()) {
        return;
    }
    EntryState& entry = *l->entry;
    std::int64_t total = -1;
    std::int64_t expectedFirst = 0;
    {
        const std::scoped_lock lock(entry.mutex);
        expectedFirst = entry.current->map.pieceOffset(l->fetch->next);
    }
    if (response.status == 206) {
        std::int64_t first = -1;
        const std::optional<std::int64_t> parsed = contentRangeTotal(response.header("content-range"), first);
        if (!parsed || first != expectedFirst) {
            cancelFetch(*l);
            retryOrFail(*l, Error{ErrorCode::ParseError, "unexpected Content-Range"});
            return;
        }
        total = *parsed;
    } else {
        // The server ignored the range: skip to where we are.
        bool ok = false;
        total = response.header("content-length").toLongLong(&ok);
        total = ok ? total : -1;
        l->fetch->skip = expectedFirst;
    }
    const QByteArray etag = response.header("etag");
    bool changed = false;
    bool sidecar = false;
    {
        const std::scoped_lock lock(entry.mutex);
        PieceMap& map = entry.current->map;
        changed = (total >= 0 && map.contentLength() >= 0 && total != map.contentLength()) ||
                  (!etag.isEmpty() && !entry.info.mediaEtag.isEmpty() && etag != entry.info.mediaEtag);
        if (!changed) {
            if (total >= 0 && map.contentLength() < 0) {
                map.setContentLength(total);
                entry.info.contentLength = total;
                sidecar = true;
                const int run = map.runEnd(l->fetch->next);
                l->fetch->end = l->fetch->end < 0 ? run : std::min(l->fetch->end, run);
            }
            if (!etag.isEmpty() && entry.info.mediaEtag.isEmpty()) {
                entry.info.mediaEtag = etag;
                sidecar = true;
            }
        }
    }
    if (changed) {
        resetStored(entry, "the server sent different media");
        continueFill(*l);
        return;
    }
    if (sidecar) {
        writeSidecar(entry);
    }
}

void AudioCache::onProgressiveData(const QByteArray& name, std::uint64_t fetchId, const QByteArray& data)
{
    Live* l = live(name);
    if (l == nullptr || !l->fetch || l->fetch->id != fetchId) {
        return;
    }
    Fetch& fetch = *l->fetch;
    QByteArrayView incoming(data);
    if (fetch.skip > 0) {
        const auto dropped = static_cast<qsizetype>(std::min<std::int64_t>(fetch.skip, incoming.size()));
        incoming = incoming.sliced(dropped);
        fetch.skip -= dropped;
    }
    fetch.buffer.append(incoming);
    while (true) {
        std::int64_t size = 0;
        bool stop = false;
        {
            const std::scoped_lock lock(l->entry->mutex);
            const PieceMap& map = l->entry->current->map;
            size = map.contentLength() < 0 ? map.chunkSize() : map.pieceSize(fetch.next);
            stop = size <= 0;
        }
        if (stop || fetch.buffer.size() < size) {
            break;
        }
        const int piece = fetch.next++;
        storePiece(*l, piece, fetch.buffer.first(static_cast<qsizetype>(size)));
        if (!live(name) || !l->fetch || l->fetch->id != fetchId) {
            return;  // finalized and retired, or reset
        }
        fetch.buffer.remove(0, static_cast<qsizetype>(size));
        bool done = fetch.end >= 0 && fetch.next >= fetch.end;
        {
            const std::scoped_lock lock(l->entry->mutex);
            done = done || l->entry->current->map.has(fetch.next);
        }
        if (done) {
            // The run is complete (the rest is stored already): stop this request, go on elsewhere.
            l->resumeAt = fetch.next;
            cancelFetch(*l);
            l->retries = 0;
            continueFill(*l);
            return;
        }
    }
}

void AudioCache::onFetchFinished(const QByteArray& name, std::uint64_t fetchId, Result<net::Response> result)
{
    Live* l = live(name);
    if (l == nullptr || !l->fetch || l->fetch->id != fetchId) {
        return;
    }
    if (!result) {
        if (result.error().code == ErrorCode::Cancelled) {
            l->fetch.reset();
            return;
        }
        l->resumeAt = l->fetch->next;
        l->fetch.reset();
        retryOrFail(*l, result.error());
        return;
    }
    const net::Response& response = *result;
    const bool hls = l->entry->info.protocol == StreamProtocol::Hls;
    if (urlExpired(response.status)) {
        const int resume = l->fetch->next;
        l->fetch.reset();
        if (!l->urlRefreshed) {
            // Signed URLs expire: ask for a fresh one once, then resume where we were.
            l->urlRefreshed = true;
            l->sourcePlaylist.reset();
            l->resumeAt = resume;
            withUrl(*l, true, [this](Live& again) { startFetch(again, again.resumeAt); });
            return;
        }
        failWaiters(*l,
                    Error{response.status == 404 || response.status == 410 ? ErrorCode::NotFound : ErrorCode::Forbidden,
                          std::format("stream answered {}", response.status)});
        return;
    }
    if (hls) {
        const int piece = l->fetch->next;
        if (!response.successful()) {
            l->resumeAt = piece;
            l->fetch.reset();
            retryOrFail(*l, Error{ErrorCode::HttpError, std::format("segment answered {}", response.status)});
            return;
        }
        if (!l->sourcePlaylist) {
            l->fetch.reset();
            continueFill(*l);
            return;
        }
        const HlsSegment& segment = l->sourcePlaylist->piece(piece);
        if (segment.rangeLength >= 0 && response.body.size() != segment.rangeLength) {
            l->resumeAt = piece;
            l->fetch.reset();
            retryOrFail(*l, Error{ErrorCode::Corrupt, "segment has the wrong size"});
            return;
        }
        l->urlRefreshed = false;
        l->retries = 0;
        storePiece(*l, piece, response.body);
        if (live(name) == nullptr || !l->fetch || l->fetch->id != fetchId) {
            return;
        }
        l->fetch.reset();
        l->resumeAt = piece + 1;
        continueFill(*l);  // the next segment, unless only prefetches with their limit reached remain
        return;
    }
    if (response.status == 416) {
        bool lengthKnown = false;
        {
            const std::scoped_lock lock(l->entry->mutex);
            PieceMap& map = l->entry->current->map;
            lengthKnown = map.contentLength() >= 0;
            if (!lengthKnown) {
                map.setContentLength(map.pieceOffset(l->fetch->next));
                l->entry->info.contentLength = map.contentLength();
            }
        }
        l->fetch.reset();
        if (lengthKnown) {
            resetStored(*l->entry, "range not satisfiable");
        } else {
            writeSidecar(*l->entry);
            finalizeIfComplete(*l);
        }
        continueFill(*l);
        return;
    }
    if (!response.successful()) {
        l->resumeAt = l->fetch->next;
        l->fetch.reset();
        retryOrFail(*l, Error{ErrorCode::HttpError, std::format("stream answered {}", response.status)});
        return;
    }
    // The body ended: the rest of the buffer is the last (short) piece, or the stream was cut off.
    Fetch fetch = std::move(*l->fetch);
    l->fetch.reset();
    std::int64_t expected = 0;
    bool unknownLength = false;
    {
        const std::scoped_lock lock(l->entry->mutex);
        PieceMap& map = l->entry->current->map;
        unknownLength = map.contentLength() < 0;
        if (unknownLength && fetch.end < 0) {
            map.setContentLength(map.pieceOffset(fetch.next) + fetch.buffer.size());
            l->entry->info.contentLength = map.contentLength();
        }
        expected = map.pieceSize(fetch.next);
    }
    if (!fetch.buffer.isEmpty() && fetch.buffer.size() == expected) {
        storePiece(*l, fetch.next, fetch.buffer);
        ++fetch.next;
    }
    if (live(name) == nullptr) {
        return;
    }
    if (unknownLength) {
        writeSidecar(*l->entry);
        finalizeIfComplete(*l);
    }
    l->urlRefreshed = false;
    l->resumeAt = fetch.next;
    bool cutOff = false;
    {
        const std::scoped_lock lock(l->entry->mutex);
        const PieceMap& map = l->entry->current->map;
        cutOff = (fetch.end < 0 && fetch.next < map.pieceCount() && !map.has(fetch.next)) ||
                 (fetch.end >= 0 && fetch.next < fetch.end && !map.has(fetch.next));
    }
    if (cutOff) {
        retryOrFail(*l, Error{ErrorCode::ConnectionFailed, "the stream ended early"});
        return;
    }
    l->retries = 0;
    continueFill(*l);
}

void AudioCache::retryOrFail(Live& live, const Error& error)
{
    const ErrorCode code = error.code;
    if (code == ErrorCode::Offline || code == ErrorCode::MeteredDeferred || code == ErrorCode::NotCacheable ||
        live.retries >= kMaxRetries) {
        failWaiters(live, error);
        return;
    }
    ++live.retries;
    const std::chrono::milliseconds delay(250U << static_cast<unsigned>(live.retries));
    QTimer::singleShot(delay, this, [this, name = live.entry->name] {
        if (Live* l = this->live(name); l != nullptr && !l->fetch && !l->readers.isEmpty()) {
            startFetch(*l, l->resumeAt);
        }
    });
}

void AudioCache::failWaiters(Live& live, const Error& error)
{
    qCDebug(lcCache) << "audio fetch failed:" << error.describe();
    live.fetch.reset();
    {
        const std::scoped_lock lock(live.entry->mutex);
        live.entry->failure = error;
        ++live.entry->failureSeq;
    }
    live.entry->changed.notify_all();
}

void AudioCache::fetchPlaylist(Live& live)
{
    net::Request request;
    request.url = live.url;
    request.background = live.foreground.isEmpty();
    request.cache = net::CacheMode::Bypass;
    Fetch fetch;
    fetch.id = m_nextFetch++;
    fetch.start = live.resumeAt;
    fetch.next = live.resumeAt;
    const QByteArray name = live.entry->name;
    const std::uint64_t id = fetch.id;
    cancelFetch(live);  // never orphan a running transfer
    live.fetch = std::move(fetch);
    ++m_networkRequests;
    net::Callbacks callbacks;
    callbacks.onFinished = [this, name, id](Result<net::Response> result) {
        Live* l = this->live(name);
        if (l == nullptr || !l->fetch || l->fetch->id != id) {
            return;
        }
        if (!result || !result->successful()) {
            if (result && urlExpired(result->status) && !l->urlRefreshed) {
                l->urlRefreshed = true;
                l->fetch.reset();
                withUrl(*l, true, [this](Live& again) { fetchPlaylist(again); });
                return;
            }
            l->fetch.reset();
            retryOrFail(*l, result ? Error{ErrorCode::HttpError, "playlist request failed"} : result.error());
            return;
        }
        Result<HlsPlaylist> playlist = parseHls(result->body, result->url);
        l->fetch.reset();
        if (!playlist) {
            if (playlist.error().code == ErrorCode::NotCacheable) {
                // Encrypted media is protected content: no entry, no sidecar, nothing stays behind.
                l->doomed = true;
            }
            failWaiters(*l, playlist.error());
            return;
        }
        bool mismatch = false;
        bool fresh = false;
        {
            const std::scoped_lock lock(l->entry->mutex);
            if (l->entry->playlist && !l->entry->playlist->sameStructure(*playlist)) {
                mismatch = true;
            } else {
                fresh = !l->entry->playlist;
                l->entry->playlist = *playlist;
                if (l->entry->current->map.pieceCount() != playlist->pieceCount()) {
                    l->entry->current->map = PieceMap::hls(playlist->pieceCount());
                }
            }
        }
        if (mismatch) {
            resetStored(*l->entry, "the playlist changed");
            const std::scoped_lock lock(l->entry->mutex);
            l->entry->playlist = *playlist;
            l->entry->current->map = PieceMap::hls(playlist->pieceCount());
            fresh = true;
        }
        l->sourcePlaylist = std::move(*playlist);
        if (fresh && !l->entry->dir.isEmpty() && !l->doomed) {
            if (auto written = m_ops.writeFile(path(l->entry->dir, "playlist.m3u8"), l->sourcePlaylist->normalized());
                !written) {
                qCDebug(lcCache) << "playlist not stored:" << written.error().describe();
            }
            persistMap(*l->entry);
        }
        l->entry->changed.notify_all();
        const int resume = l->resumeAt;
        bool have = false;
        {
            const std::scoped_lock lock(l->entry->mutex);
            have = l->entry->current->map.has(resume);
        }
        if (have) {
            continueFill(*l);
        } else {
            launchSegment(*l, resume);
        }
    };
    live.fetch->handle = m_transport.send(std::move(request), this, std::move(callbacks));
}

void AudioCache::launchSegment(Live& live, int piece)
{
    if (!live.sourcePlaylist || piece >= live.sourcePlaylist->pieceCount()) {
        return;
    }
    const HlsSegment& segment = live.sourcePlaylist->piece(piece);
    net::Request request;
    request.url = segment.url;
    if (segment.rangeOffset >= 0) {
        request.range = net::ByteRange{segment.rangeOffset, segment.rangeOffset + segment.rangeLength - 1};
    }
    request.background = live.foreground.isEmpty();
    request.cache = net::CacheMode::Bypass;
    Fetch fetch;
    fetch.id = m_nextFetch++;
    fetch.start = piece;
    fetch.next = piece;
    const QByteArray name = live.entry->name;
    const std::uint64_t id = fetch.id;
    cancelFetch(live);  // never orphan a running transfer
    live.fetch = std::move(fetch);
    ++m_networkRequests;
    net::Callbacks callbacks;
    callbacks.onFinished = [this, name, id](Result<net::Response> result) {
        onFetchFinished(name, id, std::move(result));
    };
    live.fetch->handle = m_transport.send(std::move(request), this, std::move(callbacks));
}

// --- Storing -----------------------------------------------------------------------------------------

bool AudioCache::reserveSpace(Live& live, std::int64_t bytes)
{
    if (live.entry->dir.isEmpty() || live.doomed || !m_config.diskEnabled) {
        return false;
    }
    if (m_ledger.totalBytes() + bytes > m_config.diskBudget) {
        evictFor(m_ledger.totalBytes() + bytes - m_config.diskBudget, live.entry->name);
        if (m_ledger.totalBytes() + bytes > m_config.diskBudget) {
            m_diskStalled = true;
            return false;
        }
    }
    m_bytesSinceSpaceCheck += bytes;
    if (m_bytesSinceSpaceCheck >= kSpaceCheckInterval || m_diskStalled) {
        m_bytesSinceSpaceCheck = 0;
        std::int64_t free = freeDiskSpace(m_audioDir);
        if (free >= 0 && free - bytes < m_config.minFreeBytes) {
            evictFor(m_config.minFreeBytes - (free - bytes) + kFloorSlack, live.entry->name);
            free = freeDiskSpace(m_audioDir);
            if (free >= 0 && free - bytes < m_config.minFreeBytes) {
                m_diskStalled = true;
                return false;
            }
        }
    }
    m_diskStalled = false;
    return true;
}

void AudioCache::storePiece(Live& live, int piece, const QByteArray& data)
{
    EntryState& entry = *live.entry;
    const std::uint64_t sum = checksum(data);
    const auto size = static_cast<std::int64_t>(data.size());
    const bool hls = entry.info.protocol == StreamProtocol::Hls;
    bool onDisk = false;
    if (reserveSpace(live, size)) {
        const auto write = [&] -> Result<void> {
            if (hls) {
                if (auto dir = ensureDirectory(path(entry.dir, "seg")); !dir) {
                    return dir;
                }
                return m_ops.writeFile(path(entry.dir, "seg") + u'/' + QString::fromLatin1(hlsPieceName(piece)), data);
            }
            if (!live.writeFd) {
                // A finished file that lost a piece (corrupt on disk) is repaired in place: it is what readers use.
                const QString finished = path(entry.dir, "data");
                const QString file = QFileInfo::exists(finished) ? finished : path(entry.dir, "data.part");
                live.writeFd.reset(::open(QFile::encodeName(file).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
                if (!live.writeFd) {
                    return fail(errorCodeFromErrno(errno), "cannot create the data file");
                }
                const std::scoped_lock lock(entry.mutex);
                if (!entry.current->fd || !entry.current->fd->valid()) {
                    entry.current->fd =
                        std::make_shared<UniqueFd>(::open(QFile::encodeName(file).constData(), O_RDONLY | O_CLOEXEC));
                }
            }
            std::int64_t offset = 0;
            {
                const std::scoped_lock lock(entry.mutex);
                offset = entry.current->map.pieceOffset(piece);
            }
            return m_ops.writeAt(live.writeFd.get(), data, offset);
        };
        Result<void> written = write();
        if (!written && written.error().code == ErrorCode::NoSpace) {
            // Disk full: free our own space and try once more; never fail playback over it.
            evictFor(std::max<std::int64_t>(size * 64, kFloorSlack), entry.name);
            written = write();
        }
        if (written) {
            onDisk = true;
            m_ledger.addBytes(entry.name, size);
        } else {
            qCInfo(lcCache) << "audio piece kept in RAM only:" << written.error().describe();
            m_diskStalled = written.error().code == ErrorCode::NoSpace;
        }
    }
    {
        const std::scoped_lock lock(entry.mutex);
        entry.current->map.mark(piece, static_cast<std::uint32_t>(size), sum);
        entry.failure.reset();
    }
    if (!onDisk && !entry.dir.isEmpty()) {
        live.volatilePieces.insert(piece);
    }
    live.retries = 0;           // progress: a flaky connection may keep resuming
    live.urlRefreshed = false;  // and the URL that delivered it may expire again later
    m_memory.insert(PieceKey{entry.id, entry.current->id, static_cast<std::uint32_t>(piece)}, data);
    entry.changed.notify_all();
    if (onDisk) {
        scheduleFlush(entry.name);
    }
    finalizeIfComplete(live);
}

void AudioCache::finalizeIfComplete(Live& live)
{
    bool complete = false;
    {
        const std::scoped_lock lock(live.entry->mutex);
        complete = live.entry->current->map.complete() && !live.entry->info.complete;
    }
    if (complete && live.volatilePieces.isEmpty()) {
        finalize(live);
    }
}

void AudioCache::finalize(Live& live)
{
    EntryState& entry = *live.entry;
    if (entry.dir.isEmpty() || live.doomed || !live.volatilePieces.isEmpty()) {
        return;
    }
    const bool hls = entry.info.protocol == StreamProtocol::Hls;
    if (!hls) {
        live.writeFd.reset();
        // Validate every piece against its checksum before the entry counts as finished.
        std::shared_ptr<UniqueFd> fd;
        PieceMap map;
        {
            const std::scoped_lock lock(entry.mutex);
            fd = entry.current->fd;
            map = entry.current->map;
        }
        std::vector<int> bad;
        for (int piece = 0; piece < map.pieceCount(); ++piece) {
            const Result<QByteArray> data = fd ? readAt(fd->get(), map.pieceOffset(piece), map.pieceSize(piece))
                                               : Result<QByteArray>(fail(ErrorCode::NotFound));
            if (!data || data->size() != map.pieceSize(piece) || checksum(*data) != map.checksum(piece)) {
                bad.push_back(piece);
            }
        }
        if (!bad.empty()) {
            qCInfo(lcCache) << bad.size() << "corrupt audio pieces dropped before finalizing";
            {
                const std::scoped_lock lock(entry.mutex);
                for (const int piece : bad) {
                    entry.current->map.unmark(piece);
                }
            }
            persistMap(entry);
            continueFill(live);
            return;
        }
        if (QFileInfo::exists(path(entry.dir, "data.part")) &&
            std::rename(QFile::encodeName(path(entry.dir, "data.part")).constData(),
                        QFile::encodeName(path(entry.dir, "data")).constData()) != 0) {
            qCInfo(lcCache) << "could not finalize an audio entry";
            return;
        }
        const Result<QByteArray> payload = payloadHashOfFile(path(entry.dir, "data"));
        const std::scoped_lock lock(entry.mutex);
        entry.info.payloadHash = payload ? *payload : QByteArray();
    }
    {
        const std::scoped_lock lock(entry.mutex);
        entry.info.complete = true;
        entry.info.contentLength = hls ? entry.current->map.storedBytes() : entry.current->map.contentLength();
    }
    writeSidecar(entry);
    persistMap(entry);
    m_entryComplete.insert(entry.name, true);
    m_ledger.upsert(entry.name, directorySize(entry.dir), accessStamp());
    scheduleFlush({});
    Q_EMIT usageChanged();
    if (!hls) {
        maybeTag(live);
    }
}

void AudioCache::maybeTag(Live& live)
{
    EntryState& entry = *live.entry;
    if (!m_config.embedTags || entry.dir.isEmpty() || live.tagging || live.doomed || live.coverFetches > 0) {
        return;
    }
    TrackInfo info;
    {
        const std::scoped_lock lock(entry.mutex);
        info = entry.info;
    }
    const QString signature = tagSignature(info);
    if (info.protocol != StreamProtocol::Progressive || !info.complete || info.taggedFor == signature ||
        live.tagFailedFor == signature) {
        return;
    }
    // The front cover as received, unless it is huge: then the thumbnail; never re-encoded.
    QByteArray cover;
    for (const QByteArray& hash : {info.coverHash, info.thumbHash}) {
        if (hash.isEmpty()) {
            continue;
        }
        const Result<QByteArray> data = readWholeFile(m_coversDir + u'/' + QString::fromLatin1(hash));
        if (!data) {
            continue;
        }
        QBuffer buffer;
        buffer.setData(*data);
        const QSize size = QImageReader(&buffer).size();
        if (size.isValid() && std::max(size.width(), size.height()) <= kMaxEmbeddedCoverSide) {
            cover = *data;
            break;
        }
    }
    live.tagging = true;
    live.tagAttemptFor = signature;
    const QString file = path(entry.dir, "data");
    const TagData tags = tagDataFor(info, cover, info.coverMime);
    FileOps* ops = &m_ops;
    const std::uint32_t generation = entry.current->id;
    const QByteArray name = entry.name;
    const QByteArray expected = info.payloadHash;
    (void)QtConcurrent::run([file, tags, expected, ops, mailbox = m_mailbox, cache = this, name, generation] {
        Result<PieceMap> outcome = [&] -> Result<PieceMap> {
            if (auto written = writeTags(file, tags, expected, *ops); !written) {
                return std::unexpected(written.error());
            }
            return mapOfFile(file);
        }();
        mailbox->post([cache, name, generation, outcome = std::move(outcome)] mutable {
            cache->onTagged(name, generation, std::move(outcome));
        });
    });
}

void AudioCache::onTagged(const QByteArray& name, std::uint32_t generation, Result<PieceMap> map)
{
    Live* l = live(name);
    if (l == nullptr) {
        return;
    }
    l->tagging = false;
    EntryState& entry = *l->entry;
    bool sameGeneration = false;
    {
        const std::scoped_lock lock(entry.mutex);
        sameGeneration = entry.current->id == generation;
    }
    if (!map) {
        if (map.error().code == ErrorCode::Unsupported) {
            const std::scoped_lock lock(entry.mutex);
            entry.info.taggedFor = l->tagAttemptFor;  // sidecar only for this container
        } else {
            l->tagFailedFor = l->tagAttemptFor;  // the same pass would fail the same way
            qCInfo(lcCache) << "tagging skipped, the entry is unchanged:" << map.error().describe();
        }
    } else if (sameGeneration && !entry.dir.isEmpty()) {
        // The file was rewritten: readers opened from now on use the new layout; open readers keep the old file.
        auto next = std::make_shared<Generation>();
        next->id = m_nextGeneration++;
        next->map = std::move(*map);
        next->fd = std::make_shared<UniqueFd>(
            ::open(QFile::encodeName(path(entry.dir, "data")).constData(), O_RDONLY | O_CLOEXEC));
        const std::scoped_lock lock(entry.mutex);
        entry.current = std::move(next);
        entry.info.taggedFor = l->tagAttemptFor;  // what was written, not what may have changed meanwhile
    }
    writeSidecar(entry);
    persistMap(entry);
    if (!entry.dir.isEmpty()) {  // empty: the cache moved while tagging
        m_ledger.upsert(name, directorySize(entry.dir), accessStamp());
    }
    scheduleFlush({});
    if (l->resetAfterTagging) {
        l->resetAfterTagging = false;
        resetStored(entry, "media changed");
    } else {
        maybeTag(*l);  // metadata may have changed meanwhile
    }
    retire(name);
}

void AudioCache::fetchCovers(Live& live)
{
    EntryState& entry = *live.entry;
    if (entry.dir.isEmpty() || live.doomed || live.coverFetches > 0) {
        return;
    }
    TrackInfo info;
    {
        const std::scoped_lock lock(entry.mutex);
        info = entry.info;
    }
    const auto request = [&](const QUrl& url, bool thumb) {
        net::Request r;
        r.url = url;
        r.cache = net::CacheMode::Bypass;
        ++live.coverFetches;
        ++m_networkRequests;
        net::Callbacks callbacks;
        callbacks.onFinished = [this, name = entry.name, thumb](Result<net::Response> result) {
            storeCover(name, thumb, std::move(result));
        };
        m_transport.send(std::move(r), this, std::move(callbacks));
    };
    if (info.coverHash.isEmpty() && info.artworkUrl.isValid()) {
        request(info.artworkUrl, false);
    }
    if (info.thumbHash.isEmpty() && info.artworkThumbUrl.isValid()) {
        request(info.artworkThumbUrl, true);
    }
}

void AudioCache::storeCover(const QByteArray& name, bool thumb, Result<net::Response> result)
{
    Live* l = live(name);
    if (l == nullptr) {
        return;
    }
    --l->coverFetches;
    // Only a real image is stored: a failed request or an error page is retried on a later open.
    bool image = false;
    if (result && result->successful() && !result->body.isEmpty()) {
        QBuffer buffer;
        buffer.setData(result->body);
        image = QImageReader(&buffer).canRead();
    }
    if (image && !l->doomed && !l->entry->dir.isEmpty()) {
        const QByteArray hash = contentHash(result->body);
        const QString file = m_coversDir + u'/' + QString::fromLatin1(hash);
        if (QFileInfo::exists(file) || m_ops.writeFile(file, result->body)) {
            {
                const std::scoped_lock lock(l->entry->mutex);
                (thumb ? l->entry->info.thumbHash : l->entry->info.coverHash) = hash;
                if (!thumb) {
                    l->entry->info.coverMime =
                        QString::fromLatin1(result->header("content-type").split(';').front().trimmed());
                }
                const TrackInfo& info = l->entry->info;
                m_entryCovers.insert(name,
                                     Covers{info.coverHash, info.thumbHash, info.artworkUrl, info.artworkThumbUrl});
            }
            writeSidecar(*l->entry);
            scheduleFlush({});
        }
    }
    if (l->coverFetches == 0) {
        maybeTag(*l);
        retire(name);
    }
}

void AudioCache::writeSidecar(EntryState& entry)
{
    if (entry.dir.isEmpty()) {
        return;
    }
    if (Live const* l = live(entry.name); l != nullptr && l->doomed) {
        return;
    }
    QByteArray json;
    {
        const std::scoped_lock lock(entry.mutex);
        json = entry.info.toJson();
    }
    if (auto written = m_ops.writeFile(path(entry.dir, "meta.json"), json); !written) {
        qCDebug(lcCache) << "sidecar not written:" << written.error().describe();
    }
}

void AudioCache::persistMap(EntryState& entry)
{
    if (entry.dir.isEmpty()) {
        return;
    }
    Live const* l = live(entry.name);
    if (l != nullptr && l->doomed) {
        return;
    }
    PieceMap map;
    {
        const std::scoped_lock lock(entry.mutex);
        map = entry.current->map;
    }
    if (l != nullptr) {
        for (const int piece : std::as_const(l->volatilePieces)) {
            map.unmark(piece);  // in RAM only: not stored
        }
    }
    if (auto written = m_ops.writeFile(path(entry.dir, "map"), map.serialize()); !written) {
        qCDebug(lcCache) << "piece map not written:" << written.error().describe();
    }
}

// --- Eviction, clearing, usage -------------------------------------------------------------------------

void AudioCache::removeEntryFiles(const QByteArray& name)
{
    if (!m_layout) {
        return;
    }
    removeTree(entryDir(name));
    QDir(m_layout->audio()).rmdir(QString::fromLatin1(name.first(2)));  // only if the bucket is empty now
    m_ledger.remove(name);
    m_entryComplete.remove(name);
    m_entryCovers.remove(name);
    m_retired.remove(name);
    scheduleFlush({});
}

void AudioCache::evictFor(std::int64_t bytes, const QByteArray& keep)
{
    if (!m_indexLoaded) {
        return;  // the index is still loading: nothing to choose from yet
    }
    std::vector<QByteArray> victims = m_ledger.overBudget(m_config.diskBudget, keep);
    if (bytes > 0) {
        for (const QByteArray& name : m_ledger.victims(bytes, keep)) {
            if (std::ranges::find(victims, name) == victims.end()) {
                victims.push_back(name);
            }
        }
    }
    for (const QByteArray& name : victims) {
        if (live(name) == nullptr) {
            removeEntryFiles(name);
            if (const auto it = m_retired.find(name); it != m_retired.end()) {
                m_retired.erase(it);
            }
        }
    }
    if (!victims.empty()) {
        collectCovers();
        Q_EMIT usageChanged();
    }
}

void AudioCache::collectCovers()
{
    if (!m_layout) {
        return;
    }
    QSet<QByteArray> referenced;
    for (const Covers& covers : std::as_const(m_entryCovers)) {
        referenced.insert(covers.cover);
        referenced.insert(covers.thumb);
    }
    for (const auto& l : std::as_const(m_live)) {
        const std::scoped_lock lock(l->entry->mutex);
        referenced.insert(l->entry->info.coverHash);
        referenced.insert(l->entry->info.thumbHash);
    }
    for (const QString& file : QDir(m_layout->covers()).entryList(QDir::Files)) {
        if (!referenced.contains(file.toLatin1())) {
            QFile::remove(m_layout->covers() + u'/' + file);
        }
    }
}

void AudioCache::clear()
{
    for (const QByteArray& name : m_ledger.items().keys()) {
        if (Live* l = live(name)) {
            l->doomed = true;  // in use: deleted when its last reader closes
            m_ledger.remove(name);
            m_entryComplete.remove(name);
            m_entryCovers.remove(name);
        } else {
            removeEntryFiles(name);
        }
    }
    m_retired.clear();
    for (const auto& l : std::as_const(m_live)) {
        m_memory.dropEntry(l->entry->id);
    }
    m_memory.clear();
    collectCovers();
    m_diskStalled = false;
    scheduleFlush({});
    Q_EMIT usageChanged();
}

void AudioCache::removeTrack(const QString& trackId)
{
    for (const TrackInfo& info : cachedTracks()) {
        if (info.key.trackId == trackId) {
            const QByteArray name = info.key.name();
            if (Live* l = live(name)) {
                l->doomed = true;
            } else {
                removeEntryFiles(name);
            }
        }
    }
    collectCovers();
    Q_EMIT usageChanged();
}

AudioUsage AudioCache::usage() const
{
    AudioUsage usage;
    for (auto it = m_ledger.items().cbegin(); it != m_ledger.items().cend(); ++it) {
        if (m_entryComplete.value(it.key())) {
            usage.completeBytes += it->bytes;
            ++usage.completeEntries;
        } else {
            usage.partialBytes += it->bytes;
            ++usage.partialEntries;
        }
    }
    if (m_layout) {
        usage.coverBytes = directorySize(m_layout->covers());
    }
    usage.diskStalled = m_diskStalled;
    return usage;
}

std::vector<TrackInfo> AudioCache::cachedTracks() const
{
    std::vector<TrackInfo> tracks;
    if (!m_layout) {
        return tracks;
    }
    for (auto it = m_ledger.items().cbegin(); it != m_ledger.items().cend(); ++it) {
        const Result<QByteArray> json = readWholeFile(path(entryDir(it.key()), "meta.json"));
        if (json) {
            if (Result<TrackInfo> info = TrackInfo::fromJson(*json)) {
                tracks.push_back(std::move(*info));
            }
        }
    }
    return tracks;
}

std::optional<QByteArray> AudioCache::coverFor(const QUrl& url) const
{
    if (!m_layout || url.isEmpty()) {
        return std::nullopt;
    }
    for (const Covers& covers : m_entryCovers) {
        const QByteArray& hash = covers.coverUrl == url   ? covers.cover
                                 : covers.thumbUrl == url ? covers.thumb
                                                          : QByteArray();
        if (!hash.isEmpty()) {
            if (Result<QByteArray> data = readWholeFile(m_layout->covers() + u'/' + QString::fromLatin1(hash))) {
                return std::move(*data);
            }
        }
    }
    return std::nullopt;
}

}  // namespace noctara::cache
