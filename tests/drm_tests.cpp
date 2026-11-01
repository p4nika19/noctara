#include "Doctest.hpp"
#include "TestUtil.hpp"
#include "mock/FakeOrigin.hpp"

#include "drm/DrmBackend.hpp"
#include "drm/DrmHost.hpp"
#include "drm/HelperInstaller.hpp"
#include "drm/HelperNetwork.hpp"
#include "drm/Protocol.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <csignal>
#include <map>

using namespace noctara;
using namespace noctara::drm;
using namespace std::chrono_literals;

namespace {

// Sets environment variables for the helpers started meanwhile, and clears them again.
class ScopedEnv {
public:
    ScopedEnv(std::initializer_list<std::pair<const char*, QByteArray>> values)
    {
        for (const auto& [name, value] : values) {
            qputenv(name, value);
            m_names.push_back(name);
        }
    }
    ~ScopedEnv()
    {
        for (const char* name : m_names) {
            qunsetenv(name);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    std::vector<const char*> m_names;
};

DrmHost::Options mockOptions(const QString& profile)
{
    return DrmHost::Options{
        // tests/mock-drm-helper, built next to the tests.
        .command = HelperCommand{.program = QCoreApplication::applicationDirPath() +
                                            QStringLiteral("/noctara-mock-drm-helper"),
                                 .arguments = {}},
        .profileDir = profile,
        .gpu = false,
        .idleTimeout = 1min,
        .heartbeat = 100ms,
        .heartbeatTimeout = 1s,
        .startTimeout = 10s,
        .shutdownGrace = 1s,
        .restartBackoff = 20ms,
    };
}

// What the mock helper reported: its arguments and the messages it received.
struct Transcript {
    explicit Transcript(DrmHost& host)
    {
        QObject::connect(&host, &DrmHost::received, &context, [this](const QJsonObject& message) {
            if (message.value(u"type").toString() != u"log") {
                events.push_back(message);
                return;
            }
            const QString text = message.value(u"message").toString();
            if (text.startsWith(u"args ")) {
                arguments.clear();
                for (const QJsonValue& value : QJsonDocument::fromJson(text.mid(5).toUtf8()).array()) {
                    arguments << value.toString();
                }
            } else if (text.startsWith(u"got ")) {
                received.push_back(QJsonDocument::fromJson(text.mid(4).toUtf8()).object());
            } else if (text.startsWith(u"child ")) {
                child = text.mid(6).toLongLong();
            }
        });
    }

    [[nodiscard]] std::vector<QJsonObject> ofType(const QString& type) const
    {
        std::vector<QJsonObject> found;
        for (const QJsonObject& message : received) {
            if (message.value(u"type").toString() == type) {
                found.push_back(message);
            }
        }
        return found;
    }

    QObject context;
    QStringList arguments;
    std::vector<QJsonObject> received;
    std::vector<QJsonObject> events;
    qint64 child = 0;
};

QJsonObject ping()
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("seq"), 1}};
}

bool processGone(qint64 pid)
{
    return test::waitUntil([pid] { return ::kill(static_cast<pid_t>(pid), 0) != 0; }, 5000);
}

net::NetworkConfig directConfig()
{
    net::NetworkConfig config;
    config.proxy.mode = net::ProxyMode::None;
    return config;
}

media::PlaybackSource protectedSource(std::function<Result<api::ProtectedStream>()> answer)
{
    media::PlaybackSource source;
    source.info.key = {.trackId = QStringLiteral("7"), .preset = QStringLiteral("ctr-encrypted-hls/aac_160k/hq")};
    source.info.drm = true;
    source.protectedStream = [answer = std::move(answer)](QObject* context,
                                                          std::function<void(Result<api::ProtectedStream>)> done) {
        QMetaObject::invokeMethod(context, [done, result = answer()] { done(result); }, Qt::QueuedConnection);
    };
    return source;
}

api::ProtectedStream stream(const QString& url = QStringLiteral("https://playback.media-streaming.soundcloud.cloud/x"))
{
    return api::ProtectedStream{.url = QUrl(url), .licenseToken = QStringLiteral("license-token")};
}

}  // namespace

