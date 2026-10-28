// The audio cache over the real network stack (curl transport, resolver, mock HTTP origin with byte ranges):
// a second playback and a backward seek are served without a single request, and a DRM-flagged track never
// reaches the cache. Plus measurements of the RAM tier's hot path (skipped by default).
#include "TestUtil.hpp"
#include "mock/MockServers.hpp"

#include "cache/CacheService.hpp"
#include "cache/FileOps.hpp"
#include "cache/Hash.hpp"
#include "cache/MemoryTier.hpp"
#include "core/UniqueFd.hpp"
#include "net/CurlTransport.hpp"
#include "net/DnsResolver.hpp"
#include "net/ProxyRouter.hpp"
#include "net/SystemResolver.hpp"
#include "settings/SettingsStore.hpp"

#include <QDir>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <algorithm>
#include <fcntl.h>
#include <thread>

using namespace noctara;
using namespace noctara::cache;
using namespace std::chrono_literals;

// Mock replies name only the fields that matter.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace {

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QRandomGenerator generator(seed);
    QByteArray data(size, Qt::Uninitialized);
    for (char& c : data) {
        c = static_cast<char>(generator.bounded(256));
    }
    return data;
}

// An origin that honours single byte ranges, like a CDN.
test::HttpReply serveRange(const QByteArray& content, const test::HttpRequest& request)
{
    const auto it = request.headers.find("range");
    if (it == request.headers.end()) {
        return {.status = 200, .headers = {{"etag", "\"v\""}}, .body = content};
    }
    const QByteArray spec = it->second.sliced(6);  // "bytes="
    const qsizetype dash = spec.indexOf('-');
    const qint64 first = spec.first(dash).toLongLong();
    const qint64 last = dash + 1 < spec.size() ? spec.sliced(dash + 1).toLongLong() : content.size() - 1;
    const QByteArray range = "bytes " + QByteArray::number(first) + '-' + QByteArray::number(last) + '/' +
                             QByteArray::number(content.size());
    return {.status = 206,
            .headers = {{"content-range", range}, {"etag", "\"v\""}},
            .body = content.sliced(first, last - first + 1)};
}

struct Stack {
    net::SystemResolver system;
    net::DnsResolver resolver{system};
    net::ProxyRouter router{{}, nullptr};
    net::CurlTransport transport{resolver, router};

    Stack()
    {
        net::ProxyConfig proxy;
        proxy.mode = net::ProxyMode::None;
        router.setConfig(proxy);
    }
};

TrackInfo track(const QString& id)
{
    TrackInfo info;
    info.key = TrackKey{id, QStringLiteral("progressive/mp3/sq")};
    info.title = QStringLiteral("Integration ") + id;
    return info;
}

}  // namespace

