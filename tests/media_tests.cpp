// Native playback: the queue, the PCM ring and the mixer (pure), FFmpeg decoding through StreamSource from the
// audio cache (progressive and HLS fixtures), the backend driven by a fake audio device, the controller over fake
// API and backend doubles, MPRIS metadata, the persisted player state and the waveform's bar layout.
#include "TestUtil.hpp"
#include "api/ApiClient.hpp"
#include "api/Streams.hpp"
#include "mock/FakeOrigin.hpp"

#include "cache/AudioCache.hpp"
#include "cache/CacheLayout.hpp"
#include "cache/FileOps.hpp"
#include "cache/ImageCache.hpp"
#include "cache/MemoryTier.hpp"
#include "core/FileIo.hpp"
#include "media/CoverFiles.hpp"
#include "media/Decoder.hpp"
#include "media/Mixer.hpp"
#include "media/Mpris.hpp"
#include "media/NativeBackend.hpp"
#include "media/PcmRing.hpp"
#include "media/PlaybackController.hpp"
#include "media/PlaybackQueue.hpp"
#include "media/SessionState.hpp"
#include "media/StreamSource.hpp"
#include "models/CollectionModel.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"
#include "ui/WaveformItem.hpp"

#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>

using namespace noctara;
using namespace noctara::media;
using namespace std::chrono_literals;

// Test doubles name only the fields that matter.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace {

constexpr AudioFormat kDevice{.sampleRate = 48000, .channels = 2};

QString fixturePath(const QString& name)
{
    return QStringLiteral(NOCTARA_SOURCE_DIR "/tests/fixtures/audio/") + name;
}

QByteArray fixture(const QString& name)
{
    Result<QByteArray> data = readWholeFile(fixturePath(name));
    REQUIRE(data);
    return *data;
}

// --- Audio cache over a fake origin --------------------------------------------------------------

struct Cache {
    QTemporaryDir dir;
    test::FakeOrigin origin;
    cache::MemoryTier memory{64LL << 20};
    cache::CacheLayout layout;
    std::unique_ptr<cache::AudioCache> audio;

    Cache()
        : layout(*cache::prepareLayout(dir.filePath(QStringLiteral("cache"))))
    {
        audio = std::make_unique<cache::AudioCache>(origin, memory, cache::FileOps::posix());
        audio->setLayout(layout);
        cache::AudioCacheConfig config;
        config.embedTags = false;
        audio->setConfig(config);
        audio->loadIndex();
    }

    static cache::UrlProvider urls(const QString& url)
    {
        return [url](bool, cache::UrlCallback done) {
            done(QUrl(url));
        };
    }

    cache::AudioReaderPtr open(const cache::TrackInfo& info, const QString& url)
    {
        QObject context;
        std::optional<Result<cache::AudioReaderPtr>> out;
        audio->open(info, urls(url), &context, [&](Result<cache::AudioReaderPtr> r) { out = std::move(r); });
        REQUIRE(test::waitUntil([&] { return out.has_value(); }));
        REQUIRE(*out);
        return **out;
    }
};

cache::TrackInfo trackInfo(const QString& id, cache::StreamProtocol protocol, const QString& mime)
{
    cache::TrackInfo info;
    info.key = {id, protocol == cache::StreamProtocol::Hls ? QStringLiteral("hls/x/sq")
                                                           : QStringLiteral("progressive/x/sq")};
    info.title = QStringLiteral("Track ") + id;
    info.protocol = protocol;
    info.mimeType = mime;
    return info;
}

// Serves tests/fixtures/audio/<name>/ (playlist.m3u8 and its pieces) under https://cdn.test/<name>/.
QString serveHls(test::FakeOrigin& origin, const QString& name)
{
    const QDir dir(fixturePath(name));
    for (const QString& file : dir.entryList(QDir::Files)) {
        origin.serve(QStringLiteral("https://cdn.test/") + name + u'/' + file, {.body = fixture(name + u'/' + file)});
    }
    return QStringLiteral("https://cdn.test/") + name + QStringLiteral("/playlist.m3u8");
}

struct Decoded {
    std::int64_t frames = 0;
    std::int64_t firstPosition = -1;  // the decoder's position before the first chunk
    float peak = 0;
    std::optional<Error> error;
};

// Decodes on a thread of its own (reads block; the cache answers on this thread's event loop).
Decoded decodeAll(const cache::AudioReaderPtr& reader, int segment = 0, DecoderStart start = {},
                  std::optional<std::int64_t> seekFrame = std::nullopt)
{
    Decoded out;
    std::atomic<bool> done{false};
    std::jthread worker([&] {
        StreamSource source(reader, {}, segment);
        Result<std::unique_ptr<Decoder>> decoder = Decoder::open(source, kDevice, start);
        if (decoder && seekFrame) {
            if (Result<void> sought = (*decoder)->seek(*seekFrame); !sought) {
                decoder = std::unexpected(sought.error());
            }
        }
        if (!decoder) {
            out.error = decoder.error();
            done = true;
            return;
        }
        out.firstPosition = (*decoder)->position();
        std::vector<float> chunk;
        while (true) {
            chunk.clear();
            Result<bool> more = (*decoder)->decode(chunk);
            if (!more) {
                out.error = more.error();
                break;
            }
            if (!*more) {
                break;
            }
            out.frames += static_cast<std::int64_t>(chunk.size()) / kDevice.channels;
            for (const float sample : chunk) {
                out.peak = std::max(out.peak, std::abs(sample));
            }
        }
        done = true;
    });
    REQUIRE(test::waitUntil([&] { return done.load(); }, 15000));
    worker.join();
    return out;
}

// --- Doubles -------------------------------------------------------------------------------------

// An audio device the test drives: pull() calls the render function like the audio thread would.
class FakeOutput : public IAudioOutput {
public:
    explicit FakeOutput(AudioFormat format = kDevice)
        : m_format(format)
    {
    }

    Result<AudioFormat> preferredFormat() override { return m_format; }
    Result<void> start(AudioFormat format, RenderFunction render) override
    {
        m_format = format;
        m_render = std::move(render);
        m_running = true;
        ++starts;
        return {};
    }
    void suspend() override { suspended = true; }
    void resume() override { suspended = false; }
    void stop() override
    {
        m_running = false;
        m_render = nullptr;
    }
    [[nodiscard]] bool running() const override { return m_running; }
    [[nodiscard]] std::int64_t latencyFrames() const override { return 0; }

    // Renders `frames` frames; returns how many of them carried sound.
    std::int64_t pull(std::int64_t frames)
    {
        if (!m_render) {
            return 0;
        }
        std::vector<float> buffer(static_cast<std::size_t>(frames * m_format.channels));
        m_render(buffer);
        std::int64_t audible = 0;
        for (std::int64_t f = 0; f < frames; ++f) {
            if (std::abs(buffer[static_cast<std::size_t>(f * m_format.channels)]) > 1e-4F) {
                ++audible;
            }
        }
        return audible;
    }