TEST_SUITE("drm.protocol")
{
    TEST_CASE("messages are checked against the shared definition in both directions")
    {
        CHECK(protocolVersion() == 1);
        CHECK(validate(Direction::ToHelper, ping()).isEmpty());
        CHECK_FALSE(validate(Direction::FromHelper, ping()).isEmpty());  // a ping never comes from the helper
        CHECK(validate(Direction::ToHelper, {{QStringLiteral("type"), QStringLiteral("ping")}}).contains(u"missing"));
        CHECK(validate(Direction::ToHelper,
                       {{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("seq"), QStringLiteral("1")}})
                  .contains(u"must be a number"));
        CHECK(validate(Direction::ToHelper, {{QStringLiteral("type"), QStringLiteral("format-disk")}})
                  .contains(u"unknown"));
        // Optional fields may be left out but must have the right type when present.
        const QJsonObject ready{{QStringLiteral("type"), QStringLiteral("ready")},
                                {QStringLiteral("version"), 1},
                                {QStringLiteral("widevine"), true},
                                {QStringLiteral("electron"), QStringLiteral("44")}};
        CHECK(validate(Direction::FromHelper, ready).isEmpty());
        QJsonObject withDetail = ready;
        withDetail.insert(QStringLiteral("detail"), 3);
        CHECK_FALSE(validate(Direction::FromHelper, withDetail).isEmpty());

        CHECK(test::errorOf(decode("not json", Direction::FromHelper)) == ErrorCode::ParseError);
        CHECK(test::errorOf(decode(R"({"type":"pong"})", Direction::FromHelper)) == ErrorCode::InvalidResponse);
        const Result<QJsonObject> pong = decode(R"({"type":"pong","seq":4})", Direction::FromHelper);
        REQUIRE(pong.has_value());
        CHECK(encode(*pong) == R"({"seq":4,"type":"pong"})"
                               "\n");
    }
}

