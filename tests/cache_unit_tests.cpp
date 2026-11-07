// Pure parts of the cache layer: piece maps, LRU accounting, the RAM tier, HLS playlists, HTTP freshness rules,
// tag-independent payload hashes, tagging with TagLib, and the cache directory layout and lock.
#include "TestUtil.hpp"

#include "cache/AudioPayload.hpp"
#include "cache/CacheLayout.hpp"
#include "cache/DiskLru.hpp"
#include "cache/FileOps.hpp"
#include "cache/Hls.hpp"
#include "cache/HttpCache.hpp"
#include "cache/LruLedger.hpp"
#include "cache/MemoryTier.hpp"
#include "cache/PieceMap.hpp"
#include "cache/Prefetch.hpp"
#include "cache/Tagger.hpp"
#include "cache/TrackInfo.hpp"
#include "core/FileIo.hpp"
#include "core/Paths.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <sys/stat.h>

using namespace noctara;
using namespace noctara::cache;

namespace {

QString fixture(const char* name)
{
    return QStringLiteral(NOCTARA_SOURCE_DIR "/tests/fixtures/audio/") + QLatin1StringView(name);
}

QByteArray pngCover(QColor color, int side = 64)
{
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// Fails copies (as a full disk would) but behaves like POSIX otherwise.
class FullDisk : public FileOps {
public:
    Result<void> copyFile(const QString& /*from*/, const QString& to) override
    {
        QFile partial(to);
        if (partial.open(QIODevice::WriteOnly)) {
            partial.write("half a copy");
        }
        return fail(ErrorCode::NoSpace, "no space left on device");
    }
    Result<void> writeFile(const QString& path, QByteArrayView data) override
    {
        if (failWrites) {
            return fail(ErrorCode::NoSpace, "no space left on device");
        }
        return FileOps::writeFile(path, data);
    }
    bool failWrites = false;
};

mode_t modeOf(const QString& path)
{
    struct stat info{};
    ::stat(QFile::encodeName(path).constData(), &info);
    return info.st_mode & 0777U;
}

}  // namespace

TEST_SUITE("cache.pieces")
{
    TEST_CASE("progressive maps cut the stream into fixed chunks with a short last one")
    {
        PieceMap map = PieceMap::progressive(150 * 1024, 64 * 1024);
        CHECK(map.pieceCount() == 3);
        CHECK(map.pieceSize(0) == 64 * 1024);
        CHECK(map.pieceSize(2) == 22 * 1024);
        CHECK(map.pieceAt(130 * 1024) == 2);
        CHECK(map.firstMissing() == 0);
        map.mark(1, 64 * 1024, 7);
        CHECK(map.firstMissing() == 0);
        CHECK(map.runEnd(0) == 1);
        CHECK(map.firstMissing(1) == 2);
        CHECK(map.runEnd(2) == 3);
        map.mark(0, 64 * 1024, 1);
        map.mark(2, 22 * 1024, 2);
        CHECK(map.complete());
        CHECK(map.storedBytes() == 150 * 1024);
        CHECK(map.ranges() == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 150 * 1024}});
    }

    TEST_CASE("an unknown length grows with the stored pieces and is never complete")
    {
        PieceMap map = PieceMap::progressive(-1);
        CHECK(map.pieceCount() == -1);
        map.mark(3, 100, 1);
        CHECK_FALSE(map.complete());
        CHECK(map.firstMissing(3) == 4);
        map.setContentLength(3 * PieceMap::kChunkSize + 100);
        CHECK(map.pieceCount() == 4);
        CHECK(map.has(3));
        CHECK(map.ranges().front().first == 3 * PieceMap::kChunkSize);
    }

    TEST_CASE("an HLS map is complete once every segment is stored, never before its playlist is known")
    {
        CHECK_FALSE(PieceMap::hls(0).complete());
        PieceMap map = PieceMap::hls(2);
        map.mark(0, 100, 1);
        CHECK_FALSE(map.complete());
        map.mark(1, 50, 2);
        CHECK(map.complete());
    }

    TEST_CASE("serialization round-trips and detects damage")
    {
        PieceMap map = PieceMap::progressive(1'000'000);
        map.mark(0, static_cast<std::uint32_t>(PieceMap::kChunkSize), 42);
        map.mark(7, static_cast<std::uint32_t>(PieceMap::kChunkSize), 43);
        QByteArray data = map.serialize();
        const Result<PieceMap> parsed = PieceMap::parse(data);
        REQUIRE(parsed);
        CHECK(parsed->has(0));
        CHECK(parsed->has(7));
        CHECK_FALSE(parsed->has(1));
        CHECK(parsed->checksum(7) == 43);
        CHECK(parsed->contentLength() == 1'000'000);
        data[30] = static_cast<char>(data[30] ^ 0x5A);
        CHECK(test::errorOf(PieceMap::parse(data)) == ErrorCode::Corrupt);
        CHECK(test::errorOf(PieceMap::parse(data.first(10))) == ErrorCode::Corrupt);
    }
}

