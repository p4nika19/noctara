#pragma once

#include "cache/DiskLru.hpp"
#include "cache/Mailbox.hpp"
#include "core/Error.hpp"
#include "net/CallbackChannel.hpp"
#include "net/Transport.hpp"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace noctara::cache {

class FileOps;

struct ImageUsage {
    std::int64_t memoryBytes = 0;
    std::int64_t memoryBudget = 0;
    std::int64_t diskBytes = 0;
    int diskCount = 0;
};

// Cover art and avatars: decoded images in a byte-budgeted RAM LRU keyed by URL *and* decode size (a 64 px
// thumbnail and the full-screen cover are different entries), encoded bytes in a disk LRU under
// <cache>/images. Concurrent requests for one URL share one download; decoding runs on the thread pool.
// Lives on the cache thread; request() and cached() may be called from any thread.
class ImageCache : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(Result<QImage>)>;
    // Stored covers of audio entries, used when the network cannot deliver (offline).
    using Fallback = std::function<std::optional<QByteArray>(const QUrl&)>;

    ImageCache(net::INetworkTransport& transport, FileOps& ops, std::int64_t memoryBudget, QObject* parent = nullptr);
    ~ImageCache() override;
    ImageCache(const ImageCache&) = delete;
    ImageCache& operator=(const ImageCache&) = delete;
    ImageCache(ImageCache&&) = delete;
    ImageCache& operator=(ImageCache&&) = delete;

    // Cache thread. An empty directory keeps images in RAM only.
    void setDisk(const QString& directory, std::int64_t budget);
    void setFallback(Fallback fallback) { m_fallback = std::move(fallback); }
    void clear();
    void flush();
    [[nodiscard]] ImageUsage usage() const;

    // Any thread.
    void setMemoryBudget(std::int64_t bytes);
    [[nodiscard]] std::int64_t memoryBudget() const;
    void trimMemory(std::int64_t bytes);
    // Decodes to fit within `size` (keeping the aspect ratio, never enlarging); an invalid size keeps the original.
    void request(const QUrl& url, QSize size, QObject* context, Callback done);
    [[nodiscard]] std::optional<QImage> cached(const QUrl& url, QSize size);

private:
    struct Waiter {
        QSize size;
        std::shared_ptr<net::ChannelPtr> channel;
        std::shared_ptr<Callback> done;
    };

    // Where decoded bytes came from: only a download is stored, and only once it decodes.
    enum class Source : std::uint8_t { Network, Disk, Fallback };

    void requestOnThread(const QUrl& url, Waiter waiter, bool useDisk = true);
    void onDownloaded(const QByteArray& key, const QUrl& url, Result<net::Response> result);
    void decode(const QUrl& url, const QByteArray& data, std::vector<Waiter> waiters, Source source);
    static void deliver(const Waiter& waiter, Result<QImage> result);

    [[nodiscard]] static QByteArray memoryKey(const QUrl& url, QSize size);
    void insertMemory(const QByteArray& key, const QImage& image);

    net::INetworkTransport& m_transport;
    FileOps& m_ops;
    MailboxPtr m_mailbox;
    std::unique_ptr<DiskLru> m_disk;
    Fallback m_fallback;
    QHash<QByteArray, std::vector<Waiter>> m_pending;  // URL -> waiting requests (one download each)

    // RAM tier (any thread).
    mutable std::mutex m_mutex;
    std::list<std::pair<QByteArray, QImage>> m_lru;  // front = most recently used
    std::unordered_map<QByteArray, std::list<std::pair<QByteArray, QImage>>::iterator> m_index;
    std::int64_t m_memoryBytes = 0;
    std::int64_t m_memoryBudget;
};

}  // namespace noctara::cache