    bool suspended = false;
    int starts = 0;

private:
    AudioFormat m_format;
    RenderFunction m_render;
    bool m_running = false;
};

// Pulls audio in 10 ms steps, about as fast as a device would (the backend checks its state every 100 ms), while
// keeping the event loop going, until `until` holds.
template <class Until>
bool pump(FakeOutput& output, Until until, std::int64_t* audible = nullptr, int timeoutMs = 15000)
{
    QElapsedTimer pace;
    pace.start();
    return test::waitUntil(
        [&] {
            if (pace.elapsed() >= 5) {
                pace.restart();
                const std::int64_t got = output.pull(480);
                if (audible != nullptr) {
                    *audible += got;
                }
            }
            return until();
        },
        timeoutMs);
}

// IApiClient answering from a handler on the caller's context, recording requests; lookupTracks from a table.
class FakeClient : public api::IApiClient {
public:
    using Handler = std::function<Result<QByteArray>(const api::ApiRequest&)>;

    net::RequestHandle send(api::ApiRequest request, QObject* context, api::Decoder decode) override
    {
        requests.push_back(request.url);
        Result<QByteArray> body = handler ? handler(request) : fail(ErrorCode::NotFound, "no handler");
        Result<api::Payload> payload =
            body ? Result<api::Payload>(api::Payload{.body = *body}) : std::unexpected(body.error());
        std::function<void()> deliver = decode(std::move(payload));
        QMetaObject::invokeMethod(context, std::move(deliver), Qt::QueuedConnection);
        return {};
    }

    net::RequestHandle lookupTracks(std::vector<api::Id> ids, QObject* context,
                                    std::function<void(Result<std::vector<api::Track>>)> done) override
    {
        ++lookups;
        std::vector<api::Track> found;
        for (const api::Id id : ids) {
            if (const auto it = known.find(id); it != known.end()) {
                found.push_back(it->second);
            }
        }
        QMetaObject::invokeMethod(
            context, [done = std::move(done), found = std::move(found)] { done(found); }, Qt::QueuedConnection);
        return {};
    }

    Handler handler;
    std::map<api::Id, api::Track> known;
    std::vector<QUrl> requests;
    int lookups = 0;
};

class FakeBackend : public IPlaybackBackend {
public:
    void load(const PlaybackSource& source, std::int64_t startMs, bool paused) override
    {
        loads.push_back(source);
        startPositions.push_back(startMs);
        m_position = startMs;
        m_state = paused ? BackendState::Paused : BackendState::Playing;
        Q_EMIT stateChanged();
    }
    void setNext(const std::optional<PlaybackSource>& source) override { next = source; }
    void play() override { setState(BackendState::Playing); }
    void pause() override { setState(BackendState::Paused); }
    void stop() override { setState(BackendState::Idle); }
    void seek(std::int64_t ms) override
    {
        seeks.push_back(ms);
        m_position = ms;
    }
    void setGain(double value) override { gain = value; }
    void setCrossfade(std::int64_t ms) override { crossfade = ms; }
    [[nodiscard]] BackendState state() const override { return m_state; }
    [[nodiscard]] std::int64_t position() const override { return m_position; }
    [[nodiscard]] bool buffering() const override { return false; }
    [[nodiscard]] std::vector<std::pair<double, double>> cachedRanges() const override { return {}; }

    void setPosition(std::int64_t ms)
    {
        m_position = ms;
        Q_EMIT positionChanged();
    }
    void setState(BackendState state)
    {
        m_state = state;
        Q_EMIT stateChanged();
    }

    std::vector<PlaybackSource> loads;
    std::vector<std::int64_t> startPositions;
    std::vector<std::int64_t> seeks;
    std::optional<PlaybackSource> next;
    double gain = -1;
    std::int64_t crossfade = -1;

private:
    BackendState m_state = BackendState::Idle;
    std::int64_t m_position = 0;
};

api::Track apiTrack(api::Id id, bool drm = false)
{
    api::Track track;
    track.id = id;
    track.title = QStringLiteral("Track %1").arg(id);
    track.user.username = QStringLiteral("Artist");
    track.durationMs = 200'000;
    track.trackAuthorization = QStringLiteral("auth-%1").arg(id);
    api::Transcoding transcoding;
    transcoding.url = QStringLiteral("https://api-v2.soundcloud.com/media/soundcloud:tracks:%1/x/stream/hls").arg(id);
    transcoding.protocolName = drm ? QStringLiteral("ctr-encrypted-hls") : QStringLiteral("hls");
    transcoding.protocol = cache::StreamProtocol::Hls;
    transcoding.preset = QStringLiteral("aac_160k");
    transcoding.mimeType = QStringLiteral("audio/mp4");
    transcoding.quality = QStringLiteral("hq");
    transcoding.encrypted = drm;
    track.transcodings.push_back(transcoding);
    return track;
}

struct Player {
    settings::SettingsStore store{settings::Schema::builtin()};
    FakeClient client;
    FakeBackend backend;
    std::unique_ptr<PlaybackController> controller;

    FakeBackend protectedBackend;

    explicit Player(const QString& statePath = {}, std::map<api::Id, api::Track> known = {}, bool withProtected = false)
    {
        client.known = std::move(known);
        controller = std::make_unique<PlaybackController>(
            PlaybackController::Dependencies{.settings = store,
                                             .api = client,
                                             .cache = nullptr,
                                             .native = backend,
                                             .protectedBackend = withProtected ? &protectedBackend : nullptr,
                                             .statePath = statePath});
    }
    [[nodiscard]] QString loadedId(std::size_t index) const { return backend.loads.at(index).info.key.trackId; }
};

std::shared_ptr<PcmRing> ringOf(std::int64_t frames, float value, bool finished, int channels = 1)
{
    auto ring = std::make_shared<PcmRing>(channels, std::max<std::int64_t>(frames, 1) + 16);
    REQUIRE(ring->write(std::vector<float>(static_cast<std::size_t>(frames * channels), value), {}));
    if (finished) {
        ring->finish();
    }
    return ring;
}

std::vector<float> render(Mixer& mixer, std::int64_t frames)
{
    std::vector<float> out(static_cast<std::size_t>(frames * mixer.format().channels));
    mixer.render(out);
    return out;
}

}  // namespace

// --- Queue ---------------------------------------------------------------------------------------

namespace {

std::vector<QueueItem> items(std::initializer_list<api::Id> ids)
{
    std::vector<QueueItem> out;
    for (const api::Id id : ids) {
        out.push_back({.track = apiTrack(id)});
    }
    return out;
}

std::vector<api::Id> order(const PlaybackQueue& queue)
{
    std::vector<api::Id> out;
    for (const QueueItem& item : queue.items()) {
        out.push_back(item.track.id);
    }
    return out;
}

}  // namespace