TEST_SUITE("cache.lru")
{
    TEST_CASE("victims are the oldest unpinned items, never the kept one")
    {
        LruLedger ledger;
        ledger.upsert("aa", 100, 10);
        ledger.upsert("bb", 100, 5);
        ledger.upsert("cc", 100, 20);
        ledger.upsert("dd", 100, 1);
        ledger.pin("dd");
        CHECK(ledger.totalBytes() == 400);
        CHECK(ledger.victims(150) == std::vector<QByteArray>{"bb", "aa"});
        CHECK(ledger.victims(150, "bb") == std::vector<QByteArray>{"aa", "cc"});
        CHECK(ledger.overBudget(250) == std::vector<QByteArray>{"bb", "aa"});
        CHECK(ledger.overBudget(400).empty());
        ledger.unpin("dd");
        CHECK(ledger.victims(1).front() == "dd");
        ledger.touch("dd", 99);
        CHECK(ledger.victims(1).front() == "bb");
        ledger.addBytes("aa", 50);
        ledger.remove("cc");
        CHECK(ledger.totalBytes() == 350);
    }
}

TEST_SUITE("cache.memory")
{
    TEST_CASE("the RAM tier stays within its budget and evicts inactive entries first")
    {
        MemoryTier tier(4 * 1000);
        const QByteArray piece(1000, 'x');
        tier.setActive(1, true);
        tier.insert({1, 1, 0}, piece);  // active
        tier.insert({1, 1, 1}, piece);  // active
        tier.insert({2, 1, 0}, piece);  // inactive
        tier.insert({2, 1, 1}, piece);  // inactive
        CHECK(tier.stats().bytes == 4000);
        CHECK(tier.find({1, 1, 0}).has_value());
        tier.insert({1, 1, 2}, piece);
        CHECK(tier.stats().bytes == 4000);
        CHECK_FALSE(tier.contains({2, 1, 0}));  // the least recently used inactive piece went first
        CHECK(tier.contains({1, 1, 0}));
        CHECK(tier.contains({1, 1, 1}));

        // A hit shares the buffer instead of copying it.
        const QByteArray hit = *tier.find({1, 1, 2});
        CHECK(hit.isSharedWith(*tier.find({1, 1, 2})));

        tier.setActive(1, false);
        tier.setActive(2, true);
        tier.insert({2, 1, 5}, piece);
        CHECK(tier.contains({2, 1, 1}));
        CHECK(tier.stats().pieces == 4);

        tier.trimTo(1000);
        CHECK(tier.stats().bytes <= 1000);
        tier.dropEntry(2);
        CHECK(tier.stats().bytes == 0);

        tier.insert({3, 1, 0}, QByteArray(5000, 'y'));  // larger than the whole budget: refused
        CHECK(tier.stats().bytes == 0);
        tier.setBudget(10'000);
        tier.insert({3, 1, 0}, QByteArray(5000, 'y'));
        tier.setBudget(1000);
        CHECK(tier.stats().bytes == 0);
    }
}

TEST_SUITE("cache.hls")
{
    TEST_CASE("a VOD media playlist with an init segment and byte ranges")
    {
        const QByteArray text = "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:10\n"
                                "#EXT-X-MAP:URI=\"init.mp4\"\n#EXT-X-KEY:METHOD=NONE\n"
                                "#EXTINF:9.98,\nseg1.m4s?sig=abc\n"
                                "#EXT-X-BYTERANGE:1000@0\n#EXTINF:10.0,\nall.m4s\n"
                                "#EXT-X-BYTERANGE:500\n#EXTINF:3.5,\nall.m4s\n#EXT-X-ENDLIST\n";
        const Result<HlsPlaylist> playlist = parseHls(text, QUrl(QStringLiteral("https://cdn.test/a/b/list.m3u8")));
        REQUIRE(playlist);
        CHECK(playlist->pieceCount() == 4);
        CHECK(playlist->piece(0).url == QUrl(QStringLiteral("https://cdn.test/a/b/init.mp4")));
        CHECK(playlist->piece(1).url.query() == QStringLiteral("sig=abc"));
        CHECK(playlist->piece(2).rangeOffset == 0);
        CHECK(playlist->piece(3).rangeOffset == 1000);
        CHECK(playlist->piece(3).rangeLength == 500);
        CHECK(playlist->totalDuration() == doctest::Approx(23.48));

        // The normalized copy has no remote URLs but the same structure.
        const QByteArray normalized = playlist->normalized();
        CHECK_FALSE(normalized.contains("cdn.test"));
        CHECK_FALSE(normalized.contains("sig="));
        const Result<HlsPlaylist> local = parseHls(normalized, QUrl(QStringLiteral("file:///")));
        REQUIRE(local);
        CHECK(local->sameStructure(*playlist));
    }

    TEST_CASE("master, live and encrypted playlists are not cached")
    {
        const QUrl base(QStringLiteral("https://cdn.test/"));
        CHECK(test::errorOf(parseHls("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nlow.m3u8\n", base)) ==
              ErrorCode::Unsupported);
        CHECK(test::errorOf(parseHls("#EXTM3U\n#EXTINF:10,\na.ts\n", base)) == ErrorCode::Unsupported);
        CHECK(test::errorOf(parseHls("#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://k\"\n#EXTINF:10,\na.ts\n"
                                     "#EXT-X-ENDLIST\n",
                                     base)) == ErrorCode::NotCacheable);
        CHECK(
            test::errorOf(parseHls("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:10,\na.ts\n#EXT-X-ENDLIST\n",
                                   base)) == ErrorCode::NotCacheable);
        CHECK(test::errorOf(parseHls("not a playlist", base)) == ErrorCode::ParseError);
    }

    TEST_CASE("byte ranges whose end does not fit in 64 bits are rejected")
    {
        const QUrl base(QStringLiteral("https://cdn.test/"));
        CHECK(test::errorOf(parseHls("#EXTM3U\n#EXTINF:1,\n#EXT-X-BYTERANGE:9223372036854775807@9223372036854775807\n"
                                     "seg.ts\n#EXT-X-ENDLIST\n",
                                     base)) == ErrorCode::ParseError);
        // Without an offset the range follows the previous one, whose end is already at the limit.
        CHECK(test::errorOf(parseHls("#EXTM3U\n#EXTINF:1,\n#EXT-X-BYTERANGE:9223372036854775806@1\nseg.ts\n"
                                     "#EXTINF:1,\n#EXT-X-BYTERANGE:1\nseg.ts\n#EXT-X-ENDLIST\n",
                                     base)) == ErrorCode::ParseError);
        CHECK(test::errorOf(parseHls("#EXTM3U\n#EXT-X-MAP:URI=\"i.mp4\",BYTERANGE=\"2@9223372036854775807\"\n"
                                     "#EXTINF:1,\nseg.ts\n#EXT-X-ENDLIST\n",
                                     base)) == ErrorCode::ParseError);
    }
}

TEST_SUITE("cache.http.policy")
{
    TEST_CASE("Cache-Control directives")
    {
        const CacheControl control = parseCacheControl("public, Max-Age=\"600\", must-revalidate, stale-if-error=60");
        CHECK(control.maxAge == 600);
        CHECK(control.mustRevalidate);
        CHECK(control.staleIfError == 60);
        CHECK_FALSE(control.noStore);
        CHECK(parseCacheControl("no-store").noStore);
        CHECK(parseCacheControl("no-cache, max-age=10").noCache);
    }

    TEST_CASE("what is stored and for how long")
    {
        net::Request get;
        get.url = QUrl(QStringLiteral("https://api.test/tracks/1"));
        net::Response ok;
        ok.status = 200;
        ok.body = "{}";
        CHECK(isStorable(get, ok));
        net::Response noStore = ok;
        noStore.headers = {{"cache-control", "private, no-store"}};
        CHECK_FALSE(isStorable(get, noStore));
        net::Response serverError = ok;
        serverError.status = 500;
        CHECK_FALSE(isStorable(get, serverError));
        net::Request post = get;
        post.method = net::Method::Post;
        CHECK_FALSE(isStorable(post, ok));
        net::Request ranged = get;
        ranged.range = net::ByteRange{0, 10};
        CHECK_FALSE(isStorable(ranged, ok));

        StoredResponse stored;
        stored.storedAt = 1000;
        stored.ttlFallback = 30;
        CHECK(freshnessLifetime(stored) == 30);
        CHECK(isFresh(stored, 1029));
        CHECK_FALSE(isFresh(stored, 1030));
        stored.headers = {{"date", "Sun, 06 Nov 1994 08:49:37 GMT"}, {"expires", "Sun, 06 Nov 1994 09:49:37 GMT"}};
        CHECK(freshnessLifetime(stored) == 3600);
        stored.headers.append(qMakePair(QByteArray("cache-control"), QByteArray("max-age=5")));
        CHECK(freshnessLifetime(stored) == 5);
        stored.headers.append(qMakePair(QByteArray("age"), QByteArray("4")));
        CHECK(isFresh(stored, 1000));
        CHECK_FALSE(isFresh(stored, 1001));
    }

    TEST_CASE("keys separate accounts; Vary must match")
    {
        net::Request a;
        a.url = QUrl(QStringLiteral("https://api.test/me/likes"));
        net::Request b = a;
        b.headers = {{"authorization", "OAuth one"}};
        net::Request c = a;
        c.headers = {{"authorization", "OAuth two"}};
        CHECK(httpCacheKey(a) != httpCacheKey(b));
        CHECK(httpCacheKey(b) != httpCacheKey(c));
        CHECK(httpCacheKey(b) == httpCacheKey(b));

        StoredResponse stored;
        stored.varyValues = {{"accept-language", "ru"}};
        net::Request ru = a;
        ru.headers = {{"accept-language", "ru"}};
        CHECK(varyMatches(stored, ru));
        CHECK_FALSE(varyMatches(stored, a));

        stored.status = 200;
        stored.body = "body";
        stored.headers = {{"etag", "\"x\""}};
        const Result<StoredResponse> parsed = StoredResponse::parse(stored.serialize());
        REQUIRE(parsed);
        CHECK(parsed->body == "body");
        CHECK(parsed->header("etag") == "\"x\"");
        QByteArray damaged = stored.serialize();
        damaged[damaged.size() - 12] = 'Z';
        CHECK(test::errorOf(StoredResponse::parse(damaged)) == ErrorCode::Corrupt);
    }
}

TEST_SUITE("cache.payload")
{
    TEST_CASE("containers are recognised and the payload hash ignores tags")
    {
        const QByteArray mp3 = *readWholeFile(fixture("sine.mp3"));
        const QByteArray m4a = *readWholeFile(fixture("sine.m4a"));
        const QByteArray opus = *readWholeFile(fixture("sine.opus"));
        CHECK(detectContainer(mp3) == Container::Mpeg);
        CHECK(detectContainer(m4a) == Container::Mp4);
        CHECK(detectContainer(opus) == Container::Ogg);
        CHECK(detectContainer("garbage!") == Container::Unknown);

        // An ID3v2 tag in front and an ID3v1 tag at the end do not change the MPEG payload.
        QByteArray tagged = QByteArray("ID3\x04\x00\x00\x00\x00\x00\x0A", 10) + QByteArray(10, '\0') + mp3 +
                            QByteArray("TAG") + QByteArray(125, ' ');
        CHECK(*payloadHash(tagged) == *payloadHash(mp3));
        QByteArray changed = mp3;
        changed[changed.size() / 2] = static_cast<char>(changed[changed.size() / 2] ^ 1);
        CHECK(*payloadHash(changed) != *payloadHash(mp3));
        CHECK(payloadHash(m4a));
        CHECK(payloadHash(opus));
        CHECK(test::errorOf(payloadHash(QByteArray("plain text"))) == ErrorCode::Unsupported);
    }

    TEST_CASE("an APEv2 tag at the end is skipped; a footer with an impossible size is corrupt, not a hang")
    {
        const QByteArray mp3 = *readWholeFile(fixture("sine.mp3"));
        const auto footer = [](std::uint32_t size, bool header) {
            QByteArray bytes("APETAGEX");
            const auto le32 = [&bytes](std::uint32_t value) {
                for (int i = 0; i < 4; ++i) {
                    bytes += static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU);
                }
            };
            le32(2000);  // version
            le32(size);  // items + footer
            le32(1);     // item count
            le32(header ? 0x80000000U : 0U);
            bytes += QByteArray(8, '\0');
            return bytes;
        };
        const QByteArray items(10, 'x');
        CHECK(*payloadHash(QByteArray(mp3 + items + footer(42, false))) == *payloadHash(mp3));
        CHECK(*payloadHash(QByteArray(mp3 + QByteArray(32, 'h') + items + footer(42, true))) == *payloadHash(mp3));
        for (const std::uint32_t size : {0U, 1U, 31U}) {
            CAPTURE(size);
            CHECK(test::errorOf(payloadHash(QByteArray(mp3 + footer(size, false)))) == ErrorCode::Corrupt);
        }
        CHECK(test::errorOf(payloadHash(QByteArray(mp3 + footer(0xFFFFFFF0U, false)))) == ErrorCode::Corrupt);
    }
}

