// Pure parts of the network layer: addresses, DNS wire format, cache, proxy rules, retry policy, rate limit,
// redaction, error mapping and the settings snapshot.
#include "TestUtil.hpp"

#include "core/NetAddress.hpp"
#include "net/CurlSupport.hpp"
#include "net/DnsCache.hpp"
#include "net/DnsMessage.hpp"
#include "net/NetworkConfig.hpp"
#include "net/ProxyRules.hpp"
#include "net/Redaction.hpp"
#include "net/RetryPolicy.hpp"
#include "net/TokenBucket.hpp"
#include "settings/Keys.hpp"
#include "settings/SettingsStore.hpp"

#include <QTimeZone>

#include <cerrno>

using namespace noctara;
using namespace noctara::net;
using namespace std::chrono_literals;

namespace {

IpAddress ip(const char* text)
{
    const auto address = IpAddress::parse(QString::fromLatin1(text));
    REQUIRE(address.has_value());
    return *address;
}

QByteArray hex(const char* text)
{
    return QByteArray::fromHex(text);
}

}  // namespace

TEST_SUITE("net.address")
{
    TEST_CASE("IP addresses parse strictly and format canonically")
    {
        CHECK(ip("192.0.2.1").toString() == QStringLiteral("192.0.2.1"));
        CHECK(ip("2001:DB8:0:0::1").toString() == QStringLiteral("2001:db8::1"));
        CHECK(ip("[2001:db8::1]").toUrlHost() == QStringLiteral("[2001:db8::1]"));
        CHECK_FALSE(IpAddress::parse(u"010.0.0.1"));  // no octal forms
        CHECK_FALSE(IpAddress::parse(u"1.2.3"));
        CHECK_FALSE(IpAddress::parse(u"example.com"));
        CHECK_FALSE(IpAddress::parse(u""));
    }

    TEST_CASE("local address classes")
    {
        CHECK(ip("127.0.0.53").isLoopback());
        CHECK(ip("::1").isLoopback());
        CHECK(ip("::ffff:127.0.0.1").isLoopback());
        CHECK(ip("10.1.2.3").isPrivate());
        CHECK(ip("172.31.0.1").isPrivate());
        CHECK_FALSE(ip("172.32.0.1").isPrivate());
        CHECK(ip("192.168.0.1").isPrivate());
        CHECK(ip("fd12::1").isPrivate());
        CHECK(ip("169.254.1.1").isLinkLocal());
        CHECK(ip("fe80::1").isLinkLocal());
        CHECK_FALSE(ip("1.1.1.1").isLocal());
        CHECK_FALSE(ip("2606:4700::1111").isLocal());
    }

    TEST_CASE("CIDR ranges and endpoints")
    {
        const auto net = Cidr::parse(u"10.0.0.0/8");
        REQUIRE(net);
        CHECK(net->contains(ip("10.255.0.1")));
        CHECK_FALSE(net->contains(ip("11.0.0.1")));
        CHECK_FALSE(net->contains(ip("::ffff:10.0.0.1")));  // families never mix
        const auto v6 = Cidr::parse(u"2001:db8::/33");
        REQUIRE(v6);
        CHECK(v6->contains(ip("2001:db8:7fff::1")));
        CHECK_FALSE(v6->contains(ip("2001:db8:8000::1")));
        CHECK_FALSE(Cidr::parse(u"10.0.0.0/33"));
        CHECK(Cidr::parse(u"192.0.2.7")->prefix == 32);

        CHECK(Endpoint::parse(u"1.1.1.1", 53)->port == 53);
        CHECK(Endpoint::parse(u"1.1.1.1:5353", 53)->port == 5353);
        CHECK(Endpoint::parse(u"[2606:4700::1111]:853", 53)->port == 853);
        CHECK(Endpoint::parse(u"2606:4700::1111", 53)->address == ip("2606:4700::1111"));
        CHECK_FALSE(Endpoint::parse(u"1.1.1.1:0", 53));
        CHECK_FALSE(Endpoint::parse(u"dns.google", 53));
        CHECK(Endpoint::parse(u"[::1]:5353", 53)->toString(53) == QStringLiteral("[::1]:5353"));
    }

    TEST_CASE("host name validation")
    {
        CHECK(isValidHostname(u"api-v2.soundcloud.com"));
        CHECK(isValidHostname(u"xn--80ak6aa92e.com"));
        CHECK(isValidHostname(u"_dns.example."));
        CHECK_FALSE(isValidHostname(u"-bad.example"));
        CHECK_FALSE(isValidHostname(u"bad-.example"));
        CHECK_FALSE(isValidHostname(u"two..dots"));
        CHECK_FALSE(isValidHostname(u"spaces are bad"));
        CHECK_FALSE(isValidHostname(QString(QString(64, u'a') + u".com")));
    }
}

