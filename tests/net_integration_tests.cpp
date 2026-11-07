// The resolver and transport against local mock servers: DNS modes, fallback and strict mode, proxies,
// TLS, retries, redirects, streaming, cancellation and the "curl never resolves by itself" guarantee.
#include "TestUtil.hpp"
#include "mock/MockServers.hpp"

#include "net/CurlTransport.hpp"
#include "net/DnsResolver.hpp"
#include "net/NetworkService.hpp"
#include "net/ProxyRouter.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QElapsedTimer>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <map>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace noctara;
using namespace noctara::net;
using namespace std::chrono_literals;

namespace {

// Stands in for getaddrinfo: counts calls and answers every name with a "poisoned" address, so any leak
// to the system resolver is both counted and visible in the result.
class FakeSystemResolver : public QObject, public ISystemResolver {
public:
    void lookup(const QString& host, std::function<void(Result<std::vector<IpAddress>>)> callback) override
    {
        ++calls;
        hosts.append(host);
        Result<std::vector<IpAddress>> result = std::vector{*IpAddress::parse(u"203.0.113.66")};
        if (const auto it = table.find(host); it != table.end()) {
            result = it->second;
        }
        QMetaObject::invokeMethod(this, [callback, result] { callback(result); }, Qt::QueuedConnection);
    }

    int calls = 0;
    QStringList hosts;
    std::map<QString, Result<std::vector<IpAddress>>> table;
};

IpAddress ip(const char* text)
{
    return *IpAddress::parse(QString::fromLatin1(text));
}

Result<Resolution> resolveNow(DnsResolver& resolver, const QString& host)
{
    std::optional<Result<Resolution>> out;
    resolver.resolve(host, [&](Result<Resolution> result) { out = std::move(result); });
    REQUIRE(test::waitUntil([&] { return out.has_value(); }, 10000));
    return *out;
}

DnsConfig customServers(std::vector<std::uint16_t> ports, bool fallback)
{
    DnsConfig config;
    config.mode = DnsMode::Custom;
    for (const std::uint16_t port : ports) {
        config.servers.push_back({ip("127.0.0.1"), port});
    }
    config.fallbackToSystem = fallback;
    return config;
}

// A UDP port with nothing listening: queries are refused immediately (ICMP port unreachable).
std::uint16_t deadPort()
{
    const test::TcpServer probe([](int) {});
    return probe.port();
}

// A UDP "DNS server" that takes queries and never answers, so a lookup stays in flight until it times out.
class SilentUdpServer {
public:
    SilentUdpServer()
        : m_fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0))
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof(address);
        auto* raw = reinterpret_cast<sockaddr*>(&address);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        REQUIRE(::bind(m_fd, raw, length) == 0);
        REQUIRE(::getsockname(m_fd, raw, &length) == 0);
        m_port = ntohs(address.sin_port);
    }
    ~SilentUdpServer() { ::close(m_fd); }
    SilentUdpServer(const SilentUdpServer&) = delete;
    SilentUdpServer& operator=(const SilentUdpServer&) = delete;
    SilentUdpServer(SilentUdpServer&&) = delete;
    SilentUdpServer& operator=(SilentUdpServer&&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }
    [[nodiscard]] bool received() const
    {
        char byte = 0;
        return ::recv(m_fd, &byte, 1, MSG_DONTWAIT | MSG_PEEK) >= 0;
    }

private:
    int m_fd;
    std::uint16_t m_port = 0;
};

struct Network {
    FakeSystemResolver system;
    DnsResolver resolver{system};
    ProxyRouter router{{}, nullptr};
    CurlTransport transport{resolver, router};

    Network()
    {
        resolver.setDohSender(transport.dohSender());
        DnsConfig dns;
        dns.overrides = {{QStringLiteral("app.test"), ip("127.0.0.1")},
                         {QStringLiteral("other.test"), ip("127.0.0.1")}};
        resolver.setConfig(dns);
        ProxyConfig proxy;
        proxy.mode = ProxyMode::None;
        router.setConfig(proxy);
        TransportConfig transportConfig;
        transportConfig.retryBackoff = 50ms;
        transport.setConfig(transportConfig);
    }

    void useProxy(ProxyType type, std::uint16_t port, QString user = {}, QString password = {})
    {
        ProxyConfig proxy;
        proxy.mode = ProxyMode::Manual;
        proxy.manual = {type, QStringLiteral("127.0.0.1"), port, std::move(user), std::move(password)};
        proxy.bypassLocal = false;  // the mock servers are local
        router.setConfig(proxy);
    }

    Result<Response> fetch(Request request, Callbacks callbacks = {})
    {
        QObject context;
        std::optional<Result<Response>> out;
        callbacks.onFinished = [&](Result<Response> result) {
            out = std::move(result);
        };
        transport.send(std::move(request), &context, std::move(callbacks));
        REQUIRE(test::waitUntil([&] { return out.has_value(); }, 15000));
        return *out;
    }

    Result<Response> get(const QString& url)
    {
        Request request;
        request.url = QUrl(url);
        return fetch(std::move(request));
    }
};

test::HttpReply text(QByteArray body, int status = 200)
{
    return {.status = status, .headers = {}, .body = std::move(body)};
}

}  // namespace