TEST_SUITE("cache.tags")
{
    TEST_CASE("tags round-trip in every container and leave the audio byte-identical")
    {
        const QByteArray cover = pngCover(Qt::darkCyan);
        TagData tags;
        tags.title = QStringLiteral("Ночной город 夜の街 🌃");
        tags.artist = QStringLiteral("Исполнитель · 歌手 👩🏽‍🎤");
        tags.album = QStringLiteral("Альбом");
        tags.genre = QStringLiteral("Электроника");
        tags.date = QStringLiteral("2024-05-01");
        tags.permalink = QUrl(QStringLiteral("https://soundcloud.com/artist/track"));
        tags.cover = cover;
        tags.coverMime = QStringLiteral("image/png");

        for (const char* name : {"sine.mp3", "sine.m4a", "sine.opus"}) {
            CAPTURE(name);
            QTemporaryDir dir;
            const QString file = dir.filePath(QStringLiteral("data"));
            REQUIRE(QFile::copy(fixture(name), file));
            const QByteArray before = *payloadHashOfFile(file);

            REQUIRE(writeTags(file, tags, before, FileOps::posix()));
            const Result<TagData> read = readTags(file);
            REQUIRE(read);
            CHECK(read->title == tags.title);
            CHECK(read->artist == tags.artist);
            CHECK(read->album == tags.album);
            CHECK(read->genre == tags.genre);
            CHECK(read->date.startsWith(QStringLiteral("2024")));
            CHECK(read->permalink == tags.permalink);
            CHECK(read->cover == cover);  // embedded exactly as received
            CHECK(*payloadHashOfFile(file) == before);
            CHECK_FALSE(QFileInfo::exists(file + QStringLiteral(".tagging")));

            // Re-tagging (metadata changed) replaces the tags, still without touching the audio; no album now.
            TagData renamed = tags;
            renamed.title = QStringLiteral("Новое название");
            renamed.album.clear();
            REQUIRE(writeTags(file, renamed, before, FileOps::posix()));
            CHECK(readTags(file)->title == renamed.title);
            CHECK(readTags(file)->album.isEmpty());
            CHECK(*payloadHashOfFile(file) == before);
        }
    }

    TEST_CASE("a failure while tagging leaves the previous file intact")
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("data"));
        REQUIRE(QFile::copy(fixture("sine.mp3"), file));
        const QByteArray original = *readWholeFile(file);
        FullDisk full;
        TagData tags;
        tags.title = QStringLiteral("x");
        CHECK(test::errorOf(writeTags(file, tags, {}, full)) == ErrorCode::NoSpace);
        CHECK(*readWholeFile(file) == original);
        CHECK_FALSE(QFileInfo::exists(file + QStringLiteral(".tagging")));

        // A wrong expected payload (the file is not what the sidecar says) is refused.
        CHECK(test::errorOf(writeTags(file, tags, "0123", FileOps::posix())) == ErrorCode::Corrupt);
        CHECK(*readWholeFile(file) == original);

        // Unknown containers are never remuxed: sidecar only.
        const QString other = dir.filePath(QStringLiteral("segment.ts"));
        REQUIRE(writeFileAtomic(other, QByteArray(4096, 'G')));
        CHECK(test::errorOf(writeTags(other, tags, {}, FileOps::posix())) == ErrorCode::Unsupported);
    }

    TEST_CASE("tag text is stripped of control characters only")
    {
        CHECK(sanitizeTagText(QStringLiteral(" A\u0001B\tC\u0085D 👨‍👩‍👧 ")) ==
              QStringLiteral("ABCD 👨‍👩‍👧"));
        TrackInfo info;
        info.title = QStringLiteral("T\nitle");
        info.uploader = QStringLiteral("uploader");
        info.created = QDateTime(QDate(2020, 1, 2), QTime(23, 0), QTimeZone::UTC);
        const TagData tags = tagDataFor(info, {}, QStringLiteral("image/jpeg"));
        CHECK(tags.title == QStringLiteral("Title"));
        CHECK(tags.artist == QStringLiteral("uploader"));
        CHECK(tags.album.isEmpty());
        CHECK(tags.date == QStringLiteral("2020-01-02"));
        CHECK(tags.coverMime.isEmpty());
    }
}