TEST_SUITE("net.dns.message")
{
    TEST_CASE("query encoding matches the RFC 1035 layout byte for byte")
    {
        const auto query = dns::encodeQuery("example.com", dns::RecordType::A, 0x1234, false);
        REQUIRE(query);
        CHECK(*query == hex("1234 0100 0001 0000 0000 0001"         // header: RD, one question, one OPT
                            "076578616d706c6503636f6d00 0001 0001"  // example.com A IN
                            "00 0029 04d0 0000 0000 0000"));        // OPT: payload 1232, no options
    }

    TEST_CASE("padding fills queries to a multiple of 128 bytes and sends no client subnet")
    {
        for (const char* name : {"a.io", "api-v2.soundcloud.com", "a-much-longer-name.with.many.labels.example.org"}) {
            const auto query = dns::encodeQuery(name, dns::RecordType::Aaaa, 0, true);
            REQUIRE(query);
            CHECK(query->size() % 128 == 0);
            CHECK(query->contains(hex("000c")));        // EDNS padding option
            CHECK_FALSE(query->contains(hex("0008")));  // EDNS client subnet option code
        }
        CHECK_FALSE(dns::encodeQuery("bad..name", dns::RecordType::A, 1, false));
    }

    TEST_CASE("responses follow CNAME chains through compressed names")
    {
        // api.test CNAME edge.cdn.test (300 s); edge.cdn.test A 192.0.2.10 (60 s) and A 192.0.2.11 (120 s).
        const QByteArray response =
            hex("abcd 8180 0001 0003 0000 0000"
                "03617069 0474657374 00 0001 0001"                       // question at 12: api.test
                "c00c 0005 0001 0000012c 000b 0465646765 03636e64 c010"  // CNAME -> edge.cdn.test
                "c026 0001 0001 0000003c 0004 c000020a"                  // edge.cdn.test A
                "c026 0001 0001 00000078 0004 c000020b");
        const auto decoded = dns::decodeResponse(response, "API.test", dns::RecordType::A);
        REQUIRE_MESSAGE(decoded, test::describe(decoded));
        CHECK(decoded->id == 0xabcd);
        CHECK(decoded->rcode == dns::Rcode::NoError);
        REQUIRE(decoded->addresses.size() == 2);
        CHECK(decoded->addresses[0].address == ip("192.0.2.10"));
        CHECK(decoded->addresses[0].ttl == 60);
        CHECK(decoded->addresses[1].ttl == 120);
    }

    TEST_CASE("NXDOMAIN carries the SOA negative TTL")
    {
        const QByteArray response =
            hex("0001 8183 0001 0000 0001 0000"
                "076d697373696e67 0474657374 00 001c 0001"
                "00 0006 0001 00000e10 0016 00 00 00000001 00000e10 00000258 00015180 0000001e");
        const auto decoded = dns::decodeResponse(response, "missing.test", dns::RecordType::Aaaa);
        REQUIRE(decoded);
        CHECK(decoded->rcode == dns::Rcode::NxDomain);
        CHECK(decoded->addresses.empty());
        CHECK(decoded->negativeTtl == 30U);  // min(SOA TTL 3600, MINIMUM 30)
    }

    TEST_CASE("unrelated, truncated and malicious messages are rejected")
    {
        const QByteArray answer = hex("0001 8180 0001 0000 0000 0000 03617069 0474657374 00 0001 0001");
        CHECK_FALSE(dns::decodeResponse(answer, "other.test", dns::RecordType::A));
        CHECK_FALSE(dns::decodeResponse(answer, "api.test", dns::RecordType::Aaaa));
        CHECK_FALSE(dns::decodeResponse(hex("0001 0100 0001 0000 0000 0000"), "api.test", dns::RecordType::A));
        CHECK_FALSE(dns::decodeResponse(hex("0001 81"), "api.test", dns::RecordType::A));
        // A compression pointer that points at itself.
        CHECK_FALSE(
            dns::decodeResponse(hex("0001 8180 0001 0000 0000 0000 c00c 0001 0001"), "api.test", dns::RecordType::A));
        // An A record with 5 bytes of data.
        CHECK_FALSE(dns::decodeResponse(hex("0001 8180 0001 0001 0000 0000 03617069 0474657374 00 0001 0001"
                                            "c00c 0001 0001 0000003c 0005 c000020a00"),
                                        "api.test", dns::RecordType::A));
        const auto truncated = dns::decodeResponse(
            hex("0001 8380 0001 0000 0000 0000 03617069 0474657374 00 0001 0001"), "api.test", dns::RecordType::A);
        REQUIRE(truncated);
        CHECK(truncated->truncated);
    }
}

