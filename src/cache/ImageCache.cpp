#include "cache/ImageCache.hpp"

#include "cache/Hash.hpp"
#include "core/Log.hpp"

#include <QBuffer>
#include <QImageReader>
#include <QtConcurrentRun>

#include <algorithm>

namespace noctara::cache {

namespace {

constexpr std::int64_t kMaxImageBytes = 16LL * 1024 * 1024;

std::int64_t costOf(const QImage& image)
{
    return image.sizeInBytes();
}

QImage decodeImage(const QByteArray& data, QSize size)
{
    QBuffer buffer;
    buffer.setData(data);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (size.isValid() && !size.isEmpty() && original.isValid() &&
        (original.width() > size.width() || original.height() > size.height())) {
        reader.setScaledSize(original.scaled(size, Qt::KeepAspectRatio));
    }
    return reader.read();
}

}  // namespace

ImageCache::ImageCache(net::INetworkTransport& transport, FileOps& ops, std::int64_t memoryBudget, QObject* parent)
    : QObject(parent)
    , m_transport(transport)
    , m_ops(ops)
    , m_mailbox(std::make_shared<Mailbox>(this))
    , m_memoryBudget(memoryBudget)
{
}

ImageCache::~ImageCache()
{
    m_mailbox->close();
    flush();
}

void ImageCache::setDisk(const QString& directory, std::int64_t budget)
{
    if (directory.isEmpty()) {
        flush();
        m_disk.reset();
        return;
    }
    if (!m_disk || m_disk->directory() != directory) {
        flush();
        m_disk = std::make_unique<DiskLru>(directory, budget, m_ops);
    }
    m_disk->setBudget(budget);
}

void ImageCache::clear()
{
    if (m_disk) {
        m_disk->clear();
    }
    const std::scoped_lock lock(m_mutex);
    m_lru.clear();
    m_index.clear();
    m_memoryBytes = 0;
}

void ImageCache::flush()
{
    if (m_disk) {
        m_disk->flushIndex();
    }
}

ImageUsage ImageCache::usage() const
{
    ImageUsage usage;
    if (m_disk) {
        m_disk->load();
        usage.diskBytes = m_disk->bytes();
        usage.diskCount = static_cast<int>(m_disk->count());
    }
    const std::scoped_lock lock(m_mutex);
    usage.memoryBytes = m_memoryBytes;
    usage.memoryBudget = m_memoryBudget;
    return usage;
}

void ImageCache::setMemoryBudget(std::int64_t bytes)
{
    const std::scoped_lock lock(m_mutex);
    m_memoryBudget = bytes;
    while (m_memoryBytes > m_memoryBudget && !m_lru.empty()) {
        m_memoryBytes -= costOf(m_lru.back().second);
        m_index.erase(m_lru.back().first);
        m_lru.pop_back();
    }
}

std::int64_t ImageCache::memoryBudget() const
{
    const std::scoped_lock lock(m_mutex);
    return m_memoryBudget;
}

void ImageCache::trimMemory(std::int64_t bytes)
{
    const std::scoped_lock lock(m_mutex);
    while (m_memoryBytes > bytes && !m_lru.empty()) {
        m_memoryBytes -= costOf(m_lru.back().second);
        m_index.erase(m_lru.back().first);
        m_lru.pop_back();
    }
}

QByteArray ImageCache::memoryKey(const QUrl& url, QSize size)
{
    const QByteArray prefix = size.isValid()
                                  ? QByteArray::number(size.width()) + 'x' + QByteArray::number(size.height())
                                  : QByteArray("orig");
    return prefix + ' ' + url.toEncoded();
}

void ImageCache::insertMemory(const QByteArray& key, const QImage& image)
{
    const std::int64_t cost = costOf(image);
    const std::scoped_lock lock(m_mutex);
    if (cost > m_memoryBudget) {
        return;
    }
    if (const auto it = m_index.find(key); it != m_index.end()) {
        m_memoryBytes -= costOf(it->second->second);
        m_lru.erase(it->second);
        m_index.erase(it);
    }
    while (m_memoryBytes + cost > m_memoryBudget && !m_lru.empty()) {
        m_memoryBytes -= costOf(m_lru.back().second);
        m_index.erase(m_lru.back().first);
        m_lru.pop_back();
    }
    m_lru.emplace_front(key, image);
    m_index.emplace(key, m_lru.begin());
    m_memoryBytes += cost;
}

std::optional<QImage> ImageCache::cached(const QUrl& url, QSize size)
{
    const std::scoped_lock lock(m_mutex);
    const auto it = m_index.find(memoryKey(url, size));
    if (it == m_index.end()) {
        return std::nullopt;
    }
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return it->second->second;
}

void ImageCache::request(const QUrl& url, QSize size, QObject* context, Callback done)
{
    Waiter waiter;
    waiter.size = size;
    waiter.channel = std::make_shared<net::ChannelPtr>(
        net::makeChannel(context, std::make_shared<std::atomic<bool>>(false), thread()));
    waiter.done = std::make_shared<Callback>(std::move(done));
    if (std::optional<QImage> hit = cached(url, size)) {
        // Still delivered asynchronously, like every other result; the channel is released on the cache thread.
        (*waiter.channel)->post([done = waiter.done, image = std::move(*hit)] { (*done)(image); });
        m_mailbox->post([channel = waiter.channel] { channel->reset(); });
        return;
    }
    m_mailbox->post([this, url, waiter = std::move(waiter)] mutable { requestOnThread(url, std::move(waiter)); });
}

void ImageCache::deliver(const Waiter& waiter, Result<QImage> result)
{
    (*waiter.channel)->post([done = waiter.done, result = std::move(result)] { (*done)(result); });
    waiter.channel->reset();
}

void ImageCache::requestOnThread(const QUrl& url, Waiter waiter, bool useDisk)
{
    if (std::optional<QImage> hit = cached(url, waiter.size)) {
        deliver(waiter, std::move(*hit));
        return;
    }
    const QByteArray key = nameFor(url.toEncoded());
    if (const auto pending = m_pending.find(key); pending != m_pending.end()) {
        pending->push_back(std::move(waiter));  // single flight: the running download serves this one too
        return;
    }
    if (m_disk && useDisk) {
        if (Result<QByteArray> data = m_disk->get(key)) {
            decode(url, *data, {std::move(waiter)}, Source::Disk);
            return;
        }
    }
    m_pending[key].push_back(std::move(waiter));
    net::Request request;
    request.url = url;
    request.cache = net::CacheMode::Bypass;  // stored here, not in the HTTP cache
    ++m_networkRequests;
    net::Callbacks callbacks;
    callbacks.onFinished = [this, key, url](Result<net::Response> result) {
        onDownloaded(key, url, std::move(result));
    };
    m_transport.send(std::move(request), this, std::move(callbacks));
}

void ImageCache::onDownloaded(const QByteArray& key, const QUrl& url, Result<net::Response> result)
{
    std::vector<Waiter> waiters = m_pending.take(key);
    QByteArray data;
    Source source = Source::Network;
    if (result && result->successful() && !result->body.isEmpty() && result->body.size() <= kMaxImageBytes) {
        data = result->body;
    } else if (m_fallback) {
        if (std::optional<QByteArray> stored = m_fallback(url)) {
            data = std::move(*stored);
            source = Source::Fallback;
        }
    }
    if (data.isEmpty()) {
        const Error error = result ? Error{ErrorCode::HttpError, "image request failed"} : result.error();
        for (const Waiter& waiter : waiters) {
            deliver(waiter, std::unexpected(error));
        }
        return;
    }
    decode(url, data, std::move(waiters), source);
}

void ImageCache::decode(const QUrl& url, const QByteArray& data, std::vector<Waiter> waiters, Source source)
{
    (void)QtConcurrent::run([this, mailbox = m_mailbox, url, data, source, waiters = std::move(waiters)] mutable {
        // One decode per distinct size among the waiters.
        QHash<QByteArray, QImage> decoded;
        for (const Waiter& waiter : waiters) {
            const QByteArray key = memoryKey(url, waiter.size);
            if (!decoded.contains(key)) {
                decoded.insert(key, decodeImage(data, waiter.size));
            }
        }
        mailbox->post([this, url, data, source, decoded = std::move(decoded), waiters = std::move(waiters)] {
            const bool usable = std::ranges::any_of(decoded, [](const QImage& image) { return !image.isNull(); });
            const QByteArray diskKey = nameFor(url.toEncoded());
            if (!usable && source == Source::Disk) {
                // A stored blob that does not decode (damaged, or a block page an older version kept): drop it and
                // download again instead of failing from disk in every session.
                if (m_disk) {
                    m_disk->remove(diskKey);
                }
                for (const Waiter& waiter : waiters) {
                    requestOnThread(url, waiter, false);
                }
                return;
            }
            if (usable && source == Source::Network && m_disk) {
                // Stored only once it decodes: a captive portal's HTML page must not become the cover for good.
                if (auto stored = m_disk->put(diskKey, data); !stored) {
                    qCDebug(lcCache) << "image not stored:" << stored.error().describe();
                }
            }
            for (const Waiter& waiter : waiters) {
                const QByteArray key = memoryKey(url, waiter.size);
                const QImage image = decoded.value(key);
                if (image.isNull()) {
                    deliver(waiter, fail(ErrorCode::ParseError, "not a decodable image"));
                    continue;
                }
                insertMemory(key, image);
                deliver(waiter, image);
            }
        });
    });
}

}  // namespace noctara::cache