TEST_SUITE("cache.integration")
{
    TEST_CASE("replay and backward seek come from the cache with zero requests; DRM never writes")
    {
        const QByteArray content = randomBytes(700'000, 7);
        test::HttpServer origin([&](const test::HttpRequest& request) { return serveRange(content, request); });
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/track.mp3?signature=abc").arg(origin.port()));
        Stack stack;
        QTemporaryDir dir;
        settings::SettingsStore store(settings::Schema::builtin());
        CacheService service(store, stack.transport, dir.filePath(QStringLiteral("cache")));
        REQUIRE(service.diskAvailable());

        const auto open = [&](const TrackInfo& info) {
            QObject context;
            std::optional<Result<AudioReaderPtr>> reader;
            service.audio().open(
                info, [url](bool, UrlCallback done) { done(url); }, &context,
                [&](Result<AudioReaderPtr> r) { reader = std::move(r); });
            REQUIRE(test::waitUntil([&] { return reader.has_value(); }));
            return *reader;
        };
        // The "decoder": reads the stream sequentially on its own thread, like StreamSource will.
        const auto play = [&](const AudioReaderPtr& reader, std::int64_t from) {
            std::optional<Result<QByteArray>> out;
            std::atomic<bool> done{false};
            std::jthread decoder([&] {
                out = reader->readBytes(from, content.size() - from, {}, 10s);
                done = true;
            });
            REQUIRE(test::waitUntil([&] { return done.load(); }, 15000));
            decoder.join();  // `out` is read only after the thread is gone
            REQUIRE(*out);
            return **out;
        };

        {
            const Result<AudioReaderPtr> first = open(track(QStringLiteral("int")));
            REQUIRE(first);
            CHECK(play(*first, 0) == content);
        }
        const std::size_t afterFirst = origin.requests().size();
        CHECK(afterFirst >= 1);
        {
            const Result<AudioReaderPtr> second = open(track(QStringLiteral("int")));
            REQUIRE(second);
            CHECK(play(*second, 0) == content);
            CHECK(play(*second, 400'000) == content.sliced(400'000));  // seek forward …
            CHECK(play(*second, 10'000) == content.sliced(10'000));    // … and back
        }
        CHECK(origin.requests().size() == afterFirst);
        CHECK(stack.transport.unexpectedCurlResolves() == 0);

        TrackInfo protectedTrack = track(QStringLiteral("drm"));
        protectedTrack.drm = true;
        CHECK(test::errorOf(open(protectedTrack)) == ErrorCode::NotCacheable);
        CHECK(origin.requests().size() == afterFirst);
        std::vector<TrackInfo> cached;
        service.runOnCacheThread([&] { cached = service.audio().cachedTracks(); });
        REQUIRE(cached.size() == 1);
        CHECK(cached.front().key.trackId == QStringLiteral("int"));
    }
}

// Numbers for docs/performance.md (run in release with --no-skip -tc="cache benchmark*").
TEST_CASE("cache benchmark" * doctest::skip())
{
    MemoryTier tier(256LL << 20);
    const QByteArray piece(static_cast<qsizetype>(PieceMap::kChunkSize), 'x');
    constexpr int kPieces = 2048;  // 128 MiB of 64 KiB pieces
    tier.setActive(1, true);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < kPieces; ++i) {
        tier.insert({1, 1, static_cast<std::uint32_t>(i)}, piece);
    }
    const double insertNs = static_cast<double>(timer.nsecsElapsed()) / kPieces;

    constexpr int kLookups = 1'000'000;
    QRandomGenerator generator(1);
    std::vector<std::uint32_t> order(kLookups);
    for (std::uint32_t& value : order) {
        value = generator.bounded(static_cast<std::uint32_t>(kPieces));
    }
    std::int64_t bytes = 0;
    timer.restart();
    for (const std::uint32_t index : order) {
        bytes += tier.find({1, 1, index})->size();
    }
    const double hitNs = static_cast<double>(timer.nsecsElapsed()) / kLookups;
    CHECK(bytes == static_cast<std::int64_t>(kLookups) * PieceMap::kChunkSize);

    // Eviction under a full budget: every insert pushes one piece out.
    MemoryTier small(64LL << 20);
    timer.restart();
    for (int i = 0; i < 20'000; ++i) {
        small.insert({2, 1, static_cast<std::uint32_t>(i)}, piece);
    }
    const double evictNs = static_cast<double>(timer.nsecsElapsed()) / 20'000;

    // Disk hit: pread + checksum of one 64 KiB piece from the page cache.
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("data"));
    QFile out(file);
    REQUIRE(out.open(QIODevice::WriteOnly));
    for (int i = 0; i < 256; ++i) {
        out.write(piece);
    }
    out.close();
    const UniqueFd fd(::open(QFile::encodeName(file).constData(), O_RDONLY | O_CLOEXEC));
    std::vector<double> samples;
    for (int i = 0; i < 2000; ++i) {
        timer.restart();
        const Result<QByteArray> data = readAt(fd.get(), (i % 256) * PieceMap::kChunkSize, PieceMap::kChunkSize);
        (void)checksum(*data);
        samples.push_back(static_cast<double>(timer.nsecsElapsed()) / 1000.0);
    }
    std::ranges::sort(samples);
    MESSAGE("RAM tier: insert " << insertNs << " ns, hit " << hitNs << " ns, insert+evict " << evictNs
                                << " ns; disk hit (64 KiB pread + XXH3) p50 " << samples[samples.size() / 2]
                                << " us, p99 " << samples[samples.size() * 99 / 100] << " us");
}