TEST_SUITE("net.dns.cache")
{
    TEST_CASE("TTLs are clamped and entries expire")
    {
        DnsCache cache;
        cache.setLimits({.minTtl = 60s, .maxTtl = 600s, .negativeTtl = 20s});
        const auto t0 = DnsCache::Clock::now();
        cache.storeAddresses(QStringLiteral("Short.test"), {ip("192.0.2.1")}, 5s, t0);
        cache.storeAddresses(QStringLiteral("long.test"), {ip("192.0.2.2")}, 86400s, t0);
        CHECK(cache.lookup(QStringLiteral("short.test"), t0 + 59s));  // raised to the 60 s minimum
        CHECK_FALSE(cache.lookup(QStringLiteral("short.test"), t0 + 60s));
        CHECK(cache.lookup(QStringLiteral("long.test"), t0 + 599s));  // capped at 600 s
        CHECK_FALSE(cache.lookup(QStringLiteral("long.test"), t0 + 600s));
    }

    TEST_CASE("missing names are cached negatively, never longer than configured")
    {
        DnsCache cache;
        cache.setLimits({.minTtl = 60s, .maxTtl = 600s, .negativeTtl = 20s});
        const auto t0 = DnsCache::Clock::now();
        cache.storeMissing(QStringLiteral("gone.test"), 3600s, t0);
        cache.storeMissing(QStringLiteral("soon.test"), 5s, t0);
        const auto gone = cache.lookup(QStringLiteral("gone.test"), t0 + 19s);
        REQUIRE(gone);
        CHECK(gone->addresses.empty());
        CHECK_FALSE(cache.lookup(QStringLiteral("gone.test"), t0 + 20s));
        CHECK_FALSE(cache.lookup(QStringLiteral("soon.test"), t0 + 5s));
        cache.setLimits({.minTtl = 0s, .maxTtl = 600s, .negativeTtl = 0s});
        cache.storeMissing(QStringLiteral("never.test"), std::nullopt, t0);
        CHECK_FALSE(cache.lookup(QStringLiteral("never.test"), t0));
    }

    TEST_CASE("a full cache evicts expired entries first, then the soonest to expire")
    {
        DnsCache cache(2);
        cache.setLimits({.minTtl = 0s, .maxTtl = 1000s, .negativeTtl = 10s});
        const auto t0 = DnsCache::Clock::now();
        cache.storeAddresses(QStringLiteral("a.test"), {ip("192.0.2.1")}, 100s, t0);
        cache.storeAddresses(QStringLiteral("b.test"), {ip("192.0.2.2")}, 500s, t0);
        cache.storeAddresses(QStringLiteral("c.test"), {ip("192.0.2.3")}, 300s, t0);
        CHECK(cache.size() == 2);
        CHECK_FALSE(cache.lookup(QStringLiteral("a.test"), t0));
        CHECK(cache.lookup(QStringLiteral("b.test"), t0));
        CHECK(cache.lookup(QStringLiteral("c.test"), t0));
    }
}

