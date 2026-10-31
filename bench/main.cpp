// noctara-bench: timings of the hot paths named in the spec, for docs/performance.md. Build in release mode
// (`xmake f -m release && xmake build noctara-bench`) and run build/linux/x86_64/release/noctara-bench [filter].
// Each benchmark runs a few batches after a warm-up and prints the median per operation.

#include "api/ApiClient.hpp"
#include "api/Parse.hpp"

#include "cache/MemoryTier.hpp"
#include "cache/PieceMap.hpp"
#include "drm/Protocol.hpp"
#include "models/CollectionModel.hpp"
#include "net/DnsCache.hpp"
#include "settings/Keys.hpp"
#include "settings/Schema.hpp"
#include "settings/TomlCodec.hpp"
#include "theme/AccentEngine.hpp"
#include "theme/Palette.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>

#include <algorithm>
#include <functional>
#include <print>
#include <string>
#include <vector>

using namespace noctara;

namespace {

// Keeps results alive so the optimizer cannot drop the measured work.
template <class T> void keep(const T& value)
{
    asm volatile("" : : "g"(&value) : "memory");
}

struct Benchmark {
    const char* name;
    const char* unit;  // what one operation is
    int operations;    // per batch
    std::function<void()> batch;
};

void run(const Benchmark& benchmark)
{
    benchmark.batch();  // warm-up
    std::vector<double> perOperation;
    for (int round = 0; round < 7; ++round) {
        QElapsedTimer clock;
        clock.start();
        benchmark.batch();
        perOperation.push_back(static_cast<double>(clock.nsecsElapsed()) / benchmark.operations);
    }
    std::ranges::sort(perOperation);
    const double median = perOperation[perOperation.size() / 2];
    const char* scale = "ns";
    double value = median;
    if (value >= 1e6) {
        value /= 1e6;
        scale = "ms";
    } else if (value >= 1e3) {
        value /= 1e3;
        scale = "µs";
    }
    std::println("{:<44} {:>10.2f} {} per {}", benchmark.name, value, scale, benchmark.unit);
}

QByteArray trackJson(int id)
{
    return QStringLiteral(
               R"({"kind":"track","id":%1,"title":"Night Drive %1","duration":215000,"full_duration":215000,
        "genre":"Synthwave","description":"","created_at":"2021-03-04T05:06:07Z","display_date":"2021-03-04T05:06:07Z",
        "last_modified":"2021-03-05T00:00:00Z","permalink_url":"https://soundcloud.com/uploader/night-drive-%1",
        "artwork_url":"https://i1.sndcdn.com/artworks-abc-large.jpg",
        "waveform_url":"https://wave.sndcdn.com/abc_m.json","playback_count":123456,"likes_count":789,
        "reposts_count":12,"comment_count":null,"policy":"ALLOW","streamable":true,
        "track_authorization":"auth-token-xyz",
        "publisher_metadata":{"artist":"The Artist","album_title":"Neon"},
        "user":{"id":42,"username":"uploader","permalink":"uploader","avatar_url":"https://i1.sndcdn.com/avatars-u-large.jpg","verified":true},
        "media":{"transcodings":[
          {"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:%1/aaa/stream/hls","preset":"aac_160k","duration":215000,"snipped":false,"format":{"protocol":"hls","mime_type":"audio/mp4; codecs=\"mp4a.40.2\""},"quality":"sq"},
          {"url":"https://api-v2.soundcloud.com/media/soundcloud:tracks:%1/bbb/stream/progressive","preset":"mp3_1_0","duration":215000,"snipped":false,"format":{"protocol":"progressive","mime_type":"audio/mpeg"},"quality":"sq"}
        ]}})")
        .arg(id)
        .toUtf8();
}

QByteArray pageJson(int tracks)
{
    QList<QByteArray> items;
    for (int id = 1; id <= tracks; ++id) {
        items.append(trackJson(id));
    }
    return "{\"collection\":[" + items.join(',') + "],\"next_href\":null}";
}

// The model only needs a client for paging and hydration, which these benchmarks never trigger.
class NoClient : public api::IApiClient {
public:
    net::RequestHandle send(api::ApiRequest /*request*/, QObject* /*context*/, api::Decoder /*decode*/) override
    {
        return {};
    }
    net::RequestHandle lookupTracks(std::vector<api::Id> /*ids*/, QObject* /*context*/,
                                    std::function<void(Result<std::vector<api::Track>>)> /*done*/) override
    {
        return {};
    }
};

QImage artwork(int size)
{
    // A cover-like image: two gradients and some noise, so k-means has real clusters to find.
    QImage image(size, size, QImage::Format_RGB32);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, size, size);
    gradient.setColorAt(0, QColor(30, 20, 90));
    gradient.setColorAt(0.6, QColor(220, 60, 120));
    gradient.setColorAt(1, QColor(250, 200, 80));
    painter.fillRect(image.rect(), gradient);
    QRandomGenerator generator(7);
    for (int i = 0; i < size * 4; ++i) {
        painter.setPen(QColor::fromRgb(generator.generate()));
        painter.drawPoint(generator.bounded(size), generator.bounded(size));
    }
    return image;
}

