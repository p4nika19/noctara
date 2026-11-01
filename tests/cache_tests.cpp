// The audio, image and HTTP caches against an in-process origin (tests/mock/FakeOrigin.hpp): sidecar-first
// entries, RAM and disk tiers, single flight, resume, URL changes and expiry, corruption and ENOSPC recovery,
// eviction, covers, tagging, HLS, the DRM policy, and the service's settings wiring.
#include "TestUtil.hpp"
#include "mock/FakeOrigin.hpp"

#include "cache/AudioCache.hpp"
#include "cache/AudioPayload.hpp"
#include "cache/CacheService.hpp"
#include "cache/FileOps.hpp"
#include "cache/HttpCache.hpp"
#include "cache/ImageCache.hpp"
#include "cache/Prefetch.hpp"
#include "cache/Tagger.hpp"
#include "core/FileIo.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <thread>

using namespace noctara;
using namespace noctara::cache;
using namespace std::chrono_literals;

// Fake resources name only the fields that matter to each test; the others keep their defaults.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace {

constexpr auto kTrackUrl = "https://cdn.test/track.mp3?sig=1";
constexpr auto kOtherUrl = "https://cdn2.test/track.mp3?sig=2";

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QRandomGenerator generator(seed);
    QByteArray data(size, Qt::Uninitialized);
    for (char& c : data) {
        c = static_cast<char>(generator.bounded(256));
    }
    return data;
}

QByteArray pngCover(QColor color)
{
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

TrackInfo track(const QString& id, StreamProtocol protocol = StreamProtocol::Progressive)
{
    TrackInfo info;
    info.key = TrackKey{id, protocol == StreamProtocol::Hls ? QStringLiteral("hls/opus/sq")
                                                            : QStringLiteral("progressive/mp3/sq")};
    info.protocol = protocol;
    info.title = QStringLiteral("Трек ") + id;
    info.artist = QStringLiteral("Артист");
    info.genre = QStringLiteral("Ambient");
    info.durationMs = 2000;
    info.permalink = QUrl(QStringLiteral("https://soundcloud.com/a/") + id);
    info.sourceUpdatedAt = QStringLiteral("2024-01-01T00:00:00Z");
    info.codec = QStringLiteral("mp3");
    return info;
}

// Writes fail with ENOSPC on demand.
class FlakyDisk : public FileOps {
public:
    Result<void> writeAt(int fd, QByteArrayView data, std::int64_t offset) override
    {
        if (full) {
            ++refused;
            return fail(ErrorCode::NoSpace, "no space left on device");
        }
        return FileOps::writeAt(fd, data, offset);
    }
    std::atomic<bool> full{false};
    std::atomic<int> refused{0};
};

struct Audio {
    QTemporaryDir dir;
    test::FakeOrigin origin;
    MemoryTier memory{64LL << 20};
    FileOps* ops;
    CacheLayout layout;
    AudioCacheConfig config;
    std::unique_ptr<AudioCache> cache;
    int urlCalls = 0;
    int refreshCalls = 0;

    explicit Audio(FileOps& fileOps = FileOps::posix())
        : ops(&fileOps)
        , layout(*prepareLayout(dir.filePath(QStringLiteral("cache"))))
    {
        config.embedTags = false;
        start();
    }

    void start()
    {
        cache = std::make_unique<AudioCache>(origin, memory, *ops);
        cache->setLayout(layout);
        cache->setConfig(config);
        cache->loadIndex();
    }
    // A new process: nothing in RAM, the disk index is reloaded.
    void restart()
    {
        cache.reset();
        memory.clear();
        start();
    }

    UrlProvider urls(QString first, QString refreshed = {})
    {
        return [this, first, refreshed](bool refresh, UrlCallback done) {
            ++urlCalls;
            refreshCalls += refresh ? 1 : 0;
            done(QUrl(refresh && !refreshed.isEmpty() ? refreshed : first));
        };
    }

    Result<AudioReaderPtr> tryOpen(const TrackInfo& info, UrlProvider provider, bool background = false)
    {
        QObject context;
        std::optional<Result<AudioReaderPtr>> out;
        cache->open(
            info, std::move(provider), &context, [&](Result<AudioReaderPtr> r) { out = std::move(r); }, background);
        REQUIRE(test::waitUntil([&] { return out.has_value(); }));
        return *out;
    }
    AudioReaderPtr open(const TrackInfo& info, const QString& url = QString::fromLatin1(kTrackUrl))
    {
        Result<AudioReaderPtr> reader = tryOpen(info, urls(url));
        REQUIRE(reader);
        return *reader;
    }

    static AudioReader::Piece piece(AudioReader& reader, int index)
    {
        AudioReader::Piece result;
        REQUIRE(test::waitUntil(
            [&] {
                result = reader.tryRead(index);
                return result.status != AudioReader::Status::Pending;
            },
            5000));
        return result;
    }
    static QByteArray readAll(AudioReader& reader)
    {
        QByteArray out;
        for (int i = 0; reader.pieceCount() < 0 || i < reader.pieceCount(); ++i) {
            const AudioReader::Piece p = piece(reader, i);
            if (p.status != AudioReader::Status::Ready) {
                FAIL_CHECK("piece " << i << " failed: " << p.error.describe());
                break;
            }
            out += p.data;
        }
        return out;
    }

    [[nodiscard]] QString entryDir(const TrackInfo& info) const
    {
        const QByteArray name = info.key.name();
        return layout.audio() + u'/' + QString::fromLatin1(name.first(2)) + u'/' + QString::fromLatin1(name);
    }
    void settle() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
};

// A stream URL that arrives only when the test says so, like one resolved through an API round trip.
struct DeferredUrl {
    std::vector<UrlCallback> pending;

    UrlProvider provider()
    {
        return [this](bool, UrlCallback done) {
            pending.push_back(std::move(done));
        };
    }
    void resolve(const QString& url)
    {
        for (const UrlCallback& done : std::exchange(pending, {})) {
            done(QUrl(url));
        }
    }
};

}  // namespace