TEST_SUITE("drm.network")
{
    TEST_CASE("proxy settings become startup switches and the live configuration")
    {
        net::NetworkConfig config;
        config.proxy.mode = net::ProxyMode::System;
        HelperNetwork plan = planNetwork(config);
        CHECK(plan.switches.isEmpty());
        CHECK(plan.configure.value(u"proxy").toObject().value(u"mode").toString() == u"system");
        CHECK(validate(Direction::ToHelper, plan.configure).isEmpty());

        config = directConfig();
        CHECK(planNetwork(config).switches == QStringList{QStringLiteral("--no-proxy-server")});

        config.proxy.mode = net::ProxyMode::Manual;
        config.proxy.manual = *net::ProxyServer::fromUrl(u"http://proxy.example:3128");
        config.proxy.manual.username = QStringLiteral("user");
        config.proxy.manual.password = QStringLiteral("secret");
        config.proxy.bypass = {QStringLiteral("*.corp")};
        plan = planNetwork(config);
        CHECK(plan.switches.contains(u"--proxy-server=http://proxy.example:3128"));
        CHECK(plan.switches.contains(u"--proxy-bypass-list=*.corp;<local>"));
        const QJsonObject proxy = plan.configure.value(u"proxy").toObject();
        CHECK(proxy.value(u"mode").toString() == u"manual");
        CHECK(proxy.value(u"username").toString() == u"user");
        CHECK(proxy.value(u"password").toString() == u"secret");
        CHECK_FALSE(proxy.value(u"display").toString().contains(u"secret"));
        for (const QString& option : plan.switches) {
            CHECK_FALSE(option.contains(u"secret"));  // credentials only go over the pipe
        }
        CHECK(plan.limits.empty());
        CHECK_FALSE(plan.blocked);

        // Chromium cannot sign in to SOCKS proxies, and speaks SOCKS4 for SOCKS4a.
        config.proxy.manual = *net::ProxyServer::fromUrl(u"socks5h://[2001:db8::1]:1080");
        config.proxy.manual.username = QStringLiteral("user");
        plan = planNetwork(config);
        CHECK(plan.switches.contains(u"--proxy-server=socks5://[2001:db8::1]:1080"));
        CHECK_FALSE(plan.configure.value(u"proxy").toObject().contains(u"username"));
        CHECK(std::ranges::find(plan.limits, NetworkLimit::SocksCredentials) != plan.limits.end());
        config.proxy.manual = *net::ProxyServer::fromUrl(u"socks4a://proxy.example:1080");
        plan = planNetwork(config);
        CHECK(plan.switches.contains(u"--proxy-server=socks4://proxy.example:1080"));
        CHECK(std::ranges::find(plan.limits, NetworkLimit::ProxyResolvesHere) != plan.limits.end());

        config.proxy.manual = {};
        CHECK(planNetwork(config).blocked->code == ErrorCode::ProxyNotConfigured);
    }

    TEST_CASE("DNS over HTTPS applies live with its bootstrap mapped; strict custom DNS blocks protected playback")
    {
        net::NetworkConfig config = directConfig();
        config.dns.mode = net::DnsMode::Doh;
        config.dns.doh.url = QUrl(QStringLiteral("https://dns.example/dns-query"));
        config.dns.doh.bootstrap = {*IpAddress::parse(u"192.0.2.53")};
        config.dns.fallbackToSystem = false;
        config.dns.overrides = {{QStringLiteral("api.example"), *IpAddress::parse(u"2001:db8::7")}};
        HelperNetwork plan = planNetwork(config);
        const QJsonObject dns = plan.configure.value(u"dns").toObject();
        CHECK(dns.value(u"mode").toString() == u"doh");
        CHECK(dns.value(u"template").toString() == u"https://dns.example/dns-query");
        CHECK(dns.value(u"strict").toBool());
        CHECK(plan.switches.contains(u"--noctara-doh=https://dns.example/dns-query"));
        CHECK(plan.switches.contains(u"--noctara-doh-strict"));
        CHECK(
            plan.switches.contains(u"--host-resolver-rules=MAP api.example [2001:db8::7],MAP dns.example 192.0.2.53"));
        CHECK_FALSE(plan.blocked);

        config.dns.mode = net::DnsMode::Custom;
        config.dns.servers = {*Endpoint::parse(u"192.0.2.1", 53)};
        config.dns.fallbackToSystem = true;
        plan = planNetwork(config);
        CHECK(std::ranges::find(plan.limits, NetworkLimit::CustomDns) != plan.limits.end());
        CHECK_FALSE(plan.blocked);
        config.dns.fallbackToSystem = false;
        CHECK(planNetwork(config).blocked->code == ErrorCode::ProtectedNetworkUnsupported);

        // A proxy that resolves names (given by address) leaves nothing for the local resolver.
        config.proxy.mode = net::ProxyMode::Manual;
        config.proxy.manual = *net::ProxyServer::fromUrl(u"socks5h://192.0.2.9:1080");
        plan = planNetwork(config);
        CHECK_FALSE(plan.blocked);
        CHECK(plan.limits.empty());
        config.proxy.manual = *net::ProxyServer::fromUrl(u"socks5h://proxy.example:1080");
        CHECK(planNetwork(config).blocked);  // its own name would need the system resolver
    }

    TEST_CASE("transport settings: switches where Chromium has them, limits where not, offline blocks")
    {
        net::NetworkConfig config = directConfig();
        config.transport.tls13Only = true;
        config.transport.httpVersion = net::HttpVersion::Http1_1;
        config.transport.caBundle = QStringLiteral("/etc/corp.pem");
        config.transport.bandwidthBytesPerSecond = 100'000;
        config.dns.family = net::IpFamilyPreference::V4Only;
        HelperNetwork plan = planNetwork(config);
        CHECK(plan.switches.contains(u"--ssl-version-min=tls1.3"));
        CHECK(plan.switches.contains(u"--disable-http2"));
        CHECK(plan.limits.size() == 3);
        config.transport.offline = true;
        CHECK(planNetwork(config).blocked->code == ErrorCode::Offline);
    }
}