TEST_SUITE("cache.layout")
{
    TEST_CASE("the layout is created private, versioned and tagged for backup tools")
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("cache"));
        const Result<CacheLayout> layout = prepareLayout(root);
        REQUIRE(layout);
        for (const QString& sub : {layout->audio(), layout->images(), layout->http(), layout->electron()}) {
            CHECK(QFileInfo(sub).isDir());
            CHECK(modeOf(sub) == 0700);
        }
        CHECK(modeOf(root) == 0700);
        CHECK(readWholeFile(layout->versionFile())->trimmed() == QByteArray::number(CacheLayout::kVersion));
        CHECK(readWholeFile(root + QStringLiteral("/CACHEDIR.TAG"))
                  ->startsWith("Signature: 8a477f597d28d172789f06886806bc55"));

        // Another layout version: our subdirectories are wiped, other files in the root survive.
        REQUIRE(writeFileAtomic(layout->audio() + QStringLiteral("/old"), "x"));
        REQUIRE(writeFileAtomic(root + QStringLiteral("/gpu-probe.json"), "{}"));
        REQUIRE(writeFileAtomic(layout->versionFile(), "0\n"));
        REQUIRE(prepareLayout(root));
        CHECK_FALSE(QFileInfo::exists(layout->audio() + QStringLiteral("/old")));
        CHECK(QFileInfo::exists(root + QStringLiteral("/gpu-probe.json")));

        // Without a marker nothing is deleted (it may not be ours).
        REQUIRE(writeFileAtomic(layout->audio() + QStringLiteral("/keep"), "x"));
        QFile::remove(layout->versionFile());
        REQUIRE(prepareLayout(root));
        CHECK(QFileInfo::exists(layout->audio() + QStringLiteral("/keep")));
    }

    TEST_CASE("a second instance cannot take the cache lock")
    {
        QTemporaryDir dir;
        const Result<CacheLayout> layout = prepareLayout(dir.path());
        REQUIRE(layout);
        Result<CacheLock> first = CacheLock::acquire(*layout);
        REQUIRE(first);
        CHECK(test::errorOf(CacheLock::acquire(*layout)) == ErrorCode::CacheLocked);
        *first = CacheLock{};
        CHECK(CacheLock::acquire(*layout));
    }

    TEST_CASE("unwritable locations are refused")
    {
        QTemporaryDir dir;
        const QString root = dir.filePath(QStringLiteral("ro"));
        REQUIRE(ensureDirectory(root));
        ::chmod(QFile::encodeName(root).constData(), 0500);
        CHECK(test::errorOf(prepareLayout(root + QStringLiteral("/sub"))).has_value());
        ::chmod(QFile::encodeName(root).constData(), 0700);
    }

    TEST_CASE("XDG_CACHE_HOME decides where the cache lives")
    {
        QTemporaryDir dir;
        const QByteArray previous = qgetenv("XDG_CACHE_HOME");
        qputenv("XDG_CACHE_HOME", QFile::encodeName(dir.path()));
        const QString cacheDir = AppPaths::standard().cacheDir;
        if (previous.isNull()) {
            qunsetenv("XDG_CACHE_HOME");
        } else {
            qputenv("XDG_CACHE_HOME", previous);
        }
        CHECK(cacheDir == dir.path() + u'/' + QCoreApplication::applicationName());
    }
}