TEST_SUITE("cache.audio")
{
    TEST_CASE("the sidecar is written before the first audio byte")
    {
        Audio audio;
        const QByteArray content = randomBytes(200'000, 1);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content, .etag = "\"v1\""});
        audio.origin.setHold(true);
        const TrackInfo info = track(QStringLiteral("1"));
        AudioReaderPtr reader = audio.open(info);
        const QString dir = audio.entryDir(info);
        CHECK(reader->tryRead(0).status == AudioReader::Status::Pending);
        audio.settle();
        CHECK(QFileInfo::exists(dir + QStringLiteral("/meta.json")));
        CHECK_FALSE(QFileInfo::exists(dir + QStringLiteral("/data.part")));
        const Result<TrackInfo> sidecar = TrackInfo::fromJson(*readWholeFile(dir + QStringLiteral("/meta.json")));
        REQUIRE(sidecar);
        CHECK(sidecar->title == info.title);
        CHECK_FALSE(sidecar->complete);

        audio.origin.setHold(false);
        CHECK(Audio::readAll(*reader) == content);
        CHECK(reader->complete());
        REQUIRE(test::waitUntil([&] { return QFileInfo::exists(dir + QStringLiteral("/data")); }));
        CHECK_FALSE(QFileInfo::exists(dir + QStringLiteral("/data.part")));
        const Result<TrackInfo> done = TrackInfo::fromJson(*readWholeFile(dir + QStringLiteral("/meta.json")));
        CHECK(done->complete);
        CHECK(done->contentLength == content.size());
        CHECK(done->mediaEtag == "\"v1\"");
    }

    TEST_CASE("replay and backward seeks come from RAM and disk without network requests")
    {
        Audio audio;
        const QByteArray content = randomBytes(300'000, 2);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        const TrackInfo info = track(QStringLiteral("2"));
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::readAll(*reader) == content);
            const int requests = audio.origin.count();
            const std::int64_t hits = audio.memory.stats().hits;
            CHECK(Audio::piece(*reader, 0).data == content.first(PieceMap::kChunkSize));  // seek back
            CHECK(audio.memory.stats().hits == hits + 1);
            CHECK(audio.origin.count() == requests);
        }
        audio.restart();  // a new process: RAM is empty, the disk has everything
        audio.origin.reset();
        AudioReaderPtr again = audio.open(info, QStringLiteral("https://cdn.test/expired-url-unused"));
        CHECK(again->complete());
        CHECK(again->contentLength() == content.size());
        CHECK(Audio::readAll(*again) == content);
        Result<QByteArray> middle = again->readBytes(100'000, 1000, {}, 1s);
        audio.settle();
        CHECK(audio.origin.count() == 0);
        CHECK(audio.urlCalls == 1);  // only the first session needed a URL
    }

    TEST_CASE("concurrent readers of the same range share one request")
    {
        Audio audio;
        const QByteArray content = randomBytes(100'000, 3);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        audio.origin.setHold(true);
        const TrackInfo info = track(QStringLiteral("3"));
        AudioReaderPtr a = audio.open(info);
        AudioReaderPtr b = audio.open(info);
        CHECK(a->tryRead(0).status == AudioReader::Status::Pending);
        CHECK(b->tryRead(0).status == AudioReader::Status::Pending);
        CHECK(a->tryRead(1).status == AudioReader::Status::Pending);
        audio.settle();
        audio.origin.setHold(false);
        CHECK(Audio::piece(*b, 0).data == Audio::piece(*a, 0).data);
        CHECK(Audio::readAll(*a) == content);
        CHECK(audio.origin.count() == 1);
    }

    TEST_CASE("blocking reads from several threads at once")
    {
        Audio audio;
        const QByteArray content = randomBytes(640 * 1024, 4);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        AudioReaderPtr reader = audio.open(track(QStringLiteral("4")));
        std::atomic<int> done{0};
        std::atomic<int> wrong{0};
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&, t] {
                for (int i = t; i < 10; i += 4) {
                    const Result<QByteArray> data = reader->read(i, {}, 5s);
                    if (!data || *data != content.sliced(i * PieceMap::kChunkSize, PieceMap::kChunkSize)) {
                        ++wrong;
                    }
                }
                ++done;
            });
        }
        REQUIRE(test::waitUntil([&] { return done == 4; }, 10000));
        CHECK(wrong == 0);
    }

    TEST_CASE("a partial entry resumes where it stopped, from a new URL, under the same key")
    {
        Audio audio;
        const QByteArray content = randomBytes(400'000, 5);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content, .etag = "\"same\""});
        audio.origin.serve(QString::fromLatin1(kOtherUrl), {.body = content, .etag = "\"same\""});
        audio.origin.failAfter(100'000);  // the connection drops after 100 kB, every time
        const TrackInfo info = track(QStringLiteral("5"));
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::piece(*reader, 0).status == AudioReader::Status::Ready);
            // Each attempt resumes after the last stored piece (retried with backoff, as long as it progresses).
            REQUIRE(test::waitUntil([&] { return audio.origin.count() >= 3; }, 8000));
            audio.origin.setHold(true);
            CHECK_FALSE(reader->complete());
        }
        audio.settle();
        audio.restart();
        audio.origin.failAfter(std::nullopt);
        audio.origin.reset();
        AudioReaderPtr reader = audio.open(info, QString::fromLatin1(kOtherUrl));
        CHECK_FALSE(reader->complete());
        CHECK(reader->cachedRanges().front().first == 0);
        audio.origin.setHold(false);
        CHECK(Audio::readAll(*reader) == content);
        const std::vector<net::Request> requests = audio.origin.requests();
        REQUIRE_FALSE(requests.empty());
        CHECK(requests.front().url == QUrl(QString::fromLatin1(kOtherUrl)));
        REQUIRE(requests.front().range.has_value());
        CHECK(requests.front().range->first >= 2 * PieceMap::kChunkSize);  // nothing fetched twice
        CHECK(audio.entryDir(info) == audio.entryDir(track(QStringLiteral("5"))));
    }

    TEST_CASE("an expired URL is refreshed once and the download resumes")
    {
        Audio audio;
        const QByteArray content = randomBytes(150'000, 6);
        audio.origin.forbid(QString::fromLatin1(kTrackUrl));
        audio.origin.serve(QString::fromLatin1(kOtherUrl), {.body = content});
        Result<AudioReaderPtr> reader = audio.tryOpen(
            track(QStringLiteral("6")), audio.urls(QString::fromLatin1(kTrackUrl), QString::fromLatin1(kOtherUrl)));
        REQUIRE(reader);
        CHECK(Audio::readAll(**reader) == content);
        CHECK(audio.refreshCalls == 1);
    }

    TEST_CASE("a stream that keeps failing reports the error instead of hanging")
    {
        Audio audio;
        audio.origin.setOffline(true);
        AudioReaderPtr reader = audio.open(track(QStringLiteral("7")));
        const AudioReader::Piece result = Audio::piece(*reader, 0);
        CHECK(result.status == AudioReader::Status::Failed);
        CHECK(result.error.code == ErrorCode::Offline);
        // Back online: asking again retries.
        audio.origin.setOffline(false);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = QByteArray(1000, 'x')});
        CHECK(Audio::piece(*reader, 0).data == QByteArray(1000, 'x'));
    }

    TEST_CASE("changed media on the server resets the entry")
    {
        Audio audio;
        const QByteArray first = randomBytes(200'000, 8);
        const QByteArray second = randomBytes(250'000, 9);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = first, .etag = "\"a\""});
        audio.origin.failAfter(70'000);
        const TrackInfo info = track(QStringLiteral("8"));
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::piece(*reader, 0).status == AudioReader::Status::Ready);
        }
        audio.settle();
        audio.origin.failAfter(std::nullopt);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = second, .etag = "\"b\""});
        AudioReaderPtr old = audio.open(info);
        CHECK(Audio::piece(*old, 2).status == AudioReader::Status::Failed);  // its generation is gone
        AudioReaderPtr fresh = audio.open(info);
        CHECK(Audio::readAll(*fresh) == second);
        CHECK(fresh->info().mediaEtag == "\"b\"");
    }

    TEST_CASE("corrupt and truncated stored data is dropped silently and fetched again")
    {
        Audio audio;
        const QByteArray content = randomBytes(300'000, 10);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        const TrackInfo info = track(QStringLiteral("9"));
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::readAll(*reader) == content);
        }
        audio.settle();
        const QString data = audio.entryDir(info) + QStringLiteral("/data");
        {
            QFile file(data);
            REQUIRE(file.open(QIODevice::ReadWrite));
            file.seek(70'000);
            file.write("CORRUPTION");
        }
        audio.restart();
        audio.origin.reset();
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::readAll(*reader) == content);
            CHECK(audio.origin.count() >= 1);  // the damaged piece came from the network again
        }
        audio.settle();
        REQUIRE(QFile::resize(data, 100'000));
        audio.restart();
        audio.origin.reset();
        audio.origin.setHold(true);
        AudioReaderPtr reader = audio.open(info);
        CHECK_FALSE(reader->complete());
        audio.origin.setHold(false);
        CHECK(Audio::readAll(*reader) == content);
        CHECK(audio.origin.requests().front().range->first == PieceMap::kChunkSize);
    }

    TEST_CASE("a missing or damaged sidecar means the entry is not ours: it starts over")
    {
        Audio audio;
        const QByteArray content = randomBytes(70'000, 11);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        const TrackInfo info = track(QStringLiteral("10"));
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::readAll(*reader) == content);
        }
        audio.settle();
        REQUIRE(writeFileAtomic(audio.entryDir(info) + QStringLiteral("/meta.json"), "{broken"));
        audio.restart();
        audio.origin.reset();
        audio.origin.setHold(true);
        AudioReaderPtr reader = audio.open(info);
        CHECK_FALSE(reader->complete());
        audio.origin.setHold(false);
        CHECK(Audio::readAll(*reader) == content);
        CHECK(audio.origin.count() == 1);
    }

    TEST_CASE("DRM-protected tracks never produce an entry, a sidecar or a request")
    {
        Audio audio;
        TrackInfo info = track(QStringLiteral("drm"));
        info.drm = true;
        const Result<AudioReaderPtr> reader = audio.tryOpen(info, audio.urls(QString::fromLatin1(kTrackUrl)));
        CHECK(test::errorOf(reader) == ErrorCode::NotCacheable);
        audio.settle();
        CHECK_FALSE(QFileInfo::exists(audio.entryDir(info)));
        CHECK(audio.origin.count() == 0);
        CHECK(audio.urlCalls == 0);
        CHECK(audio.cache->cachedTracks().empty());
    }

    TEST_CASE("encrypted HLS is protected content: refused, and nothing stays behind")
    {
        Audio audio;
        const QString playlist = QStringLiteral("https://cdn.test/enc.m3u8");
        audio.origin.serve(playlist, {.body = "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\"\n#EXTINF:10,\n"
                                              "a.ts\n#EXT-X-ENDLIST\n"});
        const TrackInfo info = track(QStringLiteral("enc"), StreamProtocol::Hls);
        {
            AudioReaderPtr reader = audio.open(info, playlist);
            const AudioReader::Piece result = Audio::piece(*reader, 0);
            CHECK(result.status == AudioReader::Status::Failed);
            CHECK(result.error.code == ErrorCode::NotCacheable);
        }
        audio.settle();
        CHECK_FALSE(QFileInfo::exists(audio.entryDir(info)));
    }

    TEST_CASE("HLS stores the segments and a normalized playlist and replays offline")
    {
        Audio audio;
        const QString playlist = QStringLiteral("https://cdn.test/hls/list.m3u8?sig=x");
        audio.origin.serve(playlist, {.body = "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXT-X-MAP:URI=\"init.mp4\"\n"
                                              "#EXTINF:10.0,\ns1.m4s?sig=x\n#EXTINF:10.0,\ns2.m4s?sig=x\n"
                                              "#EXTINF:4.5,\ns3.m4s?sig=x\n#EXT-X-ENDLIST\n"});
        std::vector<QByteArray> pieces;
        int index = 0;
        for (const char* name : {"init.mp4", "s1.m4s?sig=x", "s2.m4s?sig=x", "s3.m4s?sig=x"}) {
            pieces.push_back(randomBytes(20'000 + index * 1000, 100 + static_cast<quint32>(index)));
            audio.origin.serve(QStringLiteral("https://cdn.test/hls/") + QLatin1StringView(name),
                               {.body = pieces.back()});
            ++index;
        }
        const TrackInfo info = track(QStringLiteral("hls"), StreamProtocol::Hls);
        {
            AudioReaderPtr reader = audio.open(info, playlist);
            for (int i = 0; i < 4; ++i) {
                CHECK(Audio::piece(*reader, i).data == pieces[static_cast<std::size_t>(i)]);
            }
            CHECK(reader->complete());
            REQUIRE(reader->playlist().has_value());
            CHECK(reader->playlist()->totalDuration() == doctest::Approx(24.5));
        }
        audio.settle();
        const QByteArray normalized = *readWholeFile(audio.entryDir(info) + QStringLiteral("/playlist.m3u8"));
        CHECK_FALSE(normalized.contains("sig="));
        audio.restart();
        audio.origin.reset();
        audio.origin.setOffline(true);
        AudioReaderPtr offline = audio.open(info, playlist);
        CHECK(offline->complete());
        for (int i = 0; i < 4; ++i) {
            CHECK(Audio::piece(*offline, i).data == pieces[static_cast<std::size_t>(i)]);
        }
        CHECK(audio.origin.count() == 0);
    }

    TEST_CASE("least recently used entries are evicted to stay within the byte cap; open ones are kept")
    {
        Audio audio;
        audio.config.diskBudget = 700'000;
        audio.cache->setConfig(audio.config);
        std::vector<QByteArray> contents;
        for (int i = 0; i < 3; ++i) {
            contents.push_back(randomBytes(300'000, 20 + static_cast<quint32>(i)));
            audio.origin.serve(QStringLiteral("https://cdn.test/%1.mp3").arg(i), {.body = contents.back()});
        }
        AudioReaderPtr pinned = audio.open(track(QStringLiteral("e0")), QStringLiteral("https://cdn.test/0.mp3"));
        CHECK(Audio::readAll(*pinned) == contents[0]);
        for (int i = 1; i < 3; ++i) {
            AudioReaderPtr reader =
                audio.open(track(QStringLiteral("e%1").arg(i)), QStringLiteral("https://cdn.test/%1.mp3").arg(i));
            CHECK(Audio::readAll(*reader) == contents[static_cast<std::size_t>(i)]);
            audio.settle();
        }
        audio.settle();
        const AudioUsage usage = audio.cache->usage();
        CHECK(usage.completeBytes + usage.partialBytes <= 700'000 + 64 * 1024);
        CHECK(QFileInfo::exists(audio.entryDir(track(QStringLiteral("e0")))));  // in use: kept
        CHECK_FALSE(QFileInfo::exists(audio.entryDir(track(QStringLiteral("e1")))));
        CHECK(QFileInfo::exists(audio.entryDir(track(QStringLiteral("e2")))));
    }

    TEST_CASE("a full disk never breaks playback: data stays in RAM and is not recorded as stored")
    {
        FlakyDisk disk;
        Audio audio(disk);
        const QByteArray content = randomBytes(200'000, 30);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        disk.full = true;
        const TrackInfo info = track(QStringLiteral("full"));
        {
            AudioReaderPtr reader = audio.open(info);
            CHECK(Audio::readAll(*reader) == content);
            CHECK(disk.refused > 0);
            CHECK(audio.cache->usage().diskStalled);
        }
        audio.settle();
        disk.full = false;
        audio.restart();
        audio.origin.setHold(true);
        AudioReaderPtr reader = audio.open(info);
        CHECK(reader->cachedRanges().empty());  // nothing was claimed as stored
        audio.origin.setHold(false);
        CHECK(Audio::readAll(*reader) == content);
    }

    TEST_CASE("covers are stored once per image, survive a failed fetch and are retried later")
    {
        Audio audio;
        const QByteArray cover = pngCover(Qt::red);
        audio.origin.serve(QStringLiteral("https://img.test/a-large.png"), {.body = cover, .type = "image/png"});
        audio.origin.serve(QStringLiteral("https://img.test/b-large.png"), {.body = cover, .type = "image/png"});
        audio.origin.serve(QStringLiteral("https://cdn.test/a.mp3"), {.body = randomBytes(1000, 40)});
        audio.origin.serve(QStringLiteral("https://cdn.test/b.mp3"), {.body = randomBytes(1000, 41)});
        TrackInfo a = track(QStringLiteral("ca"));
        a.artworkUrl = QUrl(QStringLiteral("https://img.test/a-large.png"));
        TrackInfo b = track(QStringLiteral("cb"));
        b.artworkUrl = QUrl(QStringLiteral("https://img.test/b-large.png"));
        TrackInfo c = track(QStringLiteral("cc"));
        c.artworkUrl = QUrl(QStringLiteral("https://img.test/missing.png"));
        AudioReaderPtr ra = audio.open(a, QStringLiteral("https://cdn.test/a.mp3"));
        AudioReaderPtr rb = audio.open(b, QStringLiteral("https://cdn.test/b.mp3"));
        AudioReaderPtr rc = audio.open(c, QStringLiteral("https://cdn.test/a.mp3"));
        REQUIRE(test::waitUntil([&] { return !ra->info().coverHash.isEmpty() && !rb->info().coverHash.isEmpty(); }));
        CHECK(ra->info().coverHash == rb->info().coverHash);
        CHECK(QDir(audio.layout.covers()).entryList(QDir::Files).size() == 1);
        CHECK(*audio.cache->coverFor(a.artworkUrl) == cover);

        audio.settle();
        CHECK(rc->info().coverHash.isEmpty());  // the 404 page is not stored as a cover
        CHECK(QFileInfo::exists(audio.entryDir(c) + QStringLiteral("/meta.json")));
        rc.reset();
        audio.settle();
        audio.origin.serve(QStringLiteral("https://img.test/missing.png"),
                           {.body = pngCover(Qt::blue), .type = "image/png"});
        AudioReaderPtr retry = audio.open(c, QStringLiteral("https://cdn.test/a.mp3"));
        REQUIRE(test::waitUntil([&] { return !retry->info().coverHash.isEmpty(); }));
        CHECK(QDir(audio.layout.covers()).entryList(QDir::Files).size() == 2);
    }

    TEST_CASE("finished entries are tagged; readers opened earlier keep the original bytes")
    {
        Audio audio;
        audio.config.embedTags = true;
        audio.cache->setConfig(audio.config);
        const QByteArray mp3 = *readWholeFile(QStringLiteral(NOCTARA_SOURCE_DIR "/tests/fixtures/audio/sine.mp3"));
        const QByteArray cover = pngCover(Qt::green);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = mp3});
        audio.origin.serve(QStringLiteral("https://img.test/cover.png"), {.body = cover, .type = "image/png"});
        TrackInfo info = track(QStringLiteral("tag"));
        info.title = QStringLiteral("Дождь 雨 ☔");
        info.artworkUrl = QUrl(QStringLiteral("https://img.test/cover.png"));
        AudioReaderPtr early = audio.open(info);
        CHECK(Audio::readAll(*early) == mp3);
        REQUIRE(test::waitUntil([&] { return !early->info().taggedFor.isEmpty(); }, 5000));

        const QString data = audio.entryDir(info) + QStringLiteral("/data");
        const Result<TagData> tags = readTags(data);
        REQUIRE(tags);
        CHECK(tags->title == info.title);
        CHECK(tags->artist == info.artist);
        CHECK(tags->cover == cover);
        CHECK(*payloadHashOfFile(data) == early->info().payloadHash);

        audio.memory.clear();
        CHECK(Audio::readAll(*early) == mp3);  // bound to the untagged generation
        AudioReaderPtr late = audio.open(info);
        CHECK(late->contentLength() == QFileInfo(data).size());
        CHECK(Audio::readAll(*late).startsWith("ID3"));

        // New metadata (updated_at changed): the sidecar is refreshed and the file re-tagged.
        early.reset();
        late.reset();
        audio.settle();
        info.title = QStringLiteral("Новое имя");
        info.sourceUpdatedAt = QStringLiteral("2025-01-01T00:00:00Z");
        AudioReaderPtr refreshed = audio.open(info);
        REQUIRE(test::waitUntil([&] { return refreshed->info().taggedFor.startsWith(info.sourceUpdatedAt); }, 5000));
        CHECK(readTags(data)->title == QStringLiteral("Новое имя"));
        CHECK(*payloadHashOfFile(data) == refreshed->info().payloadHash);
    }

    TEST_CASE("cached tracks describe themselves offline; clearing removes them and their covers")
    {
        Audio audio;
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = randomBytes(5000, 50)});
        audio.origin.serve(QStringLiteral("https://img.test/c.png"),
                           {.body = pngCover(Qt::yellow), .type = "image/png"});
        TrackInfo info = track(QStringLiteral("off"));
        info.artworkUrl = QUrl(QStringLiteral("https://img.test/c.png"));
        {
            AudioReaderPtr reader = audio.open(info);
            Audio::readAll(*reader);
            REQUIRE(test::waitUntil([&] { return !reader->info().coverHash.isEmpty(); }));
        }
        audio.settle();
        audio.restart();
        audio.settle();
        const std::vector<TrackInfo> tracks = audio.cache->cachedTracks();
        REQUIRE(tracks.size() == 1);
        CHECK(tracks.front().title == info.title);
        CHECK(tracks.front().artist == info.artist);
        CHECK(tracks.front().complete);
        CHECK(audio.cache->coverFor(info.artworkUrl).has_value());

        audio.cache->clear();
        CHECK(audio.cache->cachedTracks().empty());
        CHECK_FALSE(QFileInfo::exists(audio.entryDir(info)));
        CHECK(QDir(audio.layout.covers()).entryList(QDir::Files).isEmpty());
        CHECK(audio.cache->usage().completeBytes == 0);
    }

    TEST_CASE("without a disk tier entries live in RAM only and leave no files")
    {
        Audio audio;
        audio.cache->setLayout(std::nullopt);
        const QByteArray content = randomBytes(150'000, 60);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        const TrackInfo info = track(QStringLiteral("ram"));
        AudioReaderPtr reader = audio.open(info);
        CHECK(Audio::readAll(*reader) == content);
        CHECK_FALSE(QFileInfo::exists(audio.entryDir(info)));
        // Evicted from RAM: fetched again rather than failing.
        audio.memory.clear();
        audio.origin.reset();
        CHECK(Audio::piece(*reader, 1).data == content.sliced(PieceMap::kChunkSize, PieceMap::kChunkSize));
        CHECK(audio.origin.count() >= 1);
    }

    TEST_CASE("a piece wanted while the stream URL resolves starts one download, from that piece")
    {
        Audio audio;
        const QByteArray content = randomBytes(300'000, 61);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        DeferredUrl url;
        Result<AudioReaderPtr> opened = audio.tryOpen(track(QStringLiteral("deferred")), url.provider());
        REQUIRE(opened);
        AudioReader& reader = **opened;
        CHECK(reader.tryRead(2).status == AudioReader::Status::Pending);  // the voice reads before the URL is in
        audio.settle();
        REQUIRE(url.pending.size() == 1);
        url.resolve(QString::fromLatin1(kTrackUrl));
        REQUIRE(test::waitUntil([&] { return reader.complete(); }));
        audio.settle();
        const std::vector<net::Request> requests = audio.origin.requests();
        REQUIRE(requests.size() == 2);  // from the wanted piece to the end, then what lies before it
        REQUIRE(requests.front().range.has_value());
        CHECK(requests.front().range->first == 2 * PieceMap::kChunkSize);
        CHECK(Audio::readAll(reader) == content);
    }

    TEST_CASE("a reader closed while the stream URL resolves leaves no download behind")
    {
        Audio audio;
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = randomBytes(300'000, 62)});
        DeferredUrl url;
        {
            Result<AudioReaderPtr> opened = audio.tryOpen(track(QStringLiteral("skipped")), url.provider());
            REQUIRE(opened);
        }
        audio.settle();
        REQUIRE(url.pending.size() == 1);
        url.resolve(QString::fromLatin1(kTrackUrl));
        audio.settle();
        CHECK(audio.origin.count() == 0);
    }

    TEST_CASE("a limited prefetch stops at its limit, progressive and HLS")
    {
        Audio audio;
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = randomBytes(1'000'000, 63)});
        Result<AudioReaderPtr> progressive =
            audio.tryOpen(track(QStringLiteral("limited")), audio.urls(QString::fromLatin1(kTrackUrl)), true);
        REQUIRE(progressive);
        (*progressive)->prefetch(2 * PieceMap::kChunkSize);
        REQUIRE(test::waitUntil([&] { return audio.origin.count() >= 1; }));
        test::waitUntil([] { return false; }, 500);
        std::int64_t stored = 0;
        for (const auto& [from, to] : (*progressive)->cachedRanges()) {
            stored += to - from;
        }
        CHECK(stored == 2 * PieceMap::kChunkSize);
        CHECK(audio.origin.count() == 1);

        audio.origin.reset();
        const QString playlist = QStringLiteral("https://cdn.test/hls/limited.m3u8");
        audio.origin.serve(playlist, {.body = "#EXTM3U\n#EXT-X-TARGETDURATION:10\n#EXTINF:10.0,\ns1.ts\n"
                                              "#EXTINF:10.0,\ns2.ts\n#EXTINF:10.0,\ns3.ts\n#EXT-X-ENDLIST\n"});
        for (const char* name : {"s1.ts", "s2.ts", "s3.ts"}) {
            audio.origin.serve(QStringLiteral("https://cdn.test/hls/") + QLatin1StringView(name),
                               {.body = randomBytes(20'000, 64)});
        }
        Result<AudioReaderPtr> hls =
            audio.tryOpen(track(QStringLiteral("limited-hls"), StreamProtocol::Hls), audio.urls(playlist), true);
        REQUIRE(hls);
        (*hls)->prefetch(PieceMap::kChunkSize);
        REQUIRE(test::waitUntil([&] { return audio.origin.count() >= 2; }));
        test::waitUntil([] { return false; }, 500);
        CHECK(audio.origin.count() == 2);  // the playlist and the first segment
        CHECK((*hls)->tryRead(0).status == AudioReader::Status::Ready);
    }

    TEST_CASE("a piece evicted from RAM while its download still runs is fetched again at once")
    {
        Audio audio;
        audio.cache->setLayout(std::nullopt);
        const QByteArray content = randomBytes(1'000'000, 65);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        audio.origin.pauseAfter(3 * PieceMap::kChunkSize);
        AudioReaderPtr reader = audio.open(track(QStringLiteral("evicted")));
        for (int i = 0; i < 3; ++i) {
            CHECK(Audio::piece(*reader, i).status == AudioReader::Status::Ready);
        }
        audio.memory.clear();  // memory pressure while the first download is stalled after piece 2
        const AudioReader::Piece again = Audio::piece(*reader, 0);
        CHECK(again.data == content.first(PieceMap::kChunkSize));
        audio.origin.pauseAfter(std::nullopt);
    }

    TEST_CASE("an open reader keeps working when the cache moves to another location")
    {
        Audio audio;
        const QByteArray content = randomBytes(600'000, 66);
        audio.origin.serve(QString::fromLatin1(kTrackUrl), {.body = content});
        audio.origin.pauseAfter(2 * PieceMap::kChunkSize);
        const TrackInfo info = track(QStringLiteral("moved"));
        AudioReaderPtr reader = audio.open(info);
        CHECK(Audio::piece(*reader, 0).status == AudioReader::Status::Ready);
        CHECK(Audio::piece(*reader, 1).status == AudioReader::Status::Ready);
        QTemporaryDir elsewhere;
        const CacheLayout moved = *prepareLayout(elsewhere.filePath(QStringLiteral("cache")));
        audio.cache->setLayout(moved);  // the user picked another cache location
        audio.origin.pauseAfter(std::nullopt);
        CHECK(Audio::readAll(*reader) == content);
        audio.memory.clear();  // pieces stored before the move come from the old file, later ones again
        CHECK(Audio::piece(*reader, 0).data == content.first(PieceMap::kChunkSize));
        CHECK(Audio::piece(*reader, 5).data == content.sliced(5 * PieceMap::kChunkSize, PieceMap::kChunkSize));
        const QByteArray name = info.key.name();
        CHECK_FALSE(QFileInfo::exists(moved.audio() + u'/' + QString::fromLatin1(name.first(2))));
    }

    TEST_CASE("prefetch warms upcoming tracks in the background and stops when the queue changes")
    {
        Audio audio;
        for (int i = 0; i < 3; ++i) {
            audio.origin.serve(QStringLiteral("https://cdn.test/p%1.mp3").arg(i),
                               {.body = randomBytes(200'000, 70 + static_cast<quint32>(i))});
        }
        Prefetcher prefetcher(*audio.cache);
        std::vector<PrefetchItem> queue;
        for (int i = 0; i < 3; ++i) {
            TrackInfo info = track(QStringLiteral("p%1").arg(i));
            info.drm = i == 0;  // skipped: never prefetched
            queue.push_back({info, audio.urls(QStringLiteral("https://cdn.test/p%1.mp3").arg(i))});
        }
        prefetcher.setQueue(queue, 1, -1);
        REQUIRE(test::waitUntil([&] { return prefetcher.active() == 1; }));
        REQUIRE(test::waitUntil([&] { return audio.cache->usage().completeEntries == 1; }));
        for (const net::Request& request : audio.origin.requests()) {
            CHECK(request.background);
            CHECK(request.url.toString().contains(QStringLiteral("p1")));
        }
        prefetcher.cancel();
        CHECK(prefetcher.active() == 0);
    }
}

TEST_SUITE("cache.image")
{
    TEST_CASE("one download per URL, one decode per size, and a disk copy for the next session")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        const QUrl url(QStringLiteral("https://img.test/art.png"));
        QImage big(400, 200, QImage::Format_RGB32);
        big.fill(Qt::magenta);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        big.save(&buffer, "PNG");
        origin.serve(url.toString(), {.body = png, .type = "image/png"});
        QObject context;
        {
            ImageCache images(origin, FileOps::posix(), 16 << 20);
            images.setDisk(dir.path(), 1 << 20);
            origin.setHold(true);
            std::vector<QImage> results(3);
            int done = 0;
            const std::array<QSize, 3> sizes{QSize(100, 100), QSize(100, 100), QSize()};
            for (std::size_t i = 0; i < 3; ++i) {
                images.request(url, sizes[i], &context, [&, i](Result<QImage> image) {
                    REQUIRE(image);
                    results[i] = *image;
                    ++done;
                });
            }
            test::waitUntil([] { return false; }, 100);
            origin.setHold(false);
            REQUIRE(test::waitUntil([&] { return done == 3; }));
            CHECK(origin.count() == 1);
            CHECK(results[0].size() == QSize(100, 50));  // fits the box, keeps the aspect ratio
            CHECK(results[2].size() == QSize(400, 200));
            CHECK(images.cached(url, QSize(100, 100)).has_value());
        }
        origin.reset();
        origin.setOffline(true);
        ImageCache reopened(origin, FileOps::posix(), 16 << 20);
        reopened.setDisk(dir.path(), 1 << 20);
        std::optional<Result<QImage>> again;
        reopened.request(url, QSize(50, 50), &context, [&](Result<QImage> image) { again = std::move(image); });
        REQUIRE(test::waitUntil([&] { return again.has_value(); }));
        CHECK(again->has_value());
        CHECK(origin.count() == 0);
    }

    TEST_CASE("offline, stored covers of audio entries stand in")
    {
        test::FakeOrigin origin;
        origin.setOffline(true);
        ImageCache images(origin, FileOps::posix(), 16 << 20);
        const QByteArray cover = pngCover(Qt::cyan);
        images.setFallback([&](const QUrl& url) -> std::optional<QByteArray> {
            return url.host() == u"img.test" ? std::optional(cover) : std::nullopt;
        });
        QObject context;
        std::optional<Result<QImage>> result;
        images.request(QUrl(QStringLiteral("https://img.test/x.png")), {}, &context,
                       [&](Result<QImage> image) { result = std::move(image); });
        REQUIRE(test::waitUntil([&] { return result.has_value(); }));
        CHECK(result->has_value());
        result.reset();
        images.request(QUrl(QStringLiteral("https://other.test/y.png")), {}, &context,
                       [&](Result<QImage> image) { result = std::move(image); });
        REQUIRE(test::waitUntil([&] { return result.has_value(); }));
        CHECK(test::errorOf(*result) == ErrorCode::Offline);
    }
}

TEST_SUITE("cache.http")
{
    struct Http {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        CachingTransport transport{origin, FileOps::posix()};

        Http() { transport.setDisk(dir.path(), 1 << 20); }

        Result<net::Response> get(const QString& url, net::CacheMode mode = net::CacheMode::Default,
                                  std::chrono::seconds ttl = 0s, net::Headers headers = {})
        {
            net::Request request;
            request.url = QUrl(url);
            request.cache = mode;
            request.cacheTtl = ttl;
            request.headers = std::move(headers);
            return send(std::move(request));
        }
        Result<net::Response> send(net::Request request)
        {
            QObject context;
            std::optional<Result<net::Response>> out;
            net::Callbacks callbacks;
            callbacks.onFinished = [&](Result<net::Response> result) {
                out = std::move(result);
            };
            transport.send(std::move(request), &context, std::move(callbacks));
            REQUIRE(test::waitUntil([&] { return out.has_value(); }));
            return *out;
        }
    };

    TEST_CASE("fresh responses are served without the network; stale ones are revalidated")
    {
        Http http;
        const QString url = QStringLiteral("https://api.test/tracks/1");
        http.origin.serve(url, {.body = "{\"id\":1}", .etag = "\"e1\"", .headers = {{"cache-control", "max-age=60"}}});
        CHECK(http.get(url)->body == "{\"id\":1}");
        const Result<net::Response> cached = http.get(url);
        CHECK(cached->fromCache);
        CHECK(cached->body == "{\"id\":1}");
        CHECK(http.origin.count() == 1);

        // Refresh revalidates with the ETag; a 304 keeps the stored body.
        http.origin.serve(url,
                          {.body = {}, .etag = "\"e1\"", .headers = {{"cache-control", "max-age=60"}}, .status = 304});
        const Result<net::Response> revalidated = http.get(url, net::CacheMode::Refresh);
        CHECK(revalidated->status == 200);
        CHECK(revalidated->fromCache);
        CHECK(revalidated->body == "{\"id\":1}");
        REQUIRE(http.origin.count() == 2);
        bool conditional = false;
        for (const auto& [name, value] : http.origin.requests().back().headers) {
            conditional = conditional || (name == "if-none-match" && value == "\"e1\"");
        }
        CHECK(conditional);
    }

    TEST_CASE("the fallback TTL applies only without server freshness; no-store is never kept")
    {
        Http http;
        const QString feed = QStringLiteral("https://api.test/stream");
        http.origin.serve(feed, {.body = "feed"});
        (void)http.get(feed, net::CacheMode::Default, 30s);
        CHECK(http.get(feed, net::CacheMode::Default, 30s)->fromCache);
        CHECK(http.origin.count() == 1);

        const QString secret = QStringLiteral("https://api.test/me");
        http.origin.serve(secret, {.body = "me", .headers = {{"cache-control", "no-store"}}});
        (void)http.get(secret, net::CacheMode::Default, 3600s);
        CHECK_FALSE(http.get(secret, net::CacheMode::Default, 3600s)->fromCache);
        CHECK(test::errorOf(http.get(secret, net::CacheMode::OnlyIfCached)) == ErrorCode::Offline);
    }

    TEST_CASE("offline, the stale copy is served; a POST invalidates; accounts are kept apart")
    {
        Http http;
        const QString url = QStringLiteral("https://api.test/me/likes");
        const net::Headers alice{{"authorization", "OAuth alice"}};
        const net::Headers bob{{"authorization", "OAuth bob"}};
        http.origin.serve(url, {.body = "alice-likes"});
        CHECK(http.get(url, net::CacheMode::Default, 0s, alice)->body == "alice-likes");
        http.origin.setOffline(true);
        const Result<net::Response> stale = http.get(url, net::CacheMode::Default, 0s, alice);
        REQUIRE(stale);
        CHECK(stale->stale);
        CHECK(stale->body == "alice-likes");
        CHECK(test::errorOf(http.get(url, net::CacheMode::Default, 0s, bob)) == ErrorCode::Offline);

        http.origin.setOffline(false);
        net::Request post;
        post.method = net::Method::Post;
        post.url = QUrl(url);
        post.headers = alice;
        http.origin.serve(url, {.body = "ok"});
        CHECK(http.send(post)->body == "ok");
        CHECK(test::errorOf(http.get(url, net::CacheMode::OnlyIfCached, 0s, alice)) == ErrorCode::Offline);
    }
}

TEST_SUITE("cache.service")
{
    TEST_CASE("settings drive the budgets live; a second instance runs without disk caches")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        settings::SettingsStore store(settings::Schema::builtin());
        CacheService service(store, origin, dir.filePath(QStringLiteral("cache")));
        CHECK(service.diskAvailable());
        CHECK(service.memory().stats().budget == 128LL << 20);
        REQUIRE(store.setUserValue(settings::keys::AudioRamMb, 32));
        REQUIRE(test::waitUntil([&] { return service.memory().stats().budget == 32LL << 20; }));
        CHECK(service.prefetchCount(false) == 2);
        CHECK(service.prefetchCount(true) == 0);
        REQUIRE(store.setUserValue(settings::keys::AudioCacheEnabled, false));
        test::waitUntil([] { return false; }, 50);
        CHECK(service.prefetchCount(false) == 1);

        CacheService second(store, origin, dir.filePath(QStringLiteral("cache")));
        CHECK_FALSE(second.diskAvailable());
        CHECK(second.status().problem == ErrorCode::CacheLocked);

        // A custom location that cannot be used falls back to the standard one.
        REQUIRE(store.setUserValue(settings::keys::CacheDir, QStringLiteral("/proc/no-such-place")));
        REQUIRE(test::waitUntil([&] { return service.status().fellBack; }));
        CHECK(service.diskAvailable());
    }

    TEST_CASE("a second instance keeps its own files out of the cache root the first one holds")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        settings::SettingsStore store(settings::Schema::builtin());
        const QString root = dir.filePath(QStringLiteral("cache"));
        CacheService first(store, origin, root);
        REQUIRE(first.diskAvailable());
        CHECK(first.privateRoot() == first.status().root);

        QString elsewhere;
        {
            CacheService second(store, origin, root);
            REQUIRE(second.status().problem == ErrorCode::CacheLocked);
            elsewhere = second.privateRoot();
            REQUIRE_FALSE(elsewhere.isEmpty());
            CHECK(QFileInfo(elsewhere).isDir());
            CHECK(QDir(root).relativeFilePath(elsewhere).startsWith(QStringLiteral("..")));
            CHECK(second.privateRoot() == elsewhere);
        }
        CHECK_FALSE(QFileInfo::exists(elsewhere));
    }

    TEST_CASE("clear on exit empties the caches when the service shuts down")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        origin.serve(QString::fromLatin1(kTrackUrl), {.body = randomBytes(10'000, 80)});
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::ClearCacheOnExit, true));
        const QString root = dir.filePath(QStringLiteral("cache"));
        {
            CacheService service(store, origin, root);
            QObject context;
            std::optional<Result<AudioReaderPtr>> reader;
            service.audio().open(
                track(QStringLiteral("x")), [](bool, UrlCallback done) { done(QUrl(QString::fromLatin1(kTrackUrl))); },
                &context, [&](Result<AudioReaderPtr> r) { reader = std::move(r); });
            REQUIRE(test::waitUntil([&] { return reader.has_value(); }));
            REQUIRE(*reader);
            std::optional<Result<QByteArray>> data;
            std::atomic<bool> done{false};
            std::jthread blocking([&] {
                data = (**reader)->read(0, {}, 5s);
                done = true;
            });
            REQUIRE(test::waitUntil([&] { return done.load(); }));
            blocking.join();
            REQUIRE(*data);
            reader.reset();
            std::optional<CacheUsage> usage;
            REQUIRE(test::waitUntil([&] {
                service.usage(&context, [&](CacheUsage u) { usage = u; });
                test::waitUntil([&] { return usage.has_value(); });
                const bool stored = usage->audio.completeBytes > 0;
                usage.reset();
                return stored;
            }));
        }
        CHECK(QDir(root + QStringLiteral("/audio")).entryList(QDir::Dirs | QDir::NoDotAndDotDot) ==
              QStringList{QStringLiteral("covers")});
    }
}