std::vector<Benchmark> benchmarks()
{
    std::vector<Benchmark> list;

    // --- JSON parsing (API thread).
    static const QByteArray page = pageJson(50);
    list.push_back({"api: parse a 50-track page (simdjson)", "page", 200, [] {
                        for (int i = 0; i < 200; ++i) {
                            keep(api::parse::collection(page));
                        }
                    }});

    // --- Model updates (GUI thread): a 500-row list replaced, then every role of every row read once (what a
    // ListView does when it first shows the rows).
    static const std::vector<api::Entry> entries = [] {
        std::vector<api::Entry> out;
        Result<api::Page<api::Entry>> parsed = api::parse::collection(pageJson(50));
        for (int copy = 0; copy < 10; ++copy) {
            for (api::Entry entry : parsed->items) {
                if (auto* track = std::get_if<api::Track>(&entry.item)) {
                    track->id += static_cast<api::Id>(copy) * 1000;
                }
                out.push_back(std::move(entry));
            }
        }
        return out;
    }();
    list.push_back({"models: replace 500 rows and read every role", "list", 20, [] {
                        static NoClient client;
                        for (int i = 0; i < 20; ++i) {
                            models::CollectionModel model(client);
                            model.setEntries(entries);
                            const QHash<int, QByteArray> roles = model.roleNames();
                            for (int row = 0; row < model.rowCount({}); ++row) {
                                const QModelIndex index = model.index(row);
                                for (auto it = roles.begin(); it != roles.end(); ++it) {
                                    keep(model.data(index, it.key()));
                                }
                            }
                        }
                    }});

    // --- Cache memory tier (any thread; audio reads come through here).
    static cache::MemoryTier tier(256LL * 1024 * 1024);
    static const QByteArray piece(static_cast<qsizetype>(cache::PieceMap::kChunkSize), 'x');
    static const bool filled = [] {
        tier.setActive(1, true);
        for (std::uint32_t i = 0; i < 2048; ++i) {
            tier.insert({1, 1, i}, piece);
        }
        return true;
    }();
    keep(filled);
    list.push_back({"cache: RAM tier hit (64 KiB piece)", "lookup", 100'000, [] {
                        for (std::uint32_t i = 0; i < 100'000; ++i) {
                            keep(tier.find({1, 1, (i * 7919U) % 2048U}));
                        }
                    }});
    list.push_back({"cache: RAM tier insert with eviction", "insert", 5000, [] {
                        static cache::MemoryTier small(64LL * 1024 * 1024);
                        static std::uint32_t next = 0;
                        for (int i = 0; i < 5000; ++i) {
                            small.insert({2, 1, next++}, piece);
                        }
                    }});

    // --- DNS cache (network thread), 1000 names cached.
    static net::DnsCache dns(2048);
    static std::vector<QString> hosts = [] {
        std::vector<QString> names;
        const auto now = net::DnsCache::Clock::now();
        for (int i = 0; i < 1000; ++i) {
            names.push_back(QStringLiteral("host%1.example.com").arg(i));
            dns.storeAddresses(names.back(), {*IpAddress::parse(u"192.0.2.1")}, std::chrono::seconds(600), now);
        }
        return names;
    }();
    list.push_back({"net: DNS cache hit", "lookup", 100'000, [] {
                        const auto now = net::DnsCache::Clock::now();
                        for (int i = 0; i < 100'000; ++i) {
                            keep(dns.lookup(hosts[static_cast<std::size_t>(i % 1000)], now));
                        }
                    }});

    // --- Theme: palette generation (on every accent change) and artwork analysis (worker thread).
    list.push_back({"theme: generate a palette (dark + light)", "pair", 200, [] {
                        for (int i = 0; i < 200; ++i) {
                            keep(theme::generatePalette({.scheme = theme::Scheme::Dark}));
                            keep(theme::generatePalette({.scheme = theme::Scheme::Light}));
                        }
                    }});
    static const QImage cover = artwork(600);
    list.push_back({"accent: analyse 600 px artwork", "image", 10, [] {
                        for (int i = 0; i < 10; ++i) {
                            keep(theme::analyseArtwork(cover, theme::Extraction::Balanced));
                        }
                    }});

    // --- Software path: the CPU-blurred cover backdrop that replaces the live blur without a GPU.
    list.push_back({"software: blurred 256 px backdrop", "image", 10, [] {
                        for (int i = 0; i < 10; ++i) {
                            keep(theme::blurredBackdrop(cover, theme::Scheme::Dark));
                        }
                    }});

    // --- Settings load: parse and validate a complete, documented config file with some values set.
    static const std::string config = [] {
        const settings::Schema& schema = settings::Schema::builtin();
        settings::ValueMap values;
        values.emplace_back(*schema.indexOf(settings::keys::Crossfade), QVariant::fromValue(qint64{4}));
        values.emplace_back(*schema.indexOf(settings::keys::Language), QStringLiteral("ru"));
        values.emplace_back(*schema.indexOf(settings::keys::StreamQuality), QStringLiteral("best"));
        return settings::serializeConfig(schema, values, {}).toStdString();
    }();
    list.push_back(
        {"settings: parse + validate the full config", "file", 50, [] {
             for (int i = 0; i < 50; ++i) {
                 keep(settings::parseConfig(config, QStringLiteral("config.toml"), settings::Schema::builtin()));
             }
         }});

    // --- Protected player: one incoming position message (4 per second while playing).
    list.push_back({"drm: decode + validate a helper message", "message", 10'000, [] {
                        static const QByteArray line =
                            R"({"type":"position","id":"12","ms":61234,"durationMs":323788})";
                        for (int i = 0; i < 10'000; ++i) {
                            keep(drm::decode(line, drm::Direction::FromHelper));
                        }
                    }});
    return list;
}

}  // namespace

int main(int argc, char** argv)
{
    const QGuiApplication app(argc, argv);  // QImage/QPainter and the models need an application object
    const QString filter = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    std::println("noctara-bench ({} build)",
#ifdef NDEBUG
                 "release"
#else
                 "debug: numbers are not representative"
#endif
    );
    for (const Benchmark& benchmark : benchmarks()) {
        if (filter.isEmpty() || QString::fromLatin1(benchmark.name).contains(filter, Qt::CaseInsensitive)) {
            run(benchmark);
        }
    }
    return 0;
}
