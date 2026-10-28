#pragma once

#include "cache/CacheLayout.hpp"
#include "cache/Hls.hpp"
#include "cache/LruLedger.hpp"
#include "cache/Mailbox.hpp"
#include "cache/MemoryTier.hpp"
#include "cache/PieceMap.hpp"
#include "cache/TrackInfo.hpp"
#include "core/Error.hpp"
#include "net/CallbackChannel.hpp"
#include "net/Transport.hpp"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace noctara::cache {

class FileOps;
class AudioCache;
struct EntryState;
struct Generation;

using UrlCallback = std::function<void(Result<QUrl>)>;
// Supplies the stream URL of an entry (the progressive file or the HLS media playlist). `refresh` asks for a
// freshly signed URL because the previous one was refused (signed URLs expire). Called on the cache thread; the
// callback may be invoked from any thread.
using UrlProvider = std::function<void(bool refresh, UrlCallback callback)>;

struct AudioCacheConfig {
    bool diskEnabled = true;  // Enable audio cache, a disk budget above 0 and the cache lock held
    std::int64_t diskBudget = 2048LL << 20;
    std::int64_t minFreeBytes = 1024LL << 20;  // free-space floor of the file system
    bool embedTags = true;
};

struct AudioUsage {
    std::int64_t completeBytes = 0;
    std::int64_t partialBytes = 0;
    std::int64_t coverBytes = 0;
    int completeEntries = 0;
    int partialEntries = 0;
    bool diskStalled = false;  // writes stopped: budget or free-space floor reached with nothing evictable
};

// What a reader of one cache entry sees: the pieces of one generation of the entry (a fixed-size chunk of a
// progressive stream, or an HLS segment), served from RAM, then disk, then the network. Readers stay bound to
// the generation they were opened on, so re-tagging a finished file never shifts the bytes under a decoder.
// Thread-safe; read() blocks and must not be called on the cache thread.
class AudioReader {
public:
    enum class Status : std::uint8_t { Ready, Pending, Failed };
    struct Piece {
        Status status = Status::Pending;
        QByteArray data;
        Error error;
    };

    AudioReader(std::shared_ptr<EntryState> entry, std::shared_ptr<Generation> generation, MemoryTier& memory,
                AudioCache* cache, MailboxPtr mailbox, std::uint64_t id);
    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;
    AudioReader(AudioReader&&) = delete;
    AudioReader& operator=(AudioReader&&) = delete;

    [[nodiscard]] TrackInfo info() const;
    [[nodiscard]] StreamProtocol protocol() const;
    // Progressive: bytes of this generation's file, -1 until the server told the length.
    [[nodiscard]] std::int64_t contentLength() const;
    // -1 while unknown.
    [[nodiscard]] int pieceCount() const;
    [[nodiscard]] std::int64_t pieceOffset(int piece) const;
    [[nodiscard]] int pieceAt(std::int64_t offset) const;
    [[nodiscard]] std::optional<HlsPlaylist> playlist() const;
    [[nodiscard]] bool complete() const;
    // Stored byte ranges of a progressive entry (for the waveform's cached-range display).
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> cachedRanges() const;
    // What is stored, as merged fractions [from, to) of the track: by bytes for progressive entries, by playing
    // time (segment durations) for HLS. Empty while the size or the playlist is unknown.
    [[nodiscard]] std::vector<std::pair<double, double>> cachedFractions() const;

    // Never waits for the network: Pending means the piece has been requested.
    [[nodiscard]] Piece tryRead(int piece);
    // Waits until the piece is available, the fetch fails, `stop` is requested or `timeout` passes.
    [[nodiscard]] Result<QByteArray> read(int piece, const std::stop_token& stop, std::chrono::milliseconds timeout);
    // Progressive convenience: [offset, offset + size), shorter at the end of the stream.
    [[nodiscard]] Result<QByteArray> readBytes(std::int64_t offset, std::int64_t size, const std::stop_token& stop,
                                               std::chrono::milliseconds timeout);
    // Downloads the entry in the background (up to `maxBytes`, -1 = all) while this reader is open.
    void prefetch(std::int64_t maxBytes = -1);

    [[nodiscard]] std::uint64_t id() const noexcept { return m_id; }

private:
    void request(int piece);

    std::shared_ptr<EntryState> m_entry;
    std::shared_ptr<Generation> m_generation;
    MemoryTier& m_memory;
    AudioCache* m_cache;  // only dereferenced on the cache thread, inside closures the mailbox delivered
    MailboxPtr m_mailbox;
    std::uint64_t m_id;
    std::atomic<int> m_requested{-1};
    std::atomic<std::uint64_t> m_seenFailure{0};
};

using AudioReaderPtr = std::shared_ptr<AudioReader>;

// Tiered cache of encoded audio for regular (non-DRM) tracks: RAM (MemoryTier) and disk under <cache>/audio,
// filled through the transport. Entries are keyed by track and transcoding, are self-describing (the sidecar
// is written before the first audio byte), resumable when partial, validated by per-piece checksums on read,
// finalized atomically and then tagged. One fetch per entry serves all its readers (single-flight). Lives on
// the cache thread; open() and the readers may be used from any thread.
class AudioCache : public QObject {
    Q_OBJECT

public:
    using OpenCallback = std::function<void(Result<AudioReaderPtr>)>;

    AudioCache(net::INetworkTransport& transport, MemoryTier& memory, FileOps& ops, QObject* parent = nullptr);
    ~AudioCache() override;
    AudioCache(const AudioCache&) = delete;
    AudioCache& operator=(const AudioCache&) = delete;
    AudioCache(AudioCache&&) = delete;
    AudioCache& operator=(AudioCache&&) = delete;