TEST_SUITE("net.resolver")
{
    TEST_CASE("system mode asks the OS once and then serves from the cache")
    {
        FakeSystemResolver system;
        system.table[QStringLiteral("api.test")] = std::vector{ip("192.0.2.1"), ip("2001:db8::1")};
        DnsResolver resolver(system);
        const auto first = resolveNow(resolver, QStringLiteral("API.test."));
        REQUIRE(first);
        CHECK(first->source == ResolverSource::System);
        CHECK(first->addresses.front() == ip("2001:db8::1"));  // IPv6 first under Auto
        const auto second = resolveNow(resolver, QStringLiteral("api.test"));
        REQUIRE(second);
        CHECK(second->source == ResolverSource::Cache);
        CHECK(system.calls == 1);
        resolver.flush();
        CHECK(resolveNow(resolver, QStringLiteral("api.test")));
        CHECK(system.calls == 2);
    }

    TEST_CASE("literals, localhost and host overrides never reach any resolver")
    {
        FakeSystemResolver system;
        DnsResolver resolver(system);
        DnsConfig config;
        config.overrides = {{QStringLiteral("fixed.test"), ip("192.0.2.9")}};
        config.family = IpFamilyPreference::PreferV4;
        resolver.setConfig(config);
        CHECK(resolveNow(resolver, QStringLiteral("[2001:db8::7]"))->source == ResolverSource::Literal);
        CHECK(resolveNow(resolver, QStringLiteral("localhost"))->addresses.front() == ip("127.0.0.1"));
        CHECK(resolveNow(resolver, QStringLiteral("fixed.test"))->addresses == std::vector{ip("192.0.2.9")});
        CHECK(test::errorOf(resolveNow(resolver, QStringLiteral("bad name!"))) == ErrorCode::InvalidArgument);
        CHECK(system.calls == 0);
    }

    TEST_CASE("a host override written with a trailing dot still matches")
    {
        FakeSystemResolver system;
        DnsResolver resolver(system);
        DnsConfig config;
        config.overrides = {{QStringLiteral("fixed.test."), ip("192.0.2.9")}};
        resolver.setConfig(config);
        const auto answer = resolveNow(resolver, QStringLiteral("fixed.test"));
        REQUIRE_MESSAGE(answer, test::describe(answer));
        CHECK(answer->source == ResolverSource::Override);
        CHECK(answer->addresses == std::vector{ip("192.0.2.9")});
        CHECK(system.calls == 0);
    }

    TEST_CASE("a host override with no address of the allowed IP version fails the lookup")
    {
        FakeSystemResolver system;
        DnsResolver resolver(system);
        DnsConfig config;
        config.overrides = {{QStringLiteral("fixed.test"), ip("2001:db8::9")}};
        config.family = IpFamilyPreference::V4Only;
        resolver.setConfig(config);
        CHECK(test::errorOf(resolveNow(resolver, QStringLiteral("fixed.test"))) == ErrorCode::DnsResolutionFailed);
        CHECK(system.calls == 0);
    }

    TEST_CASE("custom servers: answers, failover, TCP on truncation, family filter, single flight")
    {
        test::DnsServer server;
        server.add(QStringLiteral("api.test"), QStringLiteral("192.0.2.1"), 120);
        server.add(QStringLiteral("api.test"), QStringLiteral("2001:db8::1"), 120);
        FakeSystemResolver system;
        DnsResolver resolver(system);

        resolver.setConfig(customServers({deadPort(), server.port()}, false));
        const auto answer = resolveNow(resolver, QStringLiteral("api.test"));
        REQUIRE_MESSAGE(answer, test::describe(answer));
        CHECK(answer->source == ResolverSource::Servers);
        CHECK(answer->addresses.size() == 2);
        CHECK(answer->server == QStringLiteral("127.0.0.1:%1").arg(server.port()));

        server.setTruncateUdp(true);
        DnsConfig v4only = customServers({server.port()}, false);
        v4only.family = IpFamilyPreference::V4Only;
        resolver.setConfig(v4only);
        const int udpBefore = server.udpQueries();
        const auto viaTcp = resolveNow(resolver, QStringLiteral("api.test"));
        REQUIRE(viaTcp);
        CHECK(viaTcp->addresses == std::vector{ip("192.0.2.1")});
        CHECK(server.udpQueries() == udpBefore + 1);  // only A was asked
        CHECK(server.tcpQueries() >= 1);
        server.setTruncateUdp(false);

        resolver.flush();
        const int before = server.udpQueries();
        int answered = 0;
        for (int i = 0; i < 5; ++i) {
            resolver.resolve(QStringLiteral("api.test"), [&](const Result<Resolution>& r) { answered += r ? 1 : 0; });
        }
        REQUIRE(test::waitUntil([&] { return answered == 5; }));
        CHECK(server.udpQueries() == before + 1);  // five callers, one query
        CHECK(system.calls == 0);
    }

    TEST_CASE("a TCP answer whose id does not match the query moves on to the next server")
    {
        test::DnsServer wrong;
        wrong.add(QStringLiteral("api.test"), QStringLiteral("192.0.2.66"), 120);
        wrong.setTruncateUdp(true);
        wrong.setWrongTcpId(true);
        test::DnsServer good;
        good.add(QStringLiteral("api.test"), QStringLiteral("192.0.2.1"), 120);
        FakeSystemResolver system;
        DnsResolver resolver(system);
        DnsConfig config = customServers({wrong.port(), good.port()}, false);
        config.family = IpFamilyPreference::V4Only;
        resolver.setConfig(config);
        const auto answer = resolveNow(resolver, QStringLiteral("api.test"));
        REQUIRE_MESSAGE(answer, test::describe(answer));
        CHECK(wrong.tcpQueries() >= 1);
        CHECK(answer->addresses == std::vector{ip("192.0.2.1")});
        CHECK(answer->server == QStringLiteral("127.0.0.1:%1").arg(good.port()));
        CHECK(system.calls == 0);
    }

    TEST_CASE("a name that does not exist is cached negatively and never falls back")
    {
        test::DnsServer server;
        FakeSystemResolver system;
        DnsResolver resolver(system);
        resolver.setConfig(customServers({server.port()}, true));
        const auto missing = resolveNow(resolver, QStringLiteral("missing.test"));
        REQUIRE_FALSE(missing);
        CHECK(test::errorOf(missing) == ErrorCode::DnsResolutionFailed);
        const int queries = server.udpQueries();
        CHECK_FALSE(resolveNow(resolver, QStringLiteral("missing.test")));
        CHECK(server.udpQueries() == queries);
        CHECK(system.calls == 0);
    }

    TEST_CASE("unreachable servers fall back to the system resolver only when allowed")
    {
        FakeSystemResolver system;
        DnsResolver resolver(system);
        resolver.setConfig(customServers({deadPort()}, true));
        const auto fallback = resolveNow(resolver, QStringLiteral("api.test"));
        REQUIRE(fallback);
        CHECK(fallback->source == ResolverSource::SystemFallback);
        CHECK(system.calls == 1);

        resolver.setConfig(customServers({deadPort()}, false));
        const auto strict = resolveNow(resolver, QStringLiteral("api.test"));
        REQUIRE_FALSE(strict);
        CHECK(test::errorOf(strict) == ErrorCode::DnsServerUnreachable);
        CHECK(system.calls == 1);
    }

    TEST_CASE("a DNS settings change during a lookup asks again under the new settings")
    {
        const SilentUdpServer silent;
        FakeSystemResolver system;
        system.table[QStringLiteral("api.test")] = std::vector{ip("192.0.2.1")};
        system.table[QStringLiteral("other.test")] = std::vector{ip("192.0.2.2")};
        DnsResolver resolver(system);

        // The change arrives while the query waits for the silent server.
        resolver.setConfig(customServers({silent.port()}, false));
        std::optional<Result<Resolution>> first;
        resolver.resolve(QStringLiteral("api.test"), [&](Result<Resolution> result) { first = std::move(result); });
        REQUIRE(test::waitUntil([&] { return silent.received(); }));
        resolver.setConfig(DnsConfig{});
        REQUIRE(test::waitUntil([&] { return first.has_value(); }, 1500));
        REQUIRE_MESSAGE(*first, test::describe(*first));
        CHECK((*first)->source == ResolverSource::System);
        CHECK((*first)->addresses == std::vector{ip("192.0.2.1")});

        // The change arrives before the lookup has even picked its upstream.
        resolver.setConfig(customServers({silent.port()}, false));
        std::optional<Result<Resolution>> second;
        resolver.resolve(QStringLiteral("other.test"), [&](Result<Resolution> result) { second = std::move(result); });
        resolver.setConfig(DnsConfig{});
        REQUIRE(test::waitUntil([&] { return second.has_value(); }, 1500));
        REQUIRE_MESSAGE(*second, test::describe(*second));
        CHECK((*second)->source == ResolverSource::System);
        CHECK((*second)->addresses == std::vector{ip("192.0.2.2")});
        CHECK(system.calls == 2);
    }

    TEST_CASE("flushing the DNS cache during a lookup keeps the fallback and the DoH server")
    {
        // Custom servers that never answer, fallback allowed: the flush arrives while the query waits.
        const SilentUdpServer silent;
        FakeSystemResolver system;
        system.table[QStringLiteral("api.test")] = std::vector{ip("192.0.2.1")};
        DnsResolver resolver(system);
        resolver.setConfig(customServers({silent.port()}, true));
        std::optional<Result<Resolution>> fallback;
        resolver.resolve(QStringLiteral("api.test"), [&](Result<Resolution> result) { fallback = std::move(result); });
        REQUIRE(test::waitUntil([&] { return silent.received(); }));
        resolver.flush();
        REQUIRE(test::waitUntil([&] { return fallback.has_value(); }, 10000));
        REQUIRE_MESSAGE(*fallback, test::describe(*fallback));
        CHECK((*fallback)->source == ResolverSource::SystemFallback);

        // DoH by name without bootstrap addresses: the flush arrives while the server's name is looked up.
        test::DnsServer table;
        table.add(QStringLiteral("doh.test"), QStringLiteral("192.0.2.44"));
        const test::DohServer doh(table);
        Network net;
        net.system.table[QStringLiteral("localhost")] = std::vector{ip("127.0.0.1")};
        DnsConfig config;
        config.mode = DnsMode::Doh;
        config.doh.url = QUrl(QStringLiteral("http://localhost:%1/dns-query").arg(doh.port()));
        config.doh.allowInsecure = true;
        config.fallbackToSystem = true;
        net.resolver.setConfig(config);
        std::optional<Result<Resolution>> viaDoh;
        net.resolver.resolve(QStringLiteral("doh.test"),
                             [&](Result<Resolution> result) { viaDoh = std::move(result); });
        net.resolver.flush();
        REQUIRE(test::waitUntil([&] { return viaDoh.has_value(); }, 10000));
        REQUIRE_MESSAGE(*viaDoh, test::describe(*viaDoh));
        CHECK((*viaDoh)->source == ResolverSource::Doh);
        CHECK((*viaDoh)->addresses == std::vector{ip("192.0.2.44")});
        CHECK(net.system.hosts == QStringList{QStringLiteral("localhost")});
    }

    TEST_CASE("DoH: answers over HTTP POST, insecure URLs refused, strict mode leaks nothing")
    {
        test::DnsServer table;
        table.add(QStringLiteral("api.test"), QStringLiteral("192.0.2.44"));
        const test::DohServer doh(table);
        Network net;
        DnsConfig config;
        config.mode = DnsMode::Doh;
        config.doh.url = QUrl(doh.url());
        config.doh.allowInsecure = true;
        config.fallbackToSystem = false;
        net.resolver.setConfig(config);

        const auto answer = resolveNow(net.resolver, QStringLiteral("api.test"));
        REQUIRE_MESSAGE(answer, test::describe(answer));
        CHECK(answer->source == ResolverSource::Doh);
        CHECK(answer->addresses == std::vector{ip("192.0.2.44")});
        CHECK(doh.queries() == 2);  // A and AAAA

        config.doh.allowInsecure = false;
        net.resolver.setConfig(config);
        CHECK(test::errorOf(resolveNow(net.resolver, QStringLiteral("api.test"))) == ErrorCode::DohInsecureUrl);

        // Strict mode with a DoH server that is down: a clear DoH error, and still no system lookup.
        config.doh.allowInsecure = true;
        config.doh.url = QUrl(QStringLiteral("http://127.0.0.1:%1/dns-query").arg(deadPort()));
        net.resolver.setConfig(config);
        CHECK(test::errorOf(resolveNow(net.resolver, QStringLiteral("api.test"))) == ErrorCode::DohUnreachable);

        // Strict mode with a DoH server given by name and no bootstrap addresses cannot start.
        config.doh.url = QUrl(QStringLiteral("https://dns.example/dns-query"));
        net.resolver.setConfig(config);
        CHECK(test::errorOf(resolveNow(net.resolver, QStringLiteral("api.test"))) == ErrorCode::DohUnreachable);
        CHECK(net.system.calls == 0);
        CHECK(net.transport.unexpectedCurlResolves() == 0);

        config.fallbackToSystem = true;
        net.resolver.setConfig(config);
        net.system.table[QStringLiteral("dns.example")] = fail(ErrorCode::DnsResolutionFailed, "down");
        const auto fallback = resolveNow(net.resolver, QStringLiteral("api.test"));
        REQUIRE(fallback);
        CHECK(fallback->source == ResolverSource::SystemFallback);
    }

    TEST_CASE("DoH at an IPv6 literal address answers in strict mode")
    {
        test::DnsServer table;
        table.add(QStringLiteral("api.test"), QStringLiteral("192.0.2.45"));
        const test::DohServer doh(table, true);
        Network net;
        DnsConfig config;
        config.mode = DnsMode::Doh;
        config.doh.url = QUrl(doh.url());
        config.doh.allowInsecure = true;
        config.fallbackToSystem = false;
        net.resolver.setConfig(config);

        const auto answer = resolveNow(net.resolver, QStringLiteral("api.test"));
        REQUIRE_MESSAGE(answer, test::describe(answer));
        CHECK(answer->source == ResolverSource::Doh);
        CHECK(answer->addresses == std::vector{ip("192.0.2.45")});
        CHECK(net.system.calls == 0);
        CHECK(net.transport.unexpectedCurlResolves() == 0);
    }
}