TEST_SUITE("media.queue")
{
    TEST_CASE("repeat decides what follows the last item")
    {
        PlaybackQueue queue;
        queue.replace(items({1, 2, 3}), 2);
        CHECK(queue.current() == 2);
        CHECK_FALSE(queue.following().has_value());
        CHECK_FALSE(queue.hasNext());
        queue.setRepeat(Repeat::All);
        CHECK(queue.following() == 0);
        CHECK(queue.hasNext());
        queue.setRepeat(Repeat::One);
        CHECK(queue.following() == 2);
        // A skip moves on even with Repeat One; an automatic advance repeats.
        CHECK(queue.advance(false) == 2);
        CHECK(queue.advance(true) == 0);
    }

    TEST_CASE("previous wraps around only while repeating")
    {
        PlaybackQueue queue;
        queue.replace(items({1, 2}), 0);
        CHECK_FALSE(queue.previous().has_value());
        CHECK(queue.current() == 0);
        queue.setRepeat(Repeat::All);
        CHECK(queue.previous() == 1);
        CHECK(queue.previous() == 0);
    }

    TEST_CASE("removing and moving items keeps the current one current")
    {
        PlaybackQueue queue;
        queue.replace(items({1, 2, 3, 4}), 2);
        REQUIRE(queue.remove(0));
        CHECK(queue.current() == 1);
        CHECK(queue.currentItem()->track.id == 3);
        REQUIRE(queue.move(2, 0));  // 4 to the front
        CHECK(order(queue) == std::vector<api::Id>{4, 2, 3});
        CHECK(queue.currentItem()->track.id == 3);
        REQUIRE(queue.remove(2));  // the current, last item: the one before becomes current
        CHECK(queue.currentItem()->track.id == 2);
        queue.clear();
        CHECK(queue.current() == -1);
        CHECK(queue.currentItem() == nullptr);
    }

    TEST_CASE("shuffle keeps the current item, shuffles the rest and returns to the original order")
    {
        PlaybackQueue queue;
        std::vector<QueueItem> many;
        for (api::Id id = 1; id <= 30; ++id) {
            many.push_back({.track = apiTrack(id)});
        }
        queue.replace(std::move(many), 4);
        queue.setShuffle(true);
        CHECK(queue.currentItem()->track.id == 5);
        std::vector<api::Id> shuffled = order(queue);
        CHECK(std::vector<api::Id>(shuffled.begin(), shuffled.begin() + 5) == std::vector<api::Id>{1, 2, 3, 4, 5});
        CHECK_FALSE(std::ranges::is_sorted(shuffled));
        REQUIRE(queue.advance(false));
        const api::Id playing = queue.currentItem()->track.id;
        queue.setShuffle(false);
        CHECK(std::ranges::is_sorted(order(queue)));
        CHECK(queue.currentItem()->track.id == playing);

        // A new queue while shuffling starts with the chosen item.
        queue.setShuffle(true);
        queue.replace(items({10, 11, 12, 13}), 2);
        CHECK(queue.current() == 0);
        CHECK(queue.currentItem()->track.id == 12);
        queue.setShuffle(false);
        CHECK(order(queue) == std::vector<api::Id>{10, 11, 12, 13});
        CHECK(queue.currentItem()->track.id == 12);
    }

    TEST_CASE("every shuffle draws fresh randomness: the same queue shuffles differently each time")
    {
        const auto shuffled = [] {
            PlaybackQueue queue;
            std::vector<QueueItem> many;
            for (api::Id id = 1; id <= 30; ++id) {
                many.push_back({.track = apiTrack(id)});
            }
            queue.replace(std::move(many), 0);
            queue.setShuffle(true);
            return order(queue);
        };
        const std::vector<api::Id> first = shuffled();
        const std::vector<api::Id> second = shuffled();
        // Both keep the current item first and are permutations of the same items.
        CHECK(first.front() == 1);
        CHECK(second.front() == 1);
        CHECK(std::ranges::is_permutation(first, second));
        // Identical orders by chance: 1 in 29! (about 1e-31).
        CHECK(first != second);
    }

    TEST_CASE("play next lands after the current item, also in the original order")
    {
        PlaybackQueue queue;
        queue.replace(items({1, 2, 3}), 0);
        queue.setShuffle(true);
        queue.insertNext(items({9}));
        CHECK(order(queue)[1] == 9);
        queue.setShuffle(false);
        CHECK(order(queue) == std::vector<api::Id>{1, 9, 2, 3});
    }
}

// --- PCM ring --------------------------------------------------------------------------------------

TEST_SUITE("media.ring")
{
    TEST_CASE("frames come out in order across the wrap; the position follows what was read")
    {
        PcmRing ring(2, 8);
        ring.reset(100);
        const std::vector<float> first{1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6};
        REQUIRE(ring.write(first, {}));
        std::vector<float> out(8);
        CHECK(ring.read(out) == 4);
        CHECK(out == std::vector<float>{1, 1, 2, 2, 3, 3, 4, 4});
        CHECK(ring.position() == 104);
        const std::vector<float> second{7, 7, 8, 8, 9, 9, 10, 10};
        REQUIRE(ring.write(second, {}));  // wraps around the end of the buffer
        std::vector<float> rest(12);
        CHECK(ring.read(rest) == 6);
        CHECK(rest == std::vector<float>{5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10});
        CHECK(ring.position() == 110);
        CHECK(ring.origin() == 100);
        CHECK_FALSE(ring.drained());
        ring.finish();
        CHECK(ring.drained());
    }

    TEST_CASE("a full ring holds the producer back until the consumer reads; stopping releases it")
    {
        PcmRing ring(1, 4);
        ring.reset(0);
        std::atomic<bool> written{false};
        std::jthread producer([&] {
            const std::vector<float> samples(10, 1.0F);
            written = ring.write(samples, {});
        });
        std::this_thread::sleep_for(20ms);
        CHECK_FALSE(written.load());
        std::vector<float> out(4);
        std::int64_t total = 0;
        REQUIRE(test::waitUntil([&] {
            total += ring.read(out);
            return written.load();
        }));
        producer.join();
        total += ring.read(out);
        CHECK(total == 10);

        std::stop_source stop;
        std::atomic<int> result{-1};
        REQUIRE(ring.write(std::vector<float>(4, 1.0F), {}));
        std::jthread blocked([&] { result = ring.write(std::vector<float>(4, 1.0F), stop.get_token()) ? 1 : 0; });
        std::this_thread::sleep_for(20ms);
        stop.request_stop();
        blocked.join();
        CHECK(result.load() == 0);
    }

    TEST_CASE("hold hides the data until the next reset, which restarts the position")
    {
        PcmRing ring(1, 16);
        ring.reset(0);
        REQUIRE(ring.write(std::vector<float>(8, 1.0F), {}));
        ring.hold();
        std::vector<float> out(8);
        CHECK(ring.read(out) == 0);
        CHECK(ring.available() == 0);
        ring.reset(500);
        CHECK_FALSE(ring.held());
        CHECK(ring.available() == 0);
        CHECK(ring.position() == 500);
    }
}