    // Cache thread. nullopt keeps entries in RAM only (no disk, no sidecars).
    void setLayout(std::optional<CacheLayout> layout);
    void setConfig(const AudioCacheConfig& config);
    // Loads the disk index (or rebuilds it); called once, after startup. Eviction waits for it.
    void loadIndex();
    [[nodiscard]] bool indexLoaded() const noexcept { return m_indexLoaded; }

    // Any thread. Opens (or creates) the entry for `info` and hands a reader to `done` on `context`'s thread.
    // DRM tracks are refused with ErrorCode::NotCacheable before anything is created.
    void open(const TrackInfo& info, UrlProvider urls, QObject* context, OpenCallback done, bool background = false);

    // Cache thread.
    [[nodiscard]] AudioUsage usage() const;
    [[nodiscard]] std::vector<TrackInfo> cachedTracks() const;
    // Removes every entry not in use; entries in use go when their last reader closes.
    void clear();
    void removeTrack(const QString& trackId);
    // A stored cover by its source URL (the image cache asks when offline).
    [[nodiscard]] std::optional<QByteArray> coverFor(const QUrl& url) const;
    void flush();

    [[nodiscard]] std::int64_t networkRequests() const noexcept { return m_networkRequests; }

    static constexpr int kMaxEmbeddedCoverSide = 1200;

Q_SIGNALS:
    void usageChanged();

private:
    friend class AudioReader;
    struct Fetch;
    struct Live;

    void openOnThread(const TrackInfo& info, const UrlProvider& urls, const std::shared_ptr<net::ChannelPtr>& channel,
                      const std::shared_ptr<OpenCallback>& done, bool background);
    Result<std::shared_ptr<EntryState>> createEntry(const TrackInfo& info);
    Result<void> loadStored(EntryState& entry, const TrackInfo& fresh);
    void resetStored(EntryState& entry, const char* reason);
    void readerOpened(Live& live, std::uint64_t readerId, bool background);
    void readerClosed(const QByteArray& name, std::uint64_t readerId);
    void retire(const QByteArray& name);
    void want(const QByteArray& name, int piece);
    void setPrefetch(const QByteArray& name, std::uint64_t readerId, std::int64_t maxBytes);

    void startFetch(Live& live, int piece);
    void cancelFetch(Live& live);
    void withUrl(Live& live, bool refresh, std::function<void(Live&)> then);
    void launchProgressive(Live& live, int piece);
    void launchSegment(Live& live, int piece);
    void fetchPlaylist(Live& live);
    void onProgressiveResponse(const QByteArray& name, std::uint64_t fetchId, const net::Response& response);
    void onProgressiveData(const QByteArray& name, std::uint64_t fetchId, const QByteArray& data);
    void onFetchFinished(const QByteArray& name, std::uint64_t fetchId, Result<net::Response> result);
    void continueFill(Live& live);
    void retryOrFail(Live& live, const Error& error);
    void failWaiters(Live& live, const Error& error);

    void storePiece(Live& live, int piece, const QByteArray& data);
    void finalizeIfComplete(Live& live);
    [[nodiscard]] bool reserveSpace(Live& live, std::int64_t bytes);
    void finalize(Live& live);
    void maybeTag(Live& live);
    void onTagged(const QByteArray& name, std::uint32_t generation, Result<PieceMap> map);
    void fetchCovers(Live& live);
    void storeCover(const QByteArray& name, bool thumb, Result<net::Response> result);

    void writeSidecar(EntryState& entry);
    void persistMap(EntryState& entry);
    void scheduleFlush(const QByteArray& name);
    void removeEntryFiles(const QByteArray& name);
    void evictFor(std::int64_t bytes, const QByteArray& keep);
    void collectCovers();
    [[nodiscard]] QString entryDir(const QByteArray& name) const;
    [[nodiscard]] Live* live(const QByteArray& name);

    net::INetworkTransport& m_transport;
    MemoryTier& m_memory;
    FileOps& m_ops;
    MailboxPtr m_mailbox;
    std::optional<CacheLayout> m_layout;
    QString m_audioDir;  // of m_layout, empty without one
    QString m_coversDir;
    AudioCacheConfig m_config;
    QHash<QByteArray, std::shared_ptr<Live>> m_live;  // entries with readers (or pending work)
    LruLedger m_ledger;                               // disk entries
    struct Covers {
        QByteArray cover;  // content hashes of the stored files
        QByteArray thumb;
        QUrl coverUrl;  // where they came from (the image cache asks by URL when offline)
        QUrl thumbUrl;
    };
    QHash<QByteArray, Covers> m_entryCovers;
    QHash<QByteArray, bool> m_entryComplete;
    // Recently closed entries: reopening one keeps its generation, so its pieces in RAM still match.
    struct Retired {
        std::shared_ptr<Generation> generation;
        QString dir;
        TrackInfo info;
        std::optional<HlsPlaylist> playlist;
    };
    QHash<QByteArray, Retired> m_retired;
    QList<QByteArray> m_dirty;
    QTimer m_flushTimer;
    std::uint64_t m_nextReader = 1;
    std::uint64_t m_nextFetch = 1;
    std::uint32_t m_nextGeneration = 1;
    std::int64_t m_networkRequests = 0;
    std::int64_t m_bytesSinceSpaceCheck = 0;
    bool m_indexLoaded = false;
    bool m_indexDirty = false;
    bool m_diskStalled = false;
};

}  // namespace noctara::cache