TEST_SUITE("net.proxy")
{
    TEST_CASE("proxy URLs")
    {
        const auto socks = ProxyServer::fromUrl(u"socks5h://user:p%40ss@proxy.lan:1080");
        REQUIRE(socks);
        CHECK(socks->type == ProxyType::Socks5h);
        CHECK(socks->password == QStringLiteral("p@ss"));
        CHECK(socks->resolvesRemotely());
        CHECK(socks->curlUrl() == QStringLiteral("socks5h://proxy.lan:1080"));
        CHECK(socks->displayUrl() == QStringLiteral("socks5h://<redacted>@proxy.lan:1080"));
        CHECK_FALSE(socks->displayUrl().contains(u"user"));

        const auto bare = ProxyServer::fromUrl(u"proxy.corp:3128");
        REQUIRE(bare);
        CHECK(bare->type == ProxyType::Http);
        CHECK(bare->port == 3128);
        CHECK(ProxyServer::fromUrl(u"https://proxy.corp")->port == 443);
        CHECK(ProxyServer::fromUrl(u"socks://[2001:db8::1]")->curlUrl() ==
              QStringLiteral("socks5://[2001:db8::1]:1080"));
        CHECK_FALSE(ProxyServer::fromUrl(u"ftp://proxy"));
        const auto ofType = [](ProxyType type) {
            ProxyServer server;
            server.type = type;
            return server;
        };
        CHECK_FALSE(ofType(ProxyType::Socks5).resolvesRemotely());
        CHECK_FALSE(ofType(ProxyType::Socks4).resolvesRemotely());
        CHECK(ofType(ProxyType::Http).resolvesRemotely());
    }

    TEST_CASE("bypass rules: domains, wildcards, CIDR and local addresses")
    {
        const BypassList list({QStringLiteral("example.com"), QStringLiteral("*.cdn.test"), QStringLiteral(".corp"),
                               QStringLiteral("10.0.0.0/8"), QStringLiteral("2001:db8::/32")},
                              false);
        CHECK(list.matches(u"example.com"));
        CHECK(list.matches(u"API.example.com"));
        CHECK_FALSE(list.matches(u"notexample.com"));
        CHECK(list.matches(u"a.cdn.test"));
        CHECK_FALSE(list.matches(u"cdn.test"));
        CHECK(list.matches(u"intranet.corp"));
        CHECK(list.matches(u"corp"));
        CHECK(list.matches(u"10.1.2.3"));
        CHECK(list.matches(u"[2001:db8::5]"));
        CHECK_FALSE(list.matches(u"11.1.2.3"));
        CHECK_FALSE(list.matches(u"localhost"));
        CHECK_FALSE(list.matches(u"soundcloud.com"));

        const BypassList local({}, true);
        CHECK(local.matches(u"localhost"));
        CHECK(local.matches(u"printer"));
        CHECK(local.matches(u"192.168.1.20"));
        CHECK(local.matches(u"::1"));
        CHECK_FALSE(local.matches(u"soundcloud.com"));
        CHECK_FALSE(local.matches(u"8.8.8.8"));

        CHECK(BypassList({QStringLiteral("*")}, false).matches(u"anything.test"));
        CHECK(BypassList({QStringLiteral("<local>")}, false).matches(u"nas"));
        CHECK(BypassList::fromNoProxy(u"localhost, .internal  192.168.0.0/16").matches(u"svc.internal"));
    }

    TEST_CASE("bypass rules and hosts written with a trailing dot still match")
    {
        CHECK(BypassList({QStringLiteral("example.com.")}, false).matches(u"api.example.com"));
        CHECK(BypassList({QStringLiteral("*.cdn.test.")}, false).matches(u"a.cdn.test"));
        CHECK(BypassList({QStringLiteral("example.com")}, false).matches(u"api.example.com."));
        CHECK(BypassList({QStringLiteral("example.com")}, false).matches(u"example.com."));
        CHECK(BypassList({}, true).matches(u"localhost."));
        CHECK_FALSE(BypassList({QStringLiteral("example.com.")}, false).matches(u"example.org"));
    }

    TEST_CASE("environment variables pick the proxy per scheme and honour no_proxy")
    {
        QHash<QString, QString> env{{QStringLiteral("https_proxy"), QStringLiteral("http://secure.proxy:3128")},
                                    {QStringLiteral("HTTP_PROXY"), QStringLiteral("http://ignored:1")},
                                    {QStringLiteral("ALL_PROXY"), QStringLiteral("socks5h://all.proxy:1080")},
                                    {QStringLiteral("no_proxy"), QStringLiteral("internal.test,.lan")}};
        const auto https = environmentProxy(env, QUrl(QStringLiteral("https://api-v2.soundcloud.com/")));
        REQUIRE(https.server);
        CHECK(https.server->host == QStringLiteral("secure.proxy"));
        CHECK(https.variable == QStringLiteral("https_proxy"));
        // Upper-case HTTP_PROXY is ignored (httpoxy); all_proxy applies instead.
        const auto http = environmentProxy(env, QUrl(QStringLiteral("http://example.test/")));
        REQUIRE(http.server);
        CHECK(http.server->type == ProxyType::Socks5h);
        CHECK(http.variable == QStringLiteral("ALL_PROXY"));
        const auto bypassed = environmentProxy(env, QUrl(QStringLiteral("https://nas.lan/")));
        CHECK_FALSE(bypassed.server);
        CHECK(hasEnvironmentProxy(env));
        CHECK_FALSE(hasEnvironmentProxy({{QStringLiteral("HTTP_PROXY"), QStringLiteral("http://x:1")}}));
    }
}