TEST_SUITE("net.transport")
{
    TEST_CASE("strict DoH end to end: the request succeeds and nothing asks the system resolver")
    {
        test::HttpServer origin([](const test::HttpRequest& r) { return text("hello " + r.headers.at("host")); });
        test::DnsServer table;
        table.add(QStringLiteral("strict.test"), QStringLiteral("127.0.0.1"));
        const test::DohServer doh(table);
        Network net;
        DnsConfig config;
        config.mode = DnsMode::Doh;
        config.doh.url = QUrl(doh.url());
        config.doh.allowInsecure = true;
        config.fallbackToSystem = false;
        net.resolver.setConfig(config);

        const auto response = net.get(QStringLiteral("http://strict.test:%1/").arg(origin.port()));
        REQUIRE_MESSAGE(response, test::describe(response));
        CHECK(response->status == 200);
        CHECK(response->body == "hello strict.test:" + QByteArray::number(origin.port()));
        CHECK(response->remoteAddress == QStringLiteral("127.0.0.1"));
        CHECK(net.system.calls == 0);
        CHECK(net.transport.unexpectedCurlResolves() == 0);
    }

    TEST_CASE("redirects are followed through our resolver; credentials stay with their host")
    {
        test::HttpServer target(
            [](const test::HttpRequest& r) { return text(r.headers.contains("authorization") ? "leaked" : "clean"); });
        test::HttpServer origin([&](const test::HttpRequest& r) {
            if (r.target == "/same") {
                return test::HttpReply{.status = 302, .headers = {{"Location", "/final"}}, .body = {}};
            }
            if (r.target == "/final") {
                return text(r.headers.contains("authorization") ? "kept" : "lost");
            }
            const QByteArray location = "http://other.test:" + QByteArray::number(target.port()) + "/elsewhere";
            return test::HttpReply{.status = 302, .headers = {{"Location", location}}, .body = {}};
        });
        Network net;
        Request request;
        request.url = QUrl(QStringLiteral("http://app.test:%1/same").arg(origin.port()));
        request.headers = {{"authorization", "OAuth secret"}};
        const auto same = net.fetch(request);
        REQUIRE(same);
        CHECK(same->body == "kept");
        CHECK(same->url.path() == QStringLiteral("/final"));

        request.url = QUrl(QStringLiteral("http://app.test:%1/away").arg(origin.port()));
        const auto away = net.fetch(request);
        REQUIRE(away);
        CHECK(away->body == "clean");
        CHECK(away->url.host() == QStringLiteral("other.test"));

        // Same host, another port: a different origin, so the credentials stay behind too.
        test::HttpServer port([&](const test::HttpRequest&) {
            const QByteArray location = "http://app.test:" + QByteArray::number(target.port()) + "/x";
            return test::HttpReply{.status = 302, .headers = {{"Location", location}}, .body = {}};
        });
        request.url = QUrl(QStringLiteral("http://app.test:%1/port").arg(port.port()));
        const auto otherPort = net.fetch(request);
        REQUIRE_MESSAGE(otherPort, test::describe(otherPort));
        CHECK(otherPort->body == "clean");

        // https -> http on the same host: never send the token in cleartext.
        const test::TestCertificate certificate;
        TransportConfig config;
        config.caBundle = certificate.pemPath();
        config.retryBackoff = 50ms;
        net.transport.setConfig(config);
        test::HttpServer secure(
            [&](const test::HttpRequest&) {
                const QByteArray location = "http://app.test:" + QByteArray::number(target.port()) + "/x";
                return test::HttpReply{.status = 302, .headers = {{"Location", location}}, .body = {}};
            },
            &certificate);
        request.url = QUrl(QStringLiteral("https://app.test:%1/down").arg(secure.port()));
        const auto downgrade = net.fetch(request);
        REQUIRE_MESSAGE(downgrade, test::describe(downgrade));
        CHECK(downgrade->body == "clean");
        CHECK(net.transport.unexpectedCurlResolves() == 0);
    }

    TEST_CASE("idempotent requests are retried on 503, POST is not")
    {
        std::atomic<int> hits{0};
        test::HttpServer origin([&](const test::HttpRequest&) {
            return ++hits < 3 ? test::HttpReply{.status = 503, .headers = {{"Retry-After", "0"}}, .body = {}}
                              : text("finally");
        });
        Network net;
        const auto response = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE(response);
        CHECK(response->body == "finally");
        CHECK(response->attempts == 3);

        hits = 0;
        Request post;
        post.method = Method::Post;
        post.url = QUrl(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        post.body = "x";
        const auto once = net.fetch(post);
        REQUIRE(once);
        CHECK(once->status == 503);
        CHECK(hits == 1);
    }

    TEST_CASE("streaming delivers headers first, then the body in order")
    {
        const QByteArray payload(300'000, 'z');
        test::HttpServer origin([&](const test::HttpRequest& r) {
            return test::HttpReply{.status = r.headers.contains("range") ? 206 : 200, .headers = {}, .body = payload};
        });
        Network net;
        QByteArray streamed;
        int heads = 0;
        Callbacks callbacks;
        callbacks.onResponse = [&](const Response& head) {
            ++heads;
            CHECK(head.status == 206);
            CHECK(streamed.isEmpty());
        };
        callbacks.onData = [&](const QByteArray& chunk) {
            streamed += chunk;
        };
        Request request;
        request.url = QUrl(QStringLiteral("http://app.test:%1/a.mp3").arg(origin.port()));
        request.range = ByteRange{0, -1};
        const auto response = net.fetch(request, callbacks);
        REQUIRE(response);
        CHECK(heads == 1);
        CHECK(response->body.isEmpty());
        CHECK(streamed == payload);
        CHECK(origin.requests().back().headers.at("range") == "bytes=0-");
    }

    TEST_CASE("cancellation, offline mode and metered background requests")
    {
        test::HttpServer slow([](const test::HttpRequest&) {
            std::this_thread::sleep_for(500ms);
            return text("late");
        });
        Network net;
        {
            QObject context;
            std::optional<Result<Response>> out;
            Request request;
            request.url = QUrl(QStringLiteral("http://app.test:%1/").arg(slow.port()));
            const RequestHandle handle = net.transport.send(
                request, &context, {.onResponse = {}, .onData = {}, .onFinished = [&](auto r) { out = r; }});
            QTimer::singleShot(50, &context, [handle] { handle.cancel(); });
            REQUIRE(test::waitUntil([&] { return out.has_value(); }));
            CHECK(test::errorOf(*out) == ErrorCode::Cancelled);
        }
        {
            bool called = false;
            auto context = std::make_unique<QObject>();
            Request request;
            request.url = QUrl(QStringLiteral("http://app.test:%1/").arg(slow.port()));
            net.transport.send(request, context.get(),
                               {.onResponse = {}, .onData = {}, .onFinished = [&](auto) { called = true; }});
            context.reset();  // the receiver is gone: no callback, and the transfer is abandoned
            CHECK(test::waitUntil([&] { return net.transport.activeTransfers() == 0; }, 5000));
            CHECK_FALSE(called);
        }

        TransportConfig config;
        config.offline = true;
        net.transport.setConfig(config);
        CHECK(test::errorOf(net.get(QStringLiteral("http://app.test:%1/").arg(slow.port()))) == ErrorCode::Offline);
        config.offline = false;
        net.transport.setConfig(config);
        net.transport.setMetered(true);
        Request prefetch;
        prefetch.url = QUrl(QStringLiteral("http://app.test:%1/").arg(slow.port()));
        prefetch.background = true;
        CHECK(test::errorOf(net.fetch(prefetch)) == ErrorCode::MeteredDeferred);
    }

    TEST_CASE("HTTP proxy: absolute-form for http, CONNECT for https, destination names stay with the proxy")
    {
        const test::TestCertificate certificate;
        test::HttpServer secure([](const test::HttpRequest&) { return text("secure"); }, &certificate);
        test::HttpProxy proxy;
        Network net;
        TransportConfig config;
        config.caBundle = certificate.pemPath();
        net.transport.setConfig(config);
        net.useProxy(ProxyType::Http, proxy.port());

        const auto plain = net.get(QStringLiteral("http://remote-only.test/path?q=1"));
        REQUIRE_MESSAGE(plain, test::describe(plain));
        CHECK(plain->body == "via-proxy");
        CHECK(proxy.requestLines().front().startsWith("GET http://remote-only.test/path?q=1"));

        const auto tunnelled = net.get(QStringLiteral("https://app.test:%1/").arg(secure.port()));
        REQUIRE_MESSAGE(tunnelled, test::describe(tunnelled));
        CHECK(tunnelled->body == "secure");
        CHECK(proxy.requestLines().back() == "CONNECT app.test:" + QByteArray::number(secure.port()) + " HTTP/1.1");
        CHECK(net.system.calls == 0);
        CHECK(net.resolver.stats().lookups == 0);  // proxy given as an IP; destinations resolved remotely
        CHECK(net.transport.unexpectedCurlResolves() == 0);
    }

    TEST_CASE("proxy authentication: rejected without credentials, accepted with them")
    {
        test::HttpProxy proxy("alice:s3cret");
        Network net;
        net.useProxy(ProxyType::Http, proxy.port());
        CHECK(test::errorOf(net.get(QStringLiteral("http://example.test/"))) == ErrorCode::ProxyAuthRequired);
        net.useProxy(ProxyType::Http, proxy.port(), QStringLiteral("alice"), QStringLiteral("s3cret"));
        const auto ok = net.get(QStringLiteral("http://example.test/"));
        REQUIRE_MESSAGE(ok, test::describe(ok));
        CHECK(ok->route == QStringLiteral("http://<redacted>@127.0.0.1:%1").arg(proxy.port()));

        test::SocksProxy socks("bob:pw");
        net.useProxy(ProxyType::Socks5h, socks.port(), QStringLiteral("bob"), QStringLiteral("wrong"));
        CHECK(test::errorOf(net.get(QStringLiteral("http://example.test/"))) == ErrorCode::ProxyAuthRequired);
    }

    TEST_CASE("SOCKS5h sends the name to the proxy; SOCKS5 sends an address from our resolver")
    {
        test::HttpServer origin([](const test::HttpRequest&) { return text("socks"); });
        test::SocksProxy socks;
        Network net;

        net.useProxy(ProxyType::Socks5h, socks.port());
        REQUIRE(net.get(QStringLiteral("http://app.test:%1/").arg(origin.port())));
        CHECK(socks.connects().back().addressType == 3);
        CHECK(socks.connects().back().host == "app.test");
        CHECK(net.resolver.stats().lookups == 0);

        net.transport.drainConnections();
        net.useProxy(ProxyType::Socks5, socks.port());
        const auto local = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE_MESSAGE(local, test::describe(local));
        CHECK(local->body == "socks");
        CHECK(socks.connects().back().addressType == 1);
        CHECK(socks.connects().back().host == "127.0.0.1");  // from the host override, via our resolver
        CHECK(net.resolver.stats().lookups == 1);

        net.transport.drainConnections();
        net.useProxy(ProxyType::Socks4, socks.port());
        const auto socks4 = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE_MESSAGE(socks4, test::describe(socks4));
        CHECK(socks.connects().back().version == 4);
        CHECK(net.system.calls == 0);
        CHECK(net.transport.unexpectedCurlResolves() == 0);
    }

    TEST_CASE("the environment's no_proxy never overrides the router's proxy choice")
    {
        const QByteArray saved = qgetenv("no_proxy");
        const bool hadSaved = qEnvironmentVariableIsSet("no_proxy");
        qputenv("no_proxy", "*");
        const auto restore = qScopeGuard([&] {
            if (hadSaved) {
                qputenv("no_proxy", saved);
            } else {
                qunsetenv("no_proxy");
            }
        });
        test::HttpServer origin([](const test::HttpRequest&) { return text("direct"); });
        test::HttpProxy http;
        test::SocksProxy socks;
        Network net;

        net.useProxy(ProxyType::Http, http.port());
        const auto viaHttp = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE_MESSAGE(viaHttp, test::describe(viaHttp));
        CHECK(viaHttp->body == "via-proxy");

        net.transport.drainConnections();
        net.useProxy(ProxyType::Socks5h, socks.port());
        const auto viaSocks5h = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE_MESSAGE(viaSocks5h, test::describe(viaSocks5h));
        CHECK(socks.connects().size() == 1);

        net.transport.drainConnections();
        net.useProxy(ProxyType::Socks5, socks.port());
        const auto viaSocks5 = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE_MESSAGE(viaSocks5, test::describe(viaSocks5));
        CHECK(socks.connects().size() == 2);
        CHECK(net.transport.unexpectedCurlResolves() == 0);
    }

    TEST_CASE("certificates are always verified; a custom CA bundle makes a private CA trusted")
    {
        const test::TestCertificate certificate;
        test::HttpServer secure([](const test::HttpRequest&) { return text("ok"); }, &certificate);
        Network net;
        const QString url = QStringLiteral("https://app.test:%1/").arg(secure.port());
        CHECK(test::errorOf(net.get(url)) == ErrorCode::TlsCertificateInvalid);
        TransportConfig config;
        config.caBundle = certificate.pemPath();
        net.transport.setConfig(config);
        const auto trusted = net.get(url);
        REQUIRE_MESSAGE(trusted, test::describe(trusted));
        CHECK(trusted->body == "ok");
        CHECK(trusted->timings.tlsMs > 0);
        // The certificate does not cover this name.
        DnsConfig wrongName;
        wrongName.overrides = {{QStringLiteral("wrong.test"), ip("127.0.0.1")}};
        net.resolver.setConfig(wrongName);
        CHECK(test::errorOf(net.get(QStringLiteral("https://wrong.test:%1/").arg(secure.port()))) ==
              ErrorCode::TlsCertificateInvalid);
    }

    TEST_CASE("the bandwidth limit applies across the transfer")
    {
        const QByteArray payload(150'000, 'b');
        test::HttpServer origin([&](const test::HttpRequest&) { return text(payload); });
        Network net;
        TransportConfig config;
        config.bandwidthBytesPerSecond = 100'000;
        net.transport.setConfig(config);
        QElapsedTimer clock;
        clock.start();
        const auto response = net.get(QStringLiteral("http://app.test:%1/").arg(origin.port()));
        REQUIRE(response);
        CHECK(response->body.size() == payload.size());
        CHECK(clock.elapsed() >= 900);  // 25 kB burst, then 100 kB/s
    }

    TEST_CASE("connection failures are typed")
    {
        Network net;
        CHECK(test::errorOf(net.get(QStringLiteral("http://app.test:%1/").arg(deadPort()))) ==
              ErrorCode::ConnectionRefused);
        net.useProxy(ProxyType::Http, deadPort());
        const auto deadProxy = net.get(QStringLiteral("http://example.test/"));
        CHECK_MESSAGE(test::errorOf(deadProxy) == ErrorCode::ProxyConnectFailed, test::describe(deadProxy));
        ProxyConfig unset;
        unset.mode = ProxyMode::Manual;
        net.router.setConfig(unset);
        CHECK(test::errorOf(net.get(QStringLiteral("http://example.test/"))) == ErrorCode::ProxyNotConfigured);
        CHECK(test::errorOf(net.get(QStringLiteral("ftp://example.test/"))) == ErrorCode::InvalidArgument);
    }

    TEST_CASE("the transport can be destroyed with HTTPS transfers in flight and connections pooled")
    {
        // Teardown with a pooled TLS connection and a transfer in flight. curl_multi_cleanup reports the sockets
        // it still watches, whose notifiers must be alive then; the transport used to delete them first, a
        // use-after-free when Noctara quit mid-request over HTTP/2 (the connection stays pooled while a stream
        // is removed). The HTTP/1.1 mock closes such connections at once, so this covers the path, not that race.
        const test::TestCertificate certificate;
        std::atomic<bool> release{false};
        test::HttpServer secure(
            [&](const test::HttpRequest& request) {
                while (request.target == "/slow" && !release) {
                    std::this_thread::sleep_for(5ms);
                }
                return text("ok");
            },
            &certificate);
        for (int round = 0; round < 3; ++round) {
            auto net = std::make_unique<Network>();
            TransportConfig config;
            config.caBundle = certificate.pemPath();
            net->transport.setConfig(config);
            const auto pooled = net->get(QStringLiteral("https://app.test:%1/fast").arg(secure.port()));
            REQUIRE_MESSAGE(pooled, test::describe(pooled));
            QObject context;
            Request slow;
            slow.url = QUrl(QStringLiteral("https://app.test:%1/slow").arg(secure.port()));
            bool answered = false;
            net->transport.send(slow, &context,
                                {.onResponse = {}, .onData = {}, .onFinished = [&](auto) { answered = true; }});
            const auto seen = [&] {
                const auto requests = secure.requests();
                return std::ranges::count(requests, QByteArray("/slow"), &test::HttpRequest::target);
            };
            REQUIRE(test::waitUntil([&] { return seen() == round + 1; }, 10000));
            net.reset();
            test::waitUntil([] { return false; }, 50);  // a stale event would be delivered (and crash) now
            CHECK_FALSE(answered);
        }
        release = true;
    }
}

TEST_SUITE("net.service")
{
    TEST_CASE("the service runs the stack on its own thread and applies settings live")
    {
        namespace keys = settings::keys;
        test::HttpServer origin([](const test::HttpRequest& r) { return text("served " + r.target); });
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(keys::ProxyMode, QStringLiteral("none")));
        REQUIRE(store.setUserValue(keys::HostOverrides, QStringList{QStringLiteral("app.test=127.0.0.1")}));
        NetworkService service(store, {}, false);

        const auto fetch = [&](const QString& url) {
            QObject context;
            std::optional<Result<Response>> out;
            Request request;
            request.url = QUrl(url);
            service.transport().send(request, &context,
                                     {.onResponse = {}, .onData = {}, .onFinished = [&](auto r) { out = r; }});
            REQUIRE(test::waitUntil([&] { return out.has_value(); }, 10000));
            return *out;
        };
        const QString url = QStringLiteral("http://app.test:%1/x?token=secret").arg(origin.port());
        const auto first = fetch(url);
        REQUIRE_MESSAGE(first, test::describe(first));
        CHECK(first->body == "served /x?token=secret");
        REQUIRE(test::waitUntil([&] { return !service.recentRequests().empty(); }));
        CHECK_FALSE(service.recentRequests().front().url.contains(u"secret"));

        REQUIRE(store.setUserValue(keys::OfflineMode, true));
        REQUIRE(test::waitUntil([&] { return service.config().transport.offline; }));
        CHECK(test::errorOf(fetch(url)) == ErrorCode::Offline);
        REQUIRE(store.setUserValue(keys::OfflineMode, false));
        REQUIRE(test::waitUntil([&] { return !service.config().transport.offline; }));

        // Strict custom DNS with a server that does not answer: Test DNS reports every host, nothing leaks.
        REQUIRE(store.setUserValue(keys::DnsMode, QStringLiteral("custom")));
        REQUIRE(store.setUserValue(keys::DnsServerPreset, QStringLiteral("custom")));
        REQUIRE(store.setUserValue(keys::DnsServers, QStringList{QStringLiteral("127.0.0.1:%1").arg(deadPort())}));
        REQUIRE(store.setUserValue(keys::DnsFallbackToSystem, false));
        REQUIRE(test::waitUntil([&] { return service.config().dns.strict(); }));
        QObject context;
        std::optional<std::vector<DnsTestResult>> results;
        service.testDns(&context, [&](std::vector<DnsTestResult> r) { results = std::move(r); });
        REQUIRE(test::waitUntil([&] { return results.has_value(); }, 10000));
        REQUIRE(results->size() == serviceHosts().size());
        for (const DnsTestResult& result : *results) {
            REQUIRE_FALSE(result.result);
            CHECK(test::errorOf(result.result) == ErrorCode::DnsServerUnreachable);
        }
        std::optional<ResolverSnapshot> snapshot;
        service.snapshot(&context, [&](ResolverSnapshot s) { snapshot = s; });
        REQUIRE(test::waitUntil([&] { return snapshot.has_value(); }));
        CHECK(snapshot->systemLookups == 0);
        // The overridden host still works under strict DNS.
        CHECK(fetch(url));
    }
}

// Numbers for docs/network.md (run with --no-skip -tc="transport spike*"): loopback, so they show the
// transport's own overhead, not the network's.
TEST_CASE("transport spike measurements" * doctest::skip())
{
    test::HttpServer origin([](const test::HttpRequest&) { return text(QByteArray(1024, 'x')); });
    Network net;
    const QString url = QStringLiteral("http://app.test:%1/").arg(origin.port());
    REQUIRE(net.get(url));  // warm the connection

    QElapsedTimer clock;
    constexpr int kSequential = 500;
    clock.start();
    for (int i = 0; i < kSequential; ++i) {
        REQUIRE(net.get(url));
    }
    const double sequentialUs = static_cast<double>(clock.nsecsElapsed()) / 1e3 / kSequential;

    constexpr int kParallel = 200;
    QObject context;
    int done = 0;
    clock.restart();
    for (int i = 0; i < kParallel; ++i) {
        Request request;
        request.url = QUrl(url);
        net.transport.send(request, &context, {.onResponse = {}, .onData = {}, .onFinished = [&](auto) { ++done; }});
    }
    REQUIRE(test::waitUntil([&] { return done == kParallel; }, 30000));
    const double parallelMs = static_cast<double>(clock.nsecsElapsed()) / 1e6;

    constexpr int kCached = 10000;
    int resolved = 0;
    DnsConfig cached;
    net.resolver.setConfig(cached);
    net.system.table[QStringLiteral("cached.test")] = std::vector{ip("192.0.2.1")};
    REQUIRE(resolveNow(net.resolver, QStringLiteral("cached.test")));
    clock.restart();
    for (int i = 0; i < kCached; ++i) {
        net.resolver.resolve(QStringLiteral("cached.test"), [&](const Result<Resolution>&) { ++resolved; });
    }
    REQUIRE(test::waitUntil([&] { return resolved == kCached; }));
    const double cachedUs = static_cast<double>(clock.nsecsElapsed()) / 1e3 / kCached;

    test::DnsServer table;
    for (int i = 0; i < 50; ++i) {
        table.add(QStringLiteral("h%1.test").arg(i), QStringLiteral("192.0.2.%1").arg(i + 1));
    }
    const test::DohServer doh(table);
    DnsConfig dohConfig;
    dohConfig.mode = DnsMode::Doh;
    dohConfig.doh.url = QUrl(doh.url());
    dohConfig.doh.allowInsecure = true;
    net.resolver.setConfig(dohConfig);
    clock.restart();
    for (int i = 0; i < 50; ++i) {
        REQUIRE(resolveNow(net.resolver, QStringLiteral("h%1.test").arg(i)));
    }
    const double dohMs = static_cast<double>(clock.nsecsElapsed()) / 1e6 / 50;

    MESSAGE("sequential keep-alive GET (1 KiB): ", sequentialUs, " us/request");
    MESSAGE(kParallel, " concurrent GETs: ", parallelMs, " ms total");
    MESSAGE("cached DNS lookup: ", cachedUs, " us");
    MESSAGE("uncached DoH lookup (A+AAAA, loopback): ", dohMs, " ms");
    CHECK(net.transport.unexpectedCurlResolves() == 0);
}

// Talks to the real internet (run with --no-skip -tc="real network*"): strict DoH through Cloudflare's bootstrap
// addresses, then an HTTPS request to the SoundCloud API with system certificate verification.
TEST_CASE("real network: strict DoH and an HTTPS request" * doctest::skip())
{
    Network net;
    DnsConfig config;
    config.mode = DnsMode::Doh;
    config.doh.url = QUrl(QStringLiteral("https://cloudflare-dns.com/dns-query"));
    config.doh.bootstrap = {ip("1.1.1.1"), ip("1.0.0.1")};
    config.fallbackToSystem = false;
    net.resolver.setConfig(config);
    const auto resolved = resolveNow(net.resolver, QStringLiteral("api-v2.soundcloud.com"));
    REQUIRE_MESSAGE(resolved, test::describe(resolved));
    MESSAGE("api-v2.soundcloud.com via ", resolved->server.toStdString(), " in ", resolved->elapsed.count(), " ms");

    Request request;
    request.method = Method::Head;
    request.url = QUrl(QStringLiteral("https://api-v2.soundcloud.com/"));
    const auto response = net.fetch(request);
    REQUIRE_MESSAGE(response, test::describe(response));
    MESSAGE("HTTP ", response->status, " over ", response->httpVersion.toStdString(), " from ",
            response->remoteAddress.toStdString(), ", TLS done at ", response->timings.tlsMs, " ms");
    CHECK(net.system.calls == 0);
    CHECK(net.transport.unexpectedCurlResolves() == 0);
}