TEST_SUITE("drm.host")
{
    TEST_CASE("starts on the first message with hello and the network first; idles out; restarts for new switches")
    {
        QTemporaryDir profile;
        DrmHost::Options options = mockOptions(profile.path() + QStringLiteral("/electron"));
        options.idleTimeout = 400ms;
        DrmHost host(options);
        Transcript transcript(host);
        host.setNetwork(planNetwork(directConfig()));
        QSignalSpy ready(&host, &DrmHost::ready);
        QSignalSpy stopped(&host, &DrmHost::stopped);
        CHECK(host.status() == DrmHost::Status::Stopped);
        CHECK_FALSE(host.running());

        REQUIRE(host.send(ping()));
        REQUIRE(test::waitUntil([&] { return ready.size() == 1; }, 10000));
        CHECK(host.status() == DrmHost::Status::Ready);
        CHECK(host.electronVersion() == u"mock");
        CHECK(QDir(options.profileDir).exists());
        REQUIRE(test::waitUntil([&] { return transcript.received.size() >= 3; }));
        CHECK(transcript.received[0].value(u"type").toString() == u"hello");
        CHECK(transcript.received[1].value(u"type").toString() == u"configure");
        CHECK(transcript.arguments.contains(QStringLiteral("--noctara-user-data=") + options.profileDir));
        CHECK(transcript.arguments.contains(u"--ozone-platform=headless"));
        CHECK(transcript.arguments.contains(u"--disable-gpu"));
        CHECK(transcript.arguments.contains(u"--no-proxy-server"));
        // The heartbeat runs.
        REQUIRE(test::waitUntil([&] { return transcript.ofType(QStringLiteral("ping")).size() >= 3; }));
        const qint64 first = host.processId();

        // A live change that keeps the switches (here: the proxy credentials) is applied through "configure".
        net::NetworkConfig config = directConfig();
        config.proxy.mode = net::ProxyMode::Manual;
        config.proxy.manual = *net::ProxyServer::fromUrl(u"http://192.0.2.8:3128");
        host.setBusy(true);
        host.setNetwork(planNetwork(config));  // new switches: restarted below
        REQUIRE(test::waitUntil([&] { return ready.size() == 2; }, 10000));
        REQUIRE(stopped.size() == 1);
        config.proxy.manual.username = QStringLiteral("user");
        config.proxy.manual.password = QStringLiteral("pass");
        host.setNetwork(planNetwork(config));
        REQUIRE(test::waitUntil([&] {
            const auto configures = transcript.ofType(QStringLiteral("configure"));
            return !configures.empty() &&
                   configures.back().value(u"proxy").toObject().value(u"password").toString() == u"pass";
        }));
        CHECK(stopped.size() == 1);
        host.setBusy(false);

        // New startup switches restarted it (reported as unexpected, so a loaded track resumes); being busy, it
        // started again at once.
        CHECK(stopped.first().first().toBool());
        CHECK(processGone(first));
        CHECK(transcript.arguments.contains(u"--proxy-server=http://192.0.2.8:3128"));

        // Not busy: it closes itself after the idle timeout.
        host.setBusy(false);
        const qint64 second = host.processId();
        REQUIRE(test::waitUntil([&] { return stopped.size() == 2; }, 5000));
        CHECK_FALSE(stopped.last().first().toBool());
        CHECK(host.status() == DrmHost::Status::Stopped);
        CHECK(processGone(second));
    }

    TEST_CASE("the whole process group goes when the host does: no orphans")
    {
        QTemporaryDir profile;
        ScopedEnv env{{"NOCTARA_MOCK_DRM_CHILD", "1"}};
        qint64 pid = 0;
        qint64 child = 0;
        {
            DrmHost host(mockOptions(profile.path()));
            Transcript transcript(host);
            QSignalSpy ready(&host, &DrmHost::ready);
            REQUIRE(host.send(ping()));
            REQUIRE(test::waitUntil([&] { return ready.size() == 1 && transcript.child > 0; }, 10000));
            pid = host.processId();
            child = transcript.child;
            CHECK(::kill(static_cast<pid_t>(child), 0) == 0);
        }
        CHECK(processGone(pid));
        CHECK(processGone(child));
    }

    TEST_CASE("a crash while busy restarts it with backoff; repeated crashes give up")
    {
        QTemporaryDir profile;
        ScopedEnv env{{"NOCTARA_MOCK_DRM_CRASH_ON_LOAD", "1"}};
        DrmHost host(mockOptions(profile.path()));
        QSignalSpy ready(&host, &DrmHost::ready);
        QSignalSpy stopped(&host, &DrmHost::stopped);
        QSignalSpy failed(&host, &DrmHost::failed);
        host.setBusy(true);
        const QJsonObject load{{QStringLiteral("type"), QStringLiteral("load")},
                               {QStringLiteral("id"), QStringLiteral("1")},
                               {QStringLiteral("url"), QStringLiteral("https://x/")},
                               {QStringLiteral("licenseUrl"), QStringLiteral("https://l/")},
                               {QStringLiteral("licenseToken"), QStringLiteral("t")},
                               {QStringLiteral("startMs"), 0},
                               {QStringLiteral("volume"), 1},
                               {QStringLiteral("autoplay"), true}};
        for (int crash = 1; crash < DrmHost::kMaxCrashes; ++crash) {
            REQUIRE(host.send(load));
            REQUIRE(test::waitUntil([&] { return stopped.size() == crash; }, 10000));
            CHECK(stopped.last().first().toBool());
            REQUIRE(test::waitUntil([&] { return ready.size() == crash; }, 10000));  // restarted by itself
        }
        REQUIRE(host.send(load));
        REQUIRE(test::waitUntil([&] { return failed.size() == 1; }, 10000));
        CHECK(failed.first().first().value<Error>().code == ErrorCode::ProtectedPlayerFailed);
        CHECK(host.status() == DrmHost::Status::Failed);
    }

    TEST_CASE("a helper that stops answering is restarted")
    {
        QTemporaryDir profile;
        ScopedEnv env{{"NOCTARA_MOCK_DRM_SILENT", "1"}};
        DrmHost host(mockOptions(profile.path()));
        QSignalSpy stopped(&host, &DrmHost::stopped);
        REQUIRE(host.send(ping()));
        REQUIRE(test::waitUntil([&] { return !stopped.isEmpty(); }, 10000));
        CHECK(stopped.first().first().toBool());
    }

    TEST_CASE("missing helper, no Widevine, another protocol version and blocked networks fail cleanly")
    {
        QTemporaryDir profile;
        {
            DrmHost::Options options = mockOptions(profile.path());
            options.command.reset();
            DrmHost host(options);
            QSignalSpy failed(&host, &DrmHost::failed);
            CHECK_FALSE(host.send(ping()));
            REQUIRE(failed.size() == 1);
            CHECK(host.lastError().code == ErrorCode::ProtectedPlayerMissing);
        }
        {
            DrmHost host(mockOptions(profile.path()));
            net::NetworkConfig config = directConfig();
            config.transport.offline = true;
            host.setNetwork(planNetwork(config));
            CHECK_FALSE(host.send(ping()));
            CHECK(host.lastError().code == ErrorCode::Offline);
            CHECK_FALSE(host.running());
        }
        for (const auto& [name, value] : std::map<const char*, QByteArray>{{"NOCTARA_MOCK_DRM_WIDEVINE", "0"},
                                                                           {"NOCTARA_MOCK_DRM_VERSION", "99"}}) {
            ScopedEnv env{{name, value}};
            DrmHost host(mockOptions(profile.path()));
            QSignalSpy failed(&host, &DrmHost::failed);
            REQUIRE(host.send(ping()));
            REQUIRE(test::waitUntil([&] { return !failed.isEmpty(); }, 10000));
            CHECK(host.lastError().code == ErrorCode::ProtectedPlayerFailed);
            REQUIRE(test::waitUntil([&] { return !host.running() && host.processId() == 0; }, 5000));
        }
        CHECK_FALSE(helperIn(profile.path()));
        CHECK_FALSE(findHelper(profile.path(), {}));
    }

    TEST_CASE("real drm helper: castlabs Electron starts headless with Widevine" * doctest::skip())
    {
        const auto command = helperIn(QStringLiteral(NOCTARA_SOURCE_DIR "/build/drm-helper"));
        REQUIRE_MESSAGE(command, "run `xmake drm-helper` first");
        QTemporaryDir profile;
        DrmHost::Options options = mockOptions(profile.path());
        options.command = command;
        options.heartbeat = 5s;
        options.heartbeatTimeout = 30s;
        DrmHost host(options);
        host.setNetwork(planNetwork(net::NetworkConfig{}));
        QSignalSpy ready(&host, &DrmHost::ready);
        QSignalSpy failed(&host, &DrmHost::failed);
        REQUIRE(host.send(ping()));
        REQUIRE(test::waitUntil([&] { return !ready.isEmpty() || !failed.isEmpty(); }, 120000));
        CHECK(failed.isEmpty());
        CHECK(host.status() == DrmHost::Status::Ready);
    }
}