// --- Mixer ---------------------------------------------------------------------------------------

TEST_SUITE("media.mixer")
{
    // 1 kHz mono keeps the numbers readable: the ramps are 15 frames.
    constexpr AudioFormat kSlow{.sampleRate = 1000, .channels = 1};

    TEST_CASE("the next voice takes over without a gap, then the mixer reports the end")
    {
        Mixer mixer(kSlow);
        auto first = ringOf(1000, 0.5F, true);
        auto second = ringOf(1000, 0.25F, true);
        mixer.setVoices(first, nullptr);
        REQUIRE(mixer.replaceNext(second, 0));
        std::vector<float> out;
        for (int i = 0; i < 8; ++i) {
            const std::vector<float> block = render(mixer, 250);
            out.insert(out.end(), block.begin(), block.end());
        }
        CHECK(static_cast<double>(out[500]) == doctest::Approx(0.5));
        CHECK(static_cast<double>(out[999]) == doctest::Approx(0.5));
        CHECK(static_cast<double>(out[1000]) == doctest::Approx(0.25));  // no fade, no silence at the seam
        CHECK(static_cast<double>(out[1999]) == doctest::Approx(0.25));
        CHECK(mixer.advances() == 1);
        CHECK_FALSE(mixer.ended());
        (void)render(mixer, 10);
        CHECK(mixer.ended());
        // An armed voice is refused once the mixer moved past the advance the caller knows about.
        CHECK_FALSE(mixer.replaceNext(nullptr, 0));
        CHECK(mixer.replaceNext(nullptr, 1));
    }

    TEST_CASE("a crossfade blends the end of one voice into the next with equal power")
    {
        Mixer mixer(kSlow);
        mixer.setCrossfadeFrames(200);
        auto first = ringOf(1000, 1.0F, true);
        auto second = ringOf(1000, 0.0F, true);
        mixer.setVoices(first, nullptr);
        REQUIRE(mixer.replaceNext(second, 0));
        std::vector<float> out;
        for (int i = 0; i < 4; ++i) {
            const std::vector<float> block = render(mixer, 250);
            out.insert(out.end(), block.begin(), block.end());
        }
        CHECK(static_cast<double>(out[799]) == doctest::Approx(1.0));
        CHECK(out[900] == doctest::Approx(std::cos(std::numbers::pi / 4)).epsilon(0.02));
        CHECK(out[999] < 0.02F);
        CHECK(mixer.advances() == 1);
        CHECK(second->position() == 1000 - 800);  // the next voice already played through the blend
    }

    TEST_CASE("volume and pause change smoothly; nothing is consumed once paused")
    {
        Mixer mixer(kSlow);
        auto ring = ringOf(5000, 1.0F, false);
        mixer.setVoices(ring, nullptr);
        (void)render(mixer, 50);  // past the fade-in
        mixer.setGain(0.5F);
        const std::vector<float> softer = render(mixer, 50);
        float steepest = 0;
        for (std::size_t i = 1; i < softer.size(); ++i) {
            steepest = std::max(steepest, std::abs(softer[i] - softer[i - 1]));
        }
        CHECK(steepest <= 1.0F / 15 + 1e-4F);
        CHECK(static_cast<double>(softer.back()) == doctest::Approx(0.5));

        mixer.setPaused(true);
        const std::vector<float> fading = render(mixer, 50);
        CHECK(static_cast<double>(fading.back()) == doctest::Approx(0.0));
        (void)render(mixer, 10);
        CHECK(mixer.silent());
        const std::int64_t at = ring->position();
        (void)render(mixer, 100);
        CHECK(ring->position() == at);

        mixer.setPaused(false);
        const std::vector<float> resumed = render(mixer, 50);
        CHECK(resumed.front() < 0.1F);
        CHECK(static_cast<double>(resumed.back()) == doctest::Approx(0.5));
    }

    TEST_CASE("a starving voice is reported and fades back in")
    {
        Mixer mixer(kSlow);
        auto ring = std::make_shared<PcmRing>(1, 1000);
        ring->reset(0);
        mixer.setVoices(ring, nullptr);
        (void)render(mixer, 20);
        CHECK(mixer.starved());
        REQUIRE(ring->write(std::vector<float>(500, 1.0F), {}));
        const std::vector<float> out = render(mixer, 40);
        CHECK_FALSE(mixer.starved());
        CHECK(out.front() < 0.1F);
        CHECK(static_cast<double>(out.back()) == doctest::Approx(1.0));
    }
}

// --- Decoding --------------------------------------------------------------------------------------

TEST_SUITE("media.decoder")
{
    TEST_CASE("MP3, AAC and Opus decode from the cache into the device format")
    {
        Cache cache;
        for (const auto& [file, mime] : {std::pair{"sine.mp3", "audio/mpeg"}, std::pair{"sine.m4a", "audio/mp4"},
                                         std::pair{"sine.opus", "audio/ogg; codecs=\"opus\""}}) {
            CAPTURE(file);
            const QString url = QStringLiteral("https://cdn.test/") + QLatin1StringView(file);
            cache.origin.serve(url, {.body = fixture(QLatin1StringView(file))});
            const auto reader = cache.open(
                trackInfo(QLatin1StringView(file), cache::StreamProtocol::Progressive, QLatin1StringView(mime)), url);
            const Decoded decoded = decodeAll(reader);
            REQUIRE_MESSAGE(!decoded.error, decoded.error->describe());
            CHECK(decoded.frames == doctest::Approx(2.0 * kDevice.sampleRate).epsilon(0.03));
            CHECK(decoded.peak > 0.05F);  // the fixtures are quiet sines
        }
    }

    TEST_CASE("seeking starts exactly at the target")
    {
        Cache cache;
        const QString url = QStringLiteral("https://cdn.test/seek.mp3");
        cache.origin.serve(url, {.body = fixture(QStringLiteral("sine.mp3"))});
        const auto reader = cache.open(
            trackInfo(QStringLiteral("seek"), cache::StreamProtocol::Progressive, QStringLiteral("audio/mpeg")), url);
        const Decoded decoded = decodeAll(reader, 0, {}, kDevice.sampleRate);
        REQUIRE_MESSAGE(!decoded.error, decoded.error->describe());
        CHECK(decoded.firstPosition == kDevice.sampleRate);
        CHECK(decoded.frames == doctest::Approx(1.0 * kDevice.sampleRate).epsilon(0.05));
    }

    TEST_CASE("HLS segments play as one stream: MP3 segments and fragmented MP4 with an init segment")
    {
        Cache cache;
        for (const char* name : {"hls-mp3", "hls-aac"}) {
            CAPTURE(name);
            const QString playlist = serveHls(cache.origin, QLatin1StringView(name));
            const auto reader = cache.open(
                trackInfo(QLatin1StringView(name), cache::StreamProtocol::Hls, QStringLiteral("audio/mpeg")), playlist);
            const Decoded whole = decodeAll(reader);
            REQUIRE_MESSAGE(!whole.error, whole.error->describe());
            CHECK(whole.frames == doctest::Approx(2.0 * kDevice.sampleRate).epsilon(0.04));

            // A seek opens the stream at the segment holding the target and drops what lies before it.
            REQUIRE(reader->playlist().has_value());
            const std::vector<cache::HlsSegment> segments = reader->playlist()->segments;
            const std::int64_t target = 1200 * kDevice.sampleRate / 1000;
            double start = 0;
            int segment = 0;
            while (start + segments[static_cast<std::size_t>(segment)].duration <= 1.2) {
                start += segments[static_cast<std::size_t>(segment++)].duration;
            }
            const Decoded tail = decodeAll(
                reader, segment, {.frame = static_cast<std::int64_t>(start * kDevice.sampleRate), .trimTo = target});
            REQUIRE_MESSAGE(!tail.error, tail.error->describe());
            CHECK(tail.frames == doctest::Approx(0.8 * kDevice.sampleRate).epsilon(0.08));
        }
    }

    TEST_CASE("data that is not audio fails with DecodeFailed")
    {
        Cache cache;
        const QString url = QStringLiteral("https://cdn.test/junk.mp3");
        cache.origin.serve(url, {.body = QByteArray(20000, 'x')});
        const auto reader = cache.open(
            trackInfo(QStringLiteral("junk"), cache::StreamProtocol::Progressive, QStringLiteral("audio/mpeg")), url);
        const Decoded decoded = decodeAll(reader);
        REQUIRE(decoded.error.has_value());
        CHECK(decoded.error->code == ErrorCode::DecodeFailed);
    }
}