TEST_SUITE("cache.disklru")
{
    TEST_CASE("blobs are evicted least recently used first and survive a restart")
    {
        QTemporaryDir dir;
        {
            DiskLru lru(dir.path(), 3000, FileOps::posix());
            REQUIRE(lru.put("aaaaaaaaaaaaaaaa", QByteArray(1000, 'a')));
            REQUIRE(lru.put("bbbbbbbbbbbbbbbb", QByteArray(1000, 'b')));
            REQUIRE(lru.put("cccccccccccccccc", QByteArray(1000, 'c')));
            CHECK(lru.get("aaaaaaaaaaaaaaaa"));  // now the most recently used
            REQUIRE(lru.put("dddddddddddddddd", QByteArray(1000, 'd')));
            CHECK(lru.bytes() <= 3000);
            CHECK(lru.contains("aaaaaaaaaaaaaaaa"));
            CHECK_FALSE(lru.contains("bbbbbbbbbbbbbbbb"));
            lru.flushIndex();
        }
        DiskLru reopened(dir.path(), 3000, FileOps::posix());
        CHECK(*reopened.get("dddddddddddddddd") == QByteArray(1000, 'd'));
        CHECK(reopened.count() == 3);

        // A blob deleted behind its back is a miss, not an error that sticks.
        QFile::remove(dir.path() + QStringLiteral("/cc/cccccccccccccccc"));
        CHECK(test::errorOf(reopened.get("cccccccccccccccc")) == ErrorCode::NotFound);
        CHECK_FALSE(reopened.contains("cccccccccccccccc"));
        reopened.clear();
        CHECK(reopened.bytes() == 0);
    }

    TEST_CASE("a lost index is rebuilt from the files; a full disk is reported, not fatal")
    {
        QTemporaryDir dir;
        FullDisk full;
        {
            DiskLru lru(dir.path(), 10'000, full);
            REQUIRE(lru.put("aaaaaaaaaaaaaaaa", QByteArray(100, 'a')));
            full.failWrites = true;
            CHECK(test::errorOf(lru.put("bbbbbbbbbbbbbbbb", QByteArray(100, 'b'))) == ErrorCode::NoSpace);
            CHECK(lru.contains("aaaaaaaaaaaaaaaa") == false);  // evicted to make room before the retry
        }
        full.failWrites = false;
        DiskLru lru(dir.path(), 10'000, full);
        REQUIRE(lru.put("cccccccccccccccc", "c"));
        DiskLru again(dir.path(), 10'000, full);  // no index written: rebuilt by scanning
        CHECK(again.get("cccccccccccccccc"));
    }

    TEST_CASE("a rebuild deletes temp files left over from interrupted writes")
    {
        QTemporaryDir dir;
        REQUIRE(QDir(dir.path()).mkpath(QStringLiteral("aa")));
        const QString temp = dir.path() + QStringLiteral("/aa/.aaaaaaaaaaaaaaaa.123abc.tmp");
        {
            QFile file(temp);
            REQUIRE(file.open(QIODevice::WriteOnly));
            file.write(QByteArray(100, 'a'));
        }
        DiskLru lru(dir.path(), 1 << 20, FileOps::posix());  // no index: rebuilt by scanning
        lru.load();
        CHECK_FALSE(QFileInfo::exists(temp));
        CHECK(lru.bytes() == 0);
    }

    TEST_CASE("blobs stored after the last index flush are found and evicted after an unclean exit")
    {
        QTemporaryDir dir;
        {
            DiskLru lru(dir.path(), 1 << 20, FileOps::posix());
            REQUIRE(lru.put("aaaaaaaaaaaaaaaa", QByteArray(100, 'a')));
            lru.flushIndex();
            REQUIRE(lru.put("bbbbbbbbbbbbbbbb", QByteArray(200, 'b')));
            // Gone without flushing, like a crash or SIGTERM.
        }
        DiskLru reopened(dir.path(), 1 << 20, FileOps::posix());
        const Result<QByteArray> late = reopened.get("bbbbbbbbbbbbbbbb");
        REQUIRE(late);
        CHECK(*late == QByteArray(200, 'b'));
        CHECK(reopened.bytes() == 300);
        reopened.setBudget(150);
        CHECK_FALSE(QFileInfo::exists(dir.path() + QStringLiteral("/bb/bbbbbbbbbbbbbbbb")));
        CHECK(reopened.bytes() <= 150);

        // A clean session: the index is trusted again.
        reopened.setBudget(1 << 20);
        REQUIRE(reopened.put("cccccccccccccccc", QByteArray(50, 'c')));
        reopened.flushIndex();
        CHECK_FALSE(QFileInfo::exists(dir.path() + QStringLiteral("/index.unflushed")));
        DiskLru clean(dir.path(), 1 << 20, FileOps::posix());
        clean.load();
        CHECK(clean.bytes() == 50);
    }
}

TEST_SUITE("cache.prefetch")
{
    TEST_CASE("prefetch count follows metered mode and the audio cache switch")
    {
        CHECK(effectivePrefetchCount(2, false, true) == 2);
        CHECK(effectivePrefetchCount(2, true, true) == 0);
        CHECK(effectivePrefetchCount(3, false, false) == 1);
        CHECK(effectivePrefetchCount(0, false, true) == 0);
        CHECK(prefetchBytes(true) == -1);
        CHECK(prefetchBytes(false) > 0);
    }
}