TEST_SUITE("drm.backend")
{
    TEST_CASE("plays, pauses, seeks and ends through the helper; the license token goes only to it")
    {
        QTemporaryDir profile;
        ScopedEnv env{{"NOCTARA_MOCK_DRM_DURATION_MS", "1500"}};
        DrmHost host(mockOptions(profile.path()));
        host.setNetwork(planNetwork(directConfig()));
        Transcript transcript(host);
        DrmBackend backend(host);
        QSignalSpy ended(&backend, &DrmBackend::ended);
        QSignalSpy durations(&backend, &DrmBackend::durationKnown);
        backend.setGain(0.25);
        int resolutions = 0;
        backend.load(protectedSource([&] {
                         ++resolutions;
                         return Result<api::ProtectedStream>(stream());
                     }),
                     200, false);
        CHECK(backend.state() == media::BackendState::Loading);
        CHECK(backend.position() == 200);
        REQUIRE(test::waitUntil([&] { return backend.state() == media::BackendState::Playing; }, 10000));
        const std::vector<QJsonObject> loads = transcript.ofType(QStringLiteral("load"));
        REQUIRE(loads.size() == 1);
        CHECK(loads[0].value(u"licenseToken").toString() == u"license-token");
        CHECK(loads[0].value(u"licenseUrl").toString() ==
              u"https://license.media-streaming.soundcloud.cloud/playback/widevine");
        CHECK(loads[0].value(u"startMs").toInt() == 200);
        CHECK(loads[0].value(u"volume").toDouble() == doctest::Approx(0.25));
        CHECK(backend.cachedRanges().empty());
        REQUIRE(test::waitUntil([&] { return !durations.isEmpty(); }));
        CHECK(durations.first().first().toLongLong() == 1500);

        backend.pause();
        CHECK(backend.state() == media::BackendState::Paused);
        backend.seek(1000);
        CHECK(backend.position() == 1000);
        REQUIRE(test::waitUntil([&] { return !transcript.ofType(QStringLiteral("seek")).empty(); }));
        backend.play();
        REQUIRE(test::waitUntil([&] { return ended.size() == 1; }, 5000));
        CHECK(backend.state() == media::BackendState::Ended);
        CHECK(backend.position() == 1500);
        CHECK(resolutions == 1);

        backend.stop();
        CHECK(backend.state() == media::BackendState::Idle);
    }

    TEST_CASE("a crash resumes the track where it was with a freshly resolved stream")
    {
        QTemporaryDir profile;
        ScopedEnv env{{"NOCTARA_MOCK_DRM_CRASH_ONCE", (profile.path() + QStringLiteral("/crashed")).toUtf8()},
                      {"NOCTARA_MOCK_DRM_DURATION_MS", "60000"}};
        DrmHost host(mockOptions(profile.path()));
        host.setNetwork(planNetwork(directConfig()));
        DrmBackend backend(host);
        int resolutions = 0;
        backend.load(protectedSource([&] {
                         ++resolutions;
                         return Result<api::ProtectedStream>(stream());
                     }),
                     5000, false);
        REQUIRE(test::waitUntil([&] { return backend.state() == media::BackendState::Playing; }, 15000));
        CHECK(resolutions == 2);
        CHECK(backend.position() >= 5000);
    }

    TEST_CASE("a new load unloads the previous track at once, even when it is stopped or fails before reaching "
              "the helper")
    {
        QTemporaryDir profile;
        ScopedEnv env{{"NOCTARA_MOCK_DRM_DURATION_MS", "60000"}};
        DrmHost host(mockOptions(profile.path()));
        host.setNetwork(planNetwork(directConfig()));
        Transcript transcript(host);
        DrmBackend backend(host);
        QSignalSpy failed(&backend, &DrmBackend::failed);
        const auto playFirst = [&] {
            backend.load(protectedSource([] { return Result<api::ProtectedStream>(stream()); }), 0, false);
            REQUIRE(test::waitUntil([&] { return backend.state() == media::BackendState::Playing; }, 10000));
        };

        // Stopped while the next track's stream is still being resolved.
        playFirst();
        media::PlaybackSource pending;
        pending.info.drm = true;
        pending.protectedStream = [](QObject*, const std::function<void(Result<api::ProtectedStream>)>&) {
        };
        backend.load(pending, 0, false);
        backend.stop();
        REQUIRE(test::waitUntil([&] { return transcript.ofType(QStringLiteral("stop")).size() == 1; }, 5000));

        // The next track's resolution fails.
        playFirst();
        backend.load(protectedSource([] { return Result<api::ProtectedStream>(fail(ErrorCode::Forbidden)); }), 0,
                     false);
        REQUIRE(test::waitUntil([&] { return failed.size() == 1; }));
        REQUIRE(test::waitUntil([&] { return transcript.ofType(QStringLiteral("stop")).size() == 2; }, 5000));
    }

    TEST_CASE("a refused license, a failed resolution and a missing helper fail the track")
    {
        QTemporaryDir profile;
        DrmHost host(mockOptions(profile.path()));
        host.setNetwork(planNetwork(directConfig()));
        DrmBackend backend(host);
        QSignalSpy failed(&backend, &DrmBackend::failed);
        backend.load(protectedSource([] {
                         return Result<api::ProtectedStream>(stream(
                             QStringLiteral("https://playback.media-streaming.soundcloud.cloud/license-denied")));
                     }),
                     0, false);
        REQUIRE(test::waitUntil([&] { return failed.size() == 1; }, 10000));
        CHECK(failed.last().first().value<Error>().code == ErrorCode::LicenseDenied);
        CHECK(backend.state() == media::BackendState::Failed);

        backend.load(protectedSource([] { return Result<api::ProtectedStream>(fail(ErrorCode::Forbidden)); }), 0,
                     false);
        REQUIRE(test::waitUntil([&] { return failed.size() == 2; }));
        CHECK(failed.last().first().value<Error>().code == ErrorCode::Forbidden);

        host.setCommand(std::nullopt);
        host.shutdown();
        REQUIRE(test::waitUntil([&] { return !host.running() && host.processId() == 0; }, 5000));
        backend.load(protectedSource([] { return Result<api::ProtectedStream>(stream()); }), 0, false);
        REQUIRE(test::waitUntil([&] { return failed.size() == 3; }));
        CHECK(failed.last().first().value<Error>().code == ErrorCode::ProtectedPlayerMissing);
    }
}