// --- Native backend ------------------------------------------------------------------------------

TEST_SUITE("media.backend")
{
    TEST_CASE("a track plays to its end; the prepared next one follows without a gap")
    {
        Cache cache;
        FakeOutput output;
        NativeBackend backend(*cache.audio, output);
        QSignalSpy advanced(&backend, &IPlaybackBackend::advanced);
        QSignalSpy ended(&backend, &IPlaybackBackend::ended);
        for (const char* name : {"a.mp3", "b.mp3"}) {
            cache.origin.serve(QStringLiteral("https://cdn.test/") + QLatin1StringView(name),
                               {.body = fixture(QStringLiteral("sine.mp3"))});
        }
        const auto source = [](const char* name) {
            return PlaybackSource{.info = trackInfo(QLatin1StringView(name), cache::StreamProtocol::Progressive,
                                                    QStringLiteral("audio/mpeg")),
                                  .urls = Cache::urls(QStringLiteral("https://cdn.test/") + QLatin1StringView(name))};
        };
        backend.load(source("a.mp3"), 0, false);
        CHECK(backend.state() == BackendState::Loading);
        REQUIRE(pump(output, [&] { return backend.state() == BackendState::Playing; }));
        backend.setNext(source("b.mp3"));
        std::int64_t audible = 0;
        REQUIRE(pump(output, [&] { return !ended.isEmpty(); }, &audible));
        CHECK(advanced.size() == 1);
        CHECK(backend.state() == BackendState::Ended);
        CHECK(output.suspended);
        // Two 2-second tracks, minus the fade-in of the first.
        CHECK(audible == doctest::Approx(4.0 * kDevice.sampleRate).epsilon(0.05));
    }

    TEST_CASE("seeking reports the target at once and plays on from there; pausing suspends the device")
    {
        Cache cache;
        FakeOutput output;
        NativeBackend backend(*cache.audio, output);
        cache.origin.serve(QStringLiteral("https://cdn.test/s.mp3"), {.body = fixture(QStringLiteral("sine.mp3"))});
        backend.load(
            {.info = trackInfo(QStringLiteral("s"), cache::StreamProtocol::Progressive, QStringLiteral("audio/mpeg")),
             .urls = Cache::urls(QStringLiteral("https://cdn.test/s.mp3"))},
            0, false);
        REQUIRE(pump(output, [&] { return backend.state() == BackendState::Playing; }));
        backend.seek(1500);
        CHECK(backend.position() == 1500);
        REQUIRE(pump(output, [&] { return backend.position() > 1600; }));
        backend.pause();
        CHECK(backend.state() == BackendState::Paused);
        REQUIRE(pump(output, [&] { return output.suspended; }));
        const std::int64_t at = backend.position();
        (void)output.pull(4800);
        CHECK(backend.position() == at);
        backend.play();
        CHECK_FALSE(output.suspended);
        CHECK(backend.state() == BackendState::Playing);
    }

    TEST_CASE("replay and a backward seek come from the cache with zero requests; a DRM source never writes")
    {
        Cache cache;
        FakeOutput output;
        NativeBackend backend(*cache.audio, output);
        QSignalSpy ended(&backend, &IPlaybackBackend::ended);
        QSignalSpy failed(&backend, &IPlaybackBackend::failed);
        const QString url = QStringLiteral("https://cdn.test/replay.mp3?signature=1");
        cache.origin.serve(url, {.body = fixture(QStringLiteral("sine.mp3"))});
        const PlaybackSource source{.info = trackInfo(QStringLiteral("replay"), cache::StreamProtocol::Progressive,
                                                      QStringLiteral("audio/mpeg")),
                                    .urls = Cache::urls(url)};

        backend.load(source, 0, false);
        REQUIRE(pump(output, [&] { return !ended.isEmpty(); }));
        const int requests = cache.origin.count();
        CHECK(requests > 0);

        backend.stop();
        ended.clear();
        backend.load(source, 0, false);
        REQUIRE(pump(output, [&] { return backend.position() > 1000; }));
        backend.seek(200);
        REQUIRE(pump(output, [&] { return !ended.isEmpty(); }));
        CHECK(cache.origin.count() == requests);

        cache::TrackInfo protectedInfo =
            trackInfo(QStringLiteral("drm"), cache::StreamProtocol::Hls, QStringLiteral("audio/mp4"));
        protectedInfo.drm = true;
        backend.load({.info = protectedInfo, .urls = Cache::urls(QStringLiteral("https://cdn.test/drm.m3u8"))}, 0,
                     false);
        REQUIRE(test::waitUntil([&] { return !failed.isEmpty(); }));
        CHECK(failed.first().first().value<Error>().code == ErrorCode::NotCacheable);
        CHECK(cache.origin.count() == requests);
        CHECK_FALSE(QDir(cache.layout.audio())
                        .entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                        .join(u',')
                        .contains(QString::fromLatin1(
                            cache::TrackKey{QStringLiteral("drm"), QStringLiteral("hls/x/sq")}.name())));
    }
}