TEST_SUITE("net.policy")
{
    TEST_CASE("retry decisions and backoff")
    {
        CHECK(RetryPolicy::retryableError(ErrorCode::ConnectionFailed));
        CHECK(RetryPolicy::retryableError(ErrorCode::Timeout));
        CHECK_FALSE(RetryPolicy::retryableError(ErrorCode::TlsCertificateInvalid));
        CHECK_FALSE(RetryPolicy::retryableError(ErrorCode::ProxyAuthRequired));
        CHECK_FALSE(RetryPolicy::retryableError(ErrorCode::DnsResolutionFailed));
        CHECK(RetryPolicy::retryableStatus(503));
        CHECK(RetryPolicy::retryableStatus(429));
        CHECK_FALSE(RetryPolicy::retryableStatus(404));
        CHECK_FALSE(RetryPolicy::retryableStatus(401));

        const RetryPolicy policy{.maxRetries = 5, .baseDelay = 500ms, .maxDelay = 10s};
        CHECK(policy.delay(0, std::nullopt, 0.5) == 500ms);
        CHECK(policy.delay(2, std::nullopt, 0.5) == 2000ms);
        CHECK(policy.delay(0, std::nullopt, 0.0) == 400ms);  // -20 %
        CHECK(policy.delay(0, std::nullopt, 0.999) >= 599ms);
        CHECK(policy.delay(10, std::nullopt, 0.5) == 10s);  // capped
        CHECK(policy.delay(0, 3s, 0.5) == 3s);              // the server asked for longer
        CHECK(policy.delay(0, 3600s, 0.5) == 10s);          // but never beyond the cap
    }

    TEST_CASE("Retry-After values")
    {
        const QDateTime now(QDate(2015, 10, 21), QTime(7, 28, 0), QTimeZone::UTC);
        CHECK(parseRetryAfter("120", now) == 120s);
        CHECK(parseRetryAfter("Wed, 21 Oct 2015 07:28:30 GMT", now) == 30s);
        CHECK(parseRetryAfter("Wed, 21 Oct 2015 07:00:00 GMT", now) == 0s);
        CHECK_FALSE(parseRetryAfter("soon", now));
        CHECK_FALSE(parseRetryAfter("", now));
    }

    TEST_CASE("a huge Retry-After is clamped to the maximum delay without overflowing")
    {
        const QDateTime now(QDate(2015, 10, 21), QTime(7, 28, 0), QTimeZone::UTC);
        const auto huge = parseRetryAfter("99999999999999999", now);
        REQUIRE(huge);
        CHECK(RetryPolicy{}.delay(0, huge, 0.5) == RetryPolicy{}.maxDelay);
        CHECK(RetryPolicy{}.delay(0, std::chrono::seconds::max(), 0.5) == RetryPolicy{}.maxDelay);
    }

    TEST_CASE("token bucket limits the rate after a short burst")
    {
        TokenBucket bucket;
        const auto t0 = TokenBucket::Clock::now();
        CHECK(bucket.tryConsume(1'000'000, t0));  // unlimited
        bucket.setRate(100'000, t0);              // 100 kB/s, 25 kB burst
        CHECK(bucket.tryConsume(30'000, t0));     // the burst may be overdrawn by one chunk
        CHECK_FALSE(bucket.tryConsume(1, t0));
        CHECK(bucket.waitTime(t0) >= 50ms);
        CHECK_FALSE(bucket.tryConsume(1, t0 + 40ms));  // 4 kB refilled, still 1 kB short
        CHECK(bucket.tryConsume(16'000, t0 + 100ms));
    }

    TEST_CASE("token bucket refills to the burst after a long idle period at a high rate")
    {
        TokenBucket bucket;
        const auto t0 = TokenBucket::Clock::time_point{};
        bucket.setRate(125'000'000, t0);  // 1000 Mbit/s, the largest setting
        const auto later = t0 + std::chrono::hours(24);
        CHECK(bucket.tryConsume(1, later));
        CHECK(bucket.waitTime(later) == 0ms);
        CHECK(bucket.tryConsume(40'000'000, later));  // the burst is a quarter second
        CHECK_FALSE(bucket.tryConsume(1, later));
    }

    TEST_CASE("redaction removes credentials and every query value")
    {
        const QString url = redactUrl(QUrl(QStringLiteral(
            "https://user:secret@cf-media.sndcdn.com/abc.mp3?Policy=eyJ&Signature=xyz&Key-Pair-Id=K1&empty=#frag")));
        CHECK_FALSE(url.contains(u"secret"));
        CHECK_FALSE(url.contains(u"user"));
        CHECK_FALSE(url.contains(u"eyJ"));
        CHECK_FALSE(url.contains(u"xyz"));
        CHECK_FALSE(url.contains(u"frag"));
        CHECK(url.contains(u"cf-media.sndcdn.com/abc.mp3"));
        CHECK(url.contains(u"Signature=<redacted>"));
        CHECK(redactHeader("Authorization", "OAuth 2-xyz") == "<redacted>");
        CHECK(redactHeader("cookie", "oauth_token=x") == "<redacted>");
        CHECK(redactHeader("Accept", "application/json") == "application/json");
    }

    TEST_CASE("curl failures map to typed errors")
    {
        CHECK(errorFromCurl({.code = CURLE_COULDNT_CONNECT, .osErrno = ECONNREFUSED}) == ErrorCode::ConnectionRefused);
        CHECK(errorFromCurl({.code = CURLE_COULDNT_CONNECT, .viaProxy = true}) == ErrorCode::ProxyConnectFailed);
        CHECK(errorFromCurl({.code = CURLE_RECV_ERROR, .connectStatus = 407, .viaProxy = true}) ==
              ErrorCode::ProxyAuthRequired);
        CHECK(errorFromCurl({.code = CURLE_PROXY, .proxyError = CURLPX_USER_REJECTED}) == ErrorCode::ProxyAuthRequired);
        CHECK(errorFromCurl({.code = CURLE_PEER_FAILED_VERIFICATION}) == ErrorCode::TlsCertificateInvalid);
        CHECK(errorFromCurl({.code = CURLE_SSL_CONNECT_ERROR}) == ErrorCode::TlsHandshakeFailed);
        CHECK(errorFromCurl({.code = CURLE_OPERATION_TIMEDOUT}) == ErrorCode::Timeout);
        CHECK(errorFromCurl({.code = CURLE_COULDNT_RESOLVE_HOST}) == ErrorCode::DnsResolutionFailed);
        CHECK(errorFromCurl({.code = CURLE_ABORTED_BY_CALLBACK}) == ErrorCode::Cancelled);
    }

    TEST_CASE("the installed libcurl has what the transport needs")
    {
        const auto features = initialiseCurl();
        REQUIRE_MESSAGE(features, test::describe(features));
        CHECK(features->http2);
    }
}

TEST_SUITE("net.config")
{
    TEST_CASE("a host override written with a trailing dot is stored without it")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        REQUIRE(store.setUserValue(settings::keys::HostOverrides, QStringList{QStringLiteral("Api.Test.=192.0.2.7")}));
        const NetworkConfig config = configFromSettings(store);
        REQUIRE(config.dns.overrides.size() == 1);
        CHECK(config.dns.overrides[0].first == QStringLiteral("api.test"));  // also what the DRM helper maps
    }

    TEST_CASE("settings snapshot: presets, custom entries and derived values")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        namespace keys = settings::keys;
        NetworkConfig config = configFromSettings(store);
        CHECK(config.dns.mode == DnsMode::System);
        CHECK(config.proxy.mode == ProxyMode::System);
        CHECK(config.transport.bandwidthBytesPerSecond == 0);

        REQUIRE(store.setUserValue(keys::DnsMode, QStringLiteral("doh")));
        REQUIRE(store.setUserValue(keys::DohProvider, QStringLiteral("quad9")));
        REQUIRE(store.setUserValue(keys::DnsFallbackToSystem, false));
        REQUIRE(store.setUserValue(keys::BandwidthLimit, 8));
        REQUIRE(store.setUserValue(keys::HostOverrides, QStringList{QStringLiteral("Api.Test=192.0.2.5")}));
        config = configFromSettings(store);
        CHECK(config.dns.strict());
        CHECK(config.dns.doh.url.host() == QStringLiteral("dns.quad9.net"));
        CHECK(config.dns.doh.bootstrap.size() == 4);
        CHECK(config.dns.providerName == QStringLiteral("Quad9"));
        CHECK(config.transport.bandwidthBytesPerSecond == 1'000'000);
        REQUIRE(config.dns.overrides.size() == 1);
        CHECK(config.dns.overrides[0].first == QStringLiteral("api.test"));

        REQUIRE(store.setUserValue(keys::DnsMode, QStringLiteral("custom")));
        REQUIRE(store.setUserValue(keys::DnsServerPreset, QStringLiteral("custom")));
        REQUIRE(store.setUserValue(keys::DnsServers,
                                   QStringList{QStringLiteral("192.0.2.53"), QStringLiteral("[2001:db8::53]:5353")}));
        config = configFromSettings(store);
        REQUIRE(config.dns.servers.size() == 2);
        CHECK(config.dns.servers[1].port == 5353);

        REQUIRE(store.setUserValue(keys::ProxyMode, QStringLiteral("manual")));
        REQUIRE(store.setUserValue(keys::ProxyType, QStringLiteral("socks5h")));
        REQUIRE(store.setUserValue(keys::ProxyHost, QStringLiteral("proxy.lan")));
        REQUIRE(store.setUserValue(keys::ProxyPort, 1080));
        config = configFromSettings(store);
        CHECK(config.proxy.manual.curlUrl() == QStringLiteral("socks5h://proxy.lan:1080"));
        CHECK_FALSE(config.proxy.manual.hasCredentials());

        // The usual bracketed IPv6 form is bracketed exactly once in the proxy URL.
        REQUIRE(store.setUserValue(keys::ProxyType, QStringLiteral("http")));
        REQUIRE(store.setUserValue(keys::ProxyHost, QStringLiteral("[2001:db8::1]")));
        REQUIRE(store.setUserValue(keys::ProxyPort, 3128));
        config = configFromSettings(store);
        CHECK(config.proxy.manual.curlUrl() == QStringLiteral("http://[2001:db8::1]:3128"));
        CHECK(config.proxy.manual.displayUrl() == QStringLiteral("http://[2001:db8::1]:3128"));
    }

    TEST_CASE("network settings reject malformed values with a precise error")
    {
        settings::SettingsStore store(settings::Schema::builtin());
        namespace keys = settings::keys;
        const auto rejects = [&](std::string_view key, const QVariant& value) {
            const auto result = store.setUserValue(key, value);
            return !result && result.error().code == ErrorCode::ConfigInvalidValue;
        };
        CHECK(rejects(keys::DnsServers, QStringList{QStringLiteral("dns.google")}));
        CHECK(rejects(keys::DohUrl, QStringLiteral("ftp://dns.example/")));
        CHECK(rejects(keys::DohUrl, QStringLiteral("https://user:pw@dns.example/dns-query")));
        CHECK(rejects(keys::DohBootstrap, QStringList{QStringLiteral("1.1.1")}));
        CHECK(rejects(keys::HostOverrides, QStringList{QStringLiteral("api.test")}));
        CHECK(rejects(keys::HostOverrides, QStringList{QStringLiteral("api.test=nowhere")}));
        CHECK(rejects(keys::ProxyBypass, QStringList{QStringLiteral("bad host")}));
        CHECK(rejects(keys::ProxyHost, QStringLiteral("proxy:8080")));
        CHECK(store.setUserValue(
            keys::ProxyBypass,
            QStringList{QStringLiteral("*.cdn.test"), QStringLiteral("10.0.0.0/8"), QStringLiteral("<local>")}));
        CHECK(store.setUserValue(keys::DohUrl, QStringLiteral("https://dns.nextdns.io/abc123")));
        CHECK(store.setUserValue(keys::ProxyHost, QStringLiteral("2001:db8::1")));
    }
}