namespace {

test::FakeOrigin::Resource resource(QByteArray body)
{
    test::FakeOrigin::Resource served;
    served.body = std::move(body);
    return served;
}

QByteArray fixture(const char* name)
{
    QFile file(QStringLiteral(NOCTARA_SOURCE_DIR "/tests/fixtures/drm/") + QString::fromLatin1(name));
    REQUIRE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

Artifact artifactFor(const QString& file, const QByteArray& body)
{
    return Artifact{.version = QStringLiteral("1.0+test"),
                    .file = file,
                    .url = QUrl(QStringLiteral("https://downloads.test/") + file),
                    .sha256 = QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex(),
                    .size = body.size()};
}

}  // namespace

TEST_SUITE("drm.installer")
{
    TEST_CASE("zips unpack with their permissions, and never outside their folder")
    {
        QTemporaryDir dir;
        const QString runtime = dir.filePath(QStringLiteral("runtime"));
        QFile zip(dir.filePath(QStringLiteral("runtime.zip")));
        REQUIRE(zip.open(QIODevice::WriteOnly));
        zip.write(fixture("runtime.zip"));
        zip.close();
        REQUIRE(extractZip(zip.fileName(), runtime));
        CHECK(QFileInfo(runtime + QStringLiteral("/electron")).isExecutable());
        CHECK(QFileInfo(runtime + QStringLiteral("/resources/default_app.asar")).size() == 100000);

        QFile escape(dir.filePath(QStringLiteral("escape.zip")));
        REQUIRE(escape.open(QIODevice::WriteOnly));
        escape.write(fixture("escape.zip"));
        escape.close();
        const QString inside = dir.filePath(QStringLiteral("a/b"));
        REQUIRE(QDir().mkpath(inside));
        CHECK_FALSE(extractZip(escape.fileName(), inside));
        CHECK_FALSE(QFile::exists(dir.filePath(QStringLiteral("a/escaped.txt"))));
    }

    TEST_CASE("real drm runtime: the castlabs zip unpacks" * doctest::skip())
    {
        const QString zip = QStringLiteral(NOCTARA_SOURCE_DIR "/build/downloads/") + helperRelease().electron.file;
        REQUIRE_MESSAGE(QFile::exists(zip), "run `xmake drm-helper` first");
        QTemporaryDir dir;
        const Result<void> unpacked = extractZip(zip, dir.path());
        REQUIRE_MESSAGE(unpacked, test::describe(unpacked));
        CHECK(QFileInfo(dir.filePath(QStringLiteral("electron"))).isExecutable());
    }

    TEST_CASE("the player is downloaded through Noctara's transport, resumed after a cut, checked and installed")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        const QByteArray runtime = fixture("runtime.zip");
        const QByteArray hls = "/* hls.js */";
        const HelperRelease release{.electron = artifactFor(QStringLiteral("runtime.zip"), runtime),
                                    .hlsjs = artifactFor(QStringLiteral("hls.min.js"), hls)};
        origin.serve(release.electron.url.toString(), resource(runtime));
        origin.serve(release.hlsjs.url.toString(), resource(hls));
        HelperInstaller installer(origin, dir.path(), release);
        QSignalSpy finished(&installer, &HelperInstaller::finished);
        CHECK_FALSE(installer.installed());

        origin.failAfter(40'000);  // the connection drops part-way
        installer.install(false);
        REQUIRE(test::waitUntil([&] { return finished.size() == 1; }, 10000));
        CHECK_FALSE(finished.last().first().toBool());
        CHECK(installer.phase() == HelperInstaller::Phase::Failed);
        CHECK_FALSE(installer.installed());

        origin.failAfter(std::nullopt);
        installer.install(false);
        REQUIRE(test::waitUntil([&] { return finished.size() == 2; }, 10000));
        REQUIRE(finished.last().first().toBool());
        bool resumed = false;
        for (const net::Request& request : origin.requests()) {
            resumed = resumed || (request.url == release.electron.url && request.range && request.range->first > 0);
        }
        CHECK(resumed);
        const std::optional<HelperCommand> command = installer.installed();
        REQUIRE(command.has_value());
        CHECK(command->program.endsWith(u"/electron/electron"));
        CHECK(QFileInfo(command->program).isExecutable());
        REQUIRE(command->arguments.size() == 1);
        const QString app = command->arguments.front();
        for (const char* file : {"main.js", "preload.js", "player.html", "protocol.json", "package.json"}) {
            CHECK(QFile::exists(app + u'/' + QString::fromLatin1(file)));
        }
        QFile vendored(app + QStringLiteral("/vendor/hls.min.js"));
        REQUIRE(vendored.open(QIODevice::ReadOnly));
        CHECK(vendored.readAll() == hls);
        CHECK_FALSE(QDir(installer.root() + QStringLiteral("/downloads")).exists());
    }

    TEST_CASE("a download that does not match its pinned SHA-256 is refused and not kept")
    {
        QTemporaryDir dir;
        test::FakeOrigin origin;
        const QByteArray runtime = fixture("runtime.zip");
        HelperRelease release{.electron = artifactFor(QStringLiteral("runtime.zip"), runtime),
                              .hlsjs = artifactFor(QStringLiteral("hls.min.js"), "x")};
        release.electron.sha256 = QByteArray(64, 'a');
        origin.serve(release.electron.url.toString(), resource(runtime));
        origin.serve(release.hlsjs.url.toString(), resource("x"));
        HelperInstaller installer(origin, dir.path(), release);
        QSignalSpy finished(&installer, &HelperInstaller::finished);
        installer.install(false);
        REQUIRE(test::waitUntil([&] { return finished.size() == 1; }, 10000));
        CHECK_FALSE(finished.last().first().toBool());
        CHECK(installer.error().code == ErrorCode::Corrupt);
        CHECK(QDir(installer.root() + QStringLiteral("/downloads")).isEmpty());
    }
}