// --- Controller ----------------------------------------------------------------------------------

TEST_SUITE("media.controller")
{
    TEST_CASE("a list plays from the chosen track; without a protected player protected tracks are skipped")
    {
        Player player;
        QSignalSpy problems(player.controller.get(), &PlaybackController::problem);
        player.controller->playTracks({apiTrack(1), apiTrack(2, true), apiTrack(3)}, 0, QStringLiteral("Mix"));
        REQUIRE(player.backend.loads.size() == 1);
        CHECK(player.loadedId(0) == u"1");
        CHECK(player.backend.loads[0].info.context == u"Mix");
        CHECK(player.controller->title() == u"Track 1");
        CHECK(player.controller->playing());

        // Nothing is prepared until the end is near; then the protected item cannot be, and the end skips it.
        CHECK_FALSE(player.backend.next.has_value());
        player.backend.setPosition(190'000);
        CHECK_FALSE(player.backend.next.has_value());
        Q_EMIT player.backend.ended();
        REQUIRE(player.backend.loads.size() == 2);
        CHECK(player.loadedId(1) == u"3");
        REQUIRE(problems.size() == 1);
        CHECK(problems.first().first().toInt() == static_cast<int>(ErrorCode::ProtectedPlayback));
    }

    TEST_CASE("protected tracks play in the protected backend, resolved afresh and never prefetched")
    {
        Player player({}, {}, true);
        player.client.handler = [](const api::ApiRequest& request) -> Result<QByteArray> {
            CHECK(request.cache == net::CacheMode::Bypass);
            CHECK(request.url.query().contains(u"track_authorization=auth-2"));
            return QByteArray(R"({"url":"https://playback.media-streaming.soundcloud.cloud/cenc/x/playlist.m3u8",)"
                              R"("licenseAuthToken":"token-2"})");
        };
        player.controller->playTracks({apiTrack(1), apiTrack(2, true), apiTrack(3)}, 0, {});
        REQUIRE(player.backend.loads.size() == 1);
        CHECK(player.protectedBackend.loads.empty());

        // Near the end the protected item is handed to its backend only to warm up; the native one gets nothing.
        player.backend.setPosition(190'000);
        CHECK_FALSE(player.backend.next.has_value());
        REQUIRE(player.protectedBackend.next.has_value());
        CHECK(player.protectedBackend.next->info.drm);

        Q_EMIT player.backend.ended();
        REQUIRE(player.protectedBackend.loads.size() == 1);
        CHECK(player.backend.state() == BackendState::Idle);  // the other backend was stopped
        const PlaybackSource& source = player.protectedBackend.loads[0];
        CHECK(source.info.drm);
        CHECK_FALSE(source.urls);
        REQUIRE(source.protectedStream);
        std::optional<Result<api::ProtectedStream>> stream;
        QObject context;
        source.protectedStream(&context, [&](Result<api::ProtectedStream> answer) { stream = std::move(answer); });
        REQUIRE(test::waitUntil([&] { return stream.has_value(); }));
        REQUIRE(stream->has_value());
        CHECK((*stream)->licenseToken == u"token-2");

        // Its events speak for the current track; the native backend's no longer do.
        player.protectedBackend.setPosition(5000);
        CHECK(player.controller->position() == 5000);
        player.backend.setPosition(99'000);
        CHECK(player.controller->position() == 5000);
        Q_EMIT player.protectedBackend.ended();
        REQUIRE(player.backend.loads.size() == 2);
        CHECK(player.loadedId(1) == u"3");

        // Turned off: reported and skipped.
        REQUIRE(player.store.setUserValue(settings::keys::ProtectedEnabled, false));
        QSignalSpy problems(player.controller.get(), &PlaybackController::problem);
        player.controller->playTracks({apiTrack(4, true), apiTrack(5)}, 0, {});
        REQUIRE(problems.size() == 1);
        CHECK(problems.first().first().toInt() == static_cast<int>(ErrorCode::ProtectedPlayback));
        CHECK(player.loadedId(2) == u"5");
    }

    TEST_CASE("the queue follows its list: new tracks join (shuffled among the upcoming), lost ones leave")
    {
        Player player;
        models::CollectionModel model(player.client);
        const auto entries = [](api::Id first, api::Id last, std::initializer_list<api::Id> skip = {}) {
            std::vector<api::Entry> out;
            for (api::Id id = first; id <= last; ++id) {
                if (std::ranges::find(skip, id) == skip.end()) {
                    out.push_back({.item = apiTrack(id), .at = {}, .repostedBy = std::nullopt});
                }
            }
            return out;
        };
        const auto queued = [&player] {
            std::vector<api::Id> ids;
            for (const QueueItem& item : player.controller->queue().items()) {
                ids.push_back(item.track.id);
            }
            return ids;
        };
        model.setEntries(entries(1, 3));
        player.controller->playCollection(&model, 0, QStringLiteral("Likes"));
        REQUIRE(queued() == std::vector<api::Id>{1, 2, 3});
        model.applyEntries(entries(1, 5));  // the rest of the list arrived
        REQUIRE(test::waitUntil([&] { return queued().size() == 5; }));
        CHECK(queued() == std::vector<api::Id>{1, 2, 3, 4, 5});
        model.applyEntries(entries(1, 5, {2}));  // unliked meanwhile
        REQUIRE(test::waitUntil([&] { return queued().size() == 4; }));
        CHECK(queued() == std::vector<api::Id>{1, 3, 4, 5});
        model.applyEntries(entries(3, 5));  // the one playing stays
        test::waitUntil([] { return false; }, 100);
        CHECK(queued() == std::vector<api::Id>{1, 3, 4, 5});

        // Shuffled: later tracks land at random places after the current one, each once.
        model.setEntries(entries(1, 3));
        player.controller->shufflePlay(&model, QStringLiteral("Likes"));
        const api::Id current = player.controller->currentTrack()->id;
        model.applyEntries(entries(1, 40));
        REQUIRE(test::waitUntil([&] { return queued().size() == 40; }));
        std::vector<api::Id> ids = queued();
        CHECK(ids[static_cast<std::size_t>(player.controller->queue().current())] == current);
        CHECK(player.controller->queue().current() <= 2);  // nothing new went before it
        std::vector<api::Id> sorted = ids;
        std::ranges::sort(sorted);
        CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
        CHECK_FALSE(std::ranges::is_sorted(ids));  // 37 new tracks in order would be a 1 in 37! chance

        // A queue started elsewhere stops following.
        player.controller->playTracks({apiTrack(99)}, 0, {});
        model.applyEntries(entries(1, 45));
        test::waitUntil([] { return false; }, 100);
        CHECK(queued() == std::vector<api::Id>{99});
    }

    TEST_CASE("a followed list repointed to other contents (a new search, a refresh) stops being followed")
    {
        Player player;
        models::CollectionModel model(player.client);
        const auto entries = [](api::Id first, api::Id last) {
            std::vector<api::Entry> out;
            for (api::Id id = first; id <= last; ++id) {
                out.push_back({.item = apiTrack(id), .at = {}, .repostedBy = std::nullopt});
            }
            return out;
        };
        const auto queued = [&player] {
            std::vector<api::Id> ids;
            for (const QueueItem& item : player.controller->queue().items()) {
                ids.push_back(item.track.id);
            }
            return ids;
        };
        model.setEntries(entries(1, 3));
        player.controller->playCollection(&model, 0, QStringLiteral("Search"));
        REQUIRE(queued() == std::vector<api::Id>{1, 2, 3});
        model.setEntries(entries(10, 12));  // clear + append, as a new search does
        test::waitUntil([] { return false; }, 100);
        CHECK(queued() == std::vector<api::Id>{1, 2, 3});
        model.applyEntries(entries(20, 22));
        test::waitUntil([] { return false; }, 100);
        CHECK(queued() == std::vector<api::Id>{1, 2, 3});

        // A paged list refreshed from page one (History's refresh) is not followed afterwards either.
        player.client.handler = [](const api::ApiRequest& /*request*/) -> Result<QByteArray> {
            return QByteArray(R"({"collection":[{"kind":"track","id":1,"title":"x"}],"next_href":null})");
        };
        models::CollectionModel paged(player.client);
        paged.setEntries(entries(1, 3));
        player.controller->playCollection(&paged, 0, QStringLiteral("History"));
        paged.setSource([](int limit) { return api::endpoints::history(limit); }, 2);  // page one: track 1 only
        REQUIRE(test::waitUntil([&] { return paged.count() == 1; }));
        test::waitUntil([] { return false; }, 100);
        CHECK(queued() == std::vector<api::Id>{1, 2, 3});
    }

    TEST_CASE("saved (stale) tracks are fetched again before they play, and play as saved when that fails")
    {
        api::Track fresh = apiTrack(7);
        fresh.trackAuthorization = QStringLiteral("fresh");
        Player player({}, {{7, fresh}});
        api::Track saved = apiTrack(7);
        saved.trackAuthorization = QStringLiteral("old");
        saved.stale = true;
        api::Track unknown = apiTrack(8);
        unknown.stale = true;
        player.controller->playTracks({saved, unknown}, 0, {});
        REQUIRE(test::waitUntil([&] { return player.backend.loads.size() == 1; }));
        const QueueItem* now = player.controller->queue().currentItem();
        REQUIRE(now != nullptr);
        CHECK_FALSE(now->track.stale);
        CHECK(now->track.trackAuthorization == u"fresh");
        player.controller->next();
        REQUIRE(test::waitUntil([&] { return player.backend.loads.size() == 2; }));
        CHECK(player.loadedId(1) == u"8");
    }

    TEST_CASE("the backend's gapless advance moves the queue without loading again")
    {
        Player player;
        player.controller->playTracks({apiTrack(1), apiTrack(2)}, 0, {});
        player.backend.setPosition(180'000);
        REQUIRE(player.backend.next.has_value());
        CHECK(player.backend.next->info.key.trackId == u"2");
        QSignalSpy changed(player.controller.get(), &PlaybackController::trackChanged);
        Q_EMIT player.backend.advanced();
        CHECK(player.controller->queue().current() == 1);
        CHECK(player.controller->trackId() == u"2");
        CHECK(player.backend.loads.size() == 1);
        CHECK_FALSE(changed.isEmpty());
    }

    TEST_CASE("previous restarts a track after a few seconds, otherwise goes back")
    {
        Player player;
        player.controller->playTracks({apiTrack(1), apiTrack(2)}, 1, {});
        player.backend.setPosition(10'000);
        player.controller->previous();
        CHECK(player.backend.seeks == std::vector<std::int64_t>{0});
        CHECK(player.controller->trackId() == u"2");
        player.backend.setPosition(1000);
        player.controller->previous();
        CHECK(player.controller->trackId() == u"1");
        CHECK(player.loadedId(player.backend.loads.size() - 1) == u"1");
    }

    TEST_CASE("tracks known only by id are fetched before they play")
    {
        Player player;
        api::Track stub;
        stub.id = 7;
        stub.stub = true;
        player.client.known.emplace(7, apiTrack(7));
        player.controller->playTracks({stub}, 0, {});
        CHECK(player.backend.loads.empty());
        CHECK(player.controller->loading());
        REQUIRE(test::waitUntil([&] { return !player.backend.loads.empty(); }));
        CHECK(player.loadedId(0) == u"7");
        CHECK(player.controller->title() == u"Track 7");
    }

    TEST_CASE("a following track the lookup does not return is asked for once, then left to be skipped")
    {
        Player player;
        api::Track stub;
        stub.id = 2;
        stub.stub = true;  // deleted or private: /tracks?ids= leaves it out
        player.controller->playTracks({apiTrack(1), stub}, 0, {});
        REQUIRE(player.backend.loads.size() == 1);
        test::waitUntil([] { return false; }, 100);  // the hydration playTracks starts
        const int before = player.client.lookups;
        player.backend.setPosition(190'000);
        test::waitUntil([] { return false; }, 300);
        CHECK(player.client.lookups - before <= 1);
        CHECK_FALSE(player.backend.next.has_value());
    }

    TEST_CASE("stream URLs are resolved through the API when the cache asks, with the track authorization")
    {
        Player player;
        player.client.handler = [](const api::ApiRequest& request) -> Result<QByteArray> {
            if (request.url.path().endsWith(u"/stream/hls")) {
                return QByteArray(R"({"url":"https://cf-hls-media.sndcdn.com/playlist/x.m3u8?sig=1"})");
            }
            return fail(ErrorCode::NotFound, "unexpected");
        };
        player.controller->playTracks({apiTrack(5)}, 0, {});
        REQUIRE(player.backend.loads.size() == 1);
        CHECK(player.client.requests.empty());
        std::optional<Result<QUrl>> url;
        player.backend.loads[0].urls(false, [&](Result<QUrl> r) { url = std::move(r); });
        REQUIRE(test::waitUntil([&] { return url.has_value(); }));
        REQUIRE(*url);
        CHECK((*url)->host() == u"cf-hls-media.sndcdn.com");
        REQUIRE(player.client.requests.size() == 1);
        CHECK(player.client.requests[0].query().contains(QStringLiteral("track_authorization=auth-5")));
    }

    TEST_CASE("volume follows a loudness curve, mute is separate, and both survive a restart")
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("state/session.json"));
        {
            Player player(path);
            player.controller->setVolume(0.5);
            CHECK(player.backend.gain == doctest::Approx(0.125));
            player.controller->setMuted(true);
            CHECK(player.backend.gain == doctest::Approx(0.0));
            player.controller->setRepeat(QStringLiteral("all"));
            player.controller->setShuffle(true);
        }
        Player again(path);
        CHECK(again.controller->volume() == doctest::Approx(0.5));
        CHECK(again.controller->muted());
        CHECK(again.controller->repeat() == u"all");
        CHECK(again.controller->shuffle());

        // Without "remember volume", every session starts at the configured level.
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::RememberVolume, false));
        REQUIRE(store.setUserValue(settings::keys::StartupVolume, 40));
        FakeClient client;
        FakeBackend backend;
        PlaybackController controller({.settings = store, .api = client, .native = backend, .statePath = path});
        CHECK(controller.volume() == doctest::Approx(0.4));
    }

    TEST_CASE("the queue, the track and the position come back paused after a restart")
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("session.json"));
        {
            Player player(path);
            player.controller->playTracks({apiTrack(1), apiTrack(2), apiTrack(3)}, 1, QStringLiteral("Mix"));
            player.backend.setPosition(42'000);
        }
        // The tracks are looked up again (by id) when the next session starts.
        Player restored(path, {{1, apiTrack(1)}, {2, apiTrack(2)}, {3, apiTrack(3)}});
        REQUIRE(test::waitUntil([&] { return restored.controller->hasTrack(); }));
        CHECK(restored.controller->trackId() == u"2");
        CHECK(restored.controller->context() == u"Mix");
        CHECK(restored.controller->queue().size() == 3);
        CHECK(restored.controller->position() == 42'000);
        CHECK_FALSE(restored.controller->playing());
        CHECK(restored.backend.loads.empty());  // nothing plays until asked
        restored.controller->play();
        REQUIRE(restored.backend.loads.size() == 1);
        CHECK(restored.backend.startPositions[0] == 42'000);
    }

    TEST_CASE("the crossfade setting reaches the backend live")
    {
        Player player;
        CHECK(player.backend.crossfade == 0);
        REQUIRE(player.store.setUserValue(settings::keys::Crossfade, 6));
        CHECK(player.backend.crossfade == 6000);
    }
}

// --- MPRIS, session file, waveform ------------------------------------------------------------------

TEST_SUITE("media.mpris")
{
    TEST_CASE("metadata uses MPRIS types and a local cover file")
    {
        const QVariantMap map = mprisMetadata({.id = QStringLiteral("123"),
                                               .title = QStringLiteral("Ночь"),
                                               .artist = QStringLiteral("Artist"),
                                               .album = QStringLiteral("Set"),
                                               .durationMs = 2500,
                                               .artFile = QStringLiteral("/run/user/1/noctara/cover.png"),
                                               .url = QStringLiteral("https://soundcloud.com/a/b")});
        CHECK(map.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path() == u"/org/noctara/track/t123");
        CHECK(map.value(QStringLiteral("mpris:length")).toLongLong() == 2'500'000);
        CHECK(map.value(QStringLiteral("xesam:title")).toString() == u"Ночь");
        CHECK(map.value(QStringLiteral("xesam:artist")).toStringList() == QStringList{QStringLiteral("Artist")});
        CHECK(map.value(QStringLiteral("mpris:artUrl")).toString() == u"file:///run/user/1/noctara/cover.png");
        CHECK(mprisMetadata({}).value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path() ==
              u"/org/mpris/MediaPlayer2/TrackList/NoTrack");
        CHECK(mprisTrackPath(QStringLiteral("a-b.c")).path() == u"/org/noctara/track/ta_b_c");
    }

    TEST_CASE("a cover asked for twice at once is kept once, so a later eviction never deletes its current file")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::magenta);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        const auto url = [](char name) {
            return QStringLiteral("https://img.test/") + QLatin1Char(name) + QStringLiteral(".png");
        };
        for (const char name : {'a', 'b', 'c', 'd'}) {
            origin.serve(url(name), {.body = png, .type = "image/png"});
        }
        cache::ImageCache images(origin, cache::FileOps::posix(), 16 << 20);
        CoverFiles covers(images, dir.filePath(QStringLiteral("covers-1")));
        const auto fetch = [&](char name) {
            std::optional<QString> path;
            covers.request(url(name), [&](const QString& file) { path = file; });
            REQUIRE(test::waitUntil([&] { return path.has_value(); }));
            return *path;
        };
        // MPRIS and the notification both ask for the new track's cover in the same track change.
        int answered = 0;
        covers.request(url('a'), [&](const QString&) { ++answered; });
        covers.request(url('a'), [&](const QString&) { ++answered; });
        REQUIRE(test::waitUntil([&] { return answered == 2; }));
        for (const char name : {'b', 'c', 'd'}) {
            CHECK(QFileInfo::exists(fetch(name)));
        }
        const QString again = fetch('a');
        REQUIRE_FALSE(again.isEmpty());
        CHECK(QFileInfo::exists(again));
    }
}

TEST_SUITE("media.session")
{
    TEST_CASE("the player state round-trips; damaged content gives the defaults")
    {
        const SessionState state{.volume = 0.3,
                                 .muted = true,
                                 .shuffle = true,
                                 .repeat = Repeat::One,
                                 .queue = {5, 9, 2},
                                 .current = 1,
                                 .positionMs = 61000,
                                 .context = QStringLiteral("Mix")};
        CHECK(SessionState::fromJson(state.toJson()) == state);
        CHECK(SessionState::fromJson(R"({"version":1,"queue":[3,-1,"x",4],"current":9})").queue ==
              std::vector<qint64>{3, 4});
        CHECK(SessionState::fromJson("{nonsense") == SessionState{});
        CHECK(SessionState::fromJson(R"({"version":1,"volume":7})").volume == doctest::Approx(1.0));
    }
}

TEST_SUITE("ui.waveform")
{
    TEST_CASE("bars are the peaks of their share of the samples, never below the floor")
    {
        const QList<float> samples{0.0F, 0.9F, 0.2F, 0.1F, 0.5F, 0.4F};
        CHECK(ui::waveformBars(samples, 3, 0.1F) == std::vector<float>{0.9F, 0.2F, 0.5F});
        CHECK(ui::waveformBars({}, 2, 0.1F) == std::vector<float>{0.1F, 0.1F});
        const std::vector<float> stretched = ui::waveformBars(samples, 12, 0.05F);
        CHECK(stretched.size() == 12);
        CHECK(static_cast<double>(stretched[2]) == doctest::Approx(0.9));
    }

    TEST_CASE("a bar counts as stored when its centre lies in a stored range")
    {
        CHECK(ui::storedBars({0.0, 0.5}, 4) == std::vector<bool>{true, true, false, false});
        CHECK(ui::storedBars({0.3, 0.4, 0.8, 1.0}, 5) == std::vector<bool>{false, true, false, false, true});
    }
}
