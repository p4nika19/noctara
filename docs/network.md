# Network

All of Noctara's HTTP traffic (API, images, audio, `client_id` discovery) goes through one interface,
`net::INetworkTransport`. DNS, proxy, TLS, time limits, connection limits, retries, the speed limit, offline and
metered modes, and diagnostics are therefore implemented exactly once, in `src/net/`.

## Transport decision (spike)

**Decision:** libcurl's multi interface, driven by the Qt event loop of a dedicated network thread
(`QSocketNotifier` per socket, one `QTimer`), with Noctara's own DNS stack in front of it. There is no thread per
request.

Why not `QNetworkAccessManager`: it has no pluggable resolver and no DoH. It can't prove that a hostname was never
handed to the system resolver, and it has no SOCKS5h.

Why not curl's own `CURLOPT_DOH_URL`: it offers no TTL clamping, negative-cache control, EDNS padding control,
fallback policy or per-host diagnostics. A strict "never call getaddrinfo" guarantee can't be verified with it
either.

How the pieces fit:

- Every host curl connects to is resolved by `net::DnsResolver` and pinned with `CURLOPT_RESOLVE`. This covers the
  destination and the proxy, and IPv4 literals too: the spike showed curl may still start its resolver for a
  literal proxy address after a refused connection.
- `CURLOPT_RESOLVER_START_FUNCTION` acts as a guard. If curl ever starts its own resolution, the transfer fails and
  `unexpectedCurlResolves()` increments. Every integration test asserts it stays 0.
- Redirects are followed by Noctara, not by curl. Each new host goes through the same resolver and proxy rules.
  `Authorization` and `Cookie` headers are dropped when a redirect leaves the host.
- `CURLOPT_PROXY` is always set: `""` disables curl's own environment lookup, because routing has already been
  decided by `net::ProxyRouter`.

Verified environment: system libcurl 8.23 (OpenSSL 3.6, nghttp2, ngtcp2/nghttp3). The build fails if libcurl lacks
SSL, HTTP2 or IPv6 (pkg-config `supported_features`). `net::initialiseCurl()` checks the same at startup, and
Noctara exits with a clear message instead of misbehaving. When libcurl has no HTTP/3, the HTTP/3 option is greyed
out with an explanation.

Spike measurements (release build, loopback mock servers, so this is the transport's own overhead; from
`noctara-tests --no-skip -tc="transport spike*"`):

| Measurement | Result |
|---|---|
| Sequential keep-alive GET, 1 KiB body | ≈ 0.30 ms per request |
| 200 concurrent GETs | 47 ms in total |
| Cached DNS lookup (`DnsResolver::resolve`) | 2.4 µs |
| Uncached DoH lookup, A + AAAA in parallel | 0.47 ms |

## Threads

```
GUI thread                                   network thread ("noctara-network")
──────────                                   ──────────────────────────────────
NetworkService ── settings (network.*) ────► NetworkCore
  │  config snapshot + proxy credentials       ├─ SystemResolver (getaddrinfo on a 4-thread pool)
  │                                            ├─ DnsResolver ── ServerUpstream (UDP/TCP sockets)
  │                                            │              └─ DohUpstream ──┐
  │                                            ├─ ProxyRouter ── LibproxyDesktopProxy (1-thread pool)
  │                                            └─ CurlTransport ◄──────────────┘ (DoH is POSTed through it)
  └─ transport().send(request, context, callbacks)  (thread-safe)
         callbacks run on `context`'s thread via CallbackChannel; dropped if `context` is gone
```

`send()` can be called from any thread. Callbacks arrive in order on the context object's thread. Destroying the
context cancels the request.

## DNS

| Mode | How names are resolved |
|---|---|
| System resolver (default) | `getaddrinfo`, on a worker pool |
| Custom DNS servers | Presets (Cloudflare, Google, Quad9, AdGuard) or your own addresses. UDP first, the same server over TCP when the answer is truncated, then the next server on timeout (2 s), error, SERVFAIL or REFUSED. Answers must come from the server asked, with the query's random ID and echoing its question. |
| DNS over HTTPS | RFC 8484 POST, `application/dns-message`, ID 0, over the shared transport (HTTP/2 via ALPN). Presets: Cloudflare, Google, Quad9, AdGuard, Mullvad, each with built-in bootstrap addresses. A custom URL must be `https://`. `http://` is accepted only for loopback/LAN servers with "Allow insecure DoH" on. |

Common to all modes:

- **Wire format:** EDNS(0) with a 1232-byte UDP payload. DoH queries are padded to 128-byte blocks (RFC 8467).
  EDNS Client Subnet is never sent.
- **Order of lookup:** IP literals, then `localhost` (RFC 6761), then host overrides, then the cache, then the
  configured mode.
- **Single flight:** concurrent lookups of one name share one query.
- **Address families:** A and AAAA are asked in parallel. Results are ordered by the IP version setting, and curl
  races both families (happy eyeballs).
- **Cache:** TTLs are clamped to [minimum, maximum]. The system resolver gives no TTL, so those answers live for the
  minimum. "No such name" is cached for min(SOA, negative lifetime). The cache is flushed on every DNS settings
  change and by "Flush DNS cache".
- **Fallback and strict mode:** "Fall back to the system resolver" (default on) applies only when the chosen servers
  are unreachable or answer nonsense. An authoritative NXDOMAIN is final, so falling back would only leak.
  With fallback off (**strict mode**) the system resolver is never called, not even for the DoH server's own name.
  A DoH server given by name therefore needs bootstrap addresses in strict mode. This is proven by tests that use a
  poisoned system resolver, both for the resolver alone and end to end through the transport.

## Proxy

| Mode | Behaviour |
|---|---|
| No proxy | Direct connections. |
| System (default) | Proxy environment variables if any is set: `https_proxy`/`HTTPS_PROXY` for https, `http_proxy` for http (upper-case `HTTP_PROXY` is ignored, as in curl, because of httpoxy), then `all_proxy`/`ALL_PROXY`; `no_proxy` is honoured. Otherwise the desktop's settings through libproxy (GNOME, KDE, PAC/WPAD), cached per scheme and host for a minute. The settings page shows which source is used and what it resolved to. |
| Manual | HTTP, HTTPS, SOCKS4, SOCKS5, or SOCKS5 with remote DNS (socks5h). The bypass list takes host names (with subdomains), `*.domain`, `.domain`, IP addresses, CIDR ranges, `<local>` and `*`. "Bypass for local addresses" (default on) adds localhost, private and link-local ranges and dot-less names. |

Proxy credentials are kept only in the system keyring (Secret Service via libsecret, under the key
`network.proxy.credentials`). They never go to the config file, logs, diagnostics or `--print-config`; proxy URLs
are always displayed as `scheme://<redacted>@host:port`. Without a keyring, credentials are kept in memory and the UI
says so. HTTP proxies negotiate authentication with `CURLAUTH_ANY`; SOCKS5 uses RFC 1929.

### DNS ↔ proxy

Who resolves destination names depends on the proxy type, and the Network page explains the current case:

| Proxy | Destination names resolved by | Noctara's DNS settings apply to |
|---|---|---|
| none | Noctara | everything |
| HTTP / HTTPS (CONNECT and absolute-URI requests) | the proxy | finding the proxy |
| SOCKS5h, SOCKS4a | the proxy | finding the proxy |
| SOCKS5, SOCKS4 | Noctara | everything (only addresses reach the proxy) |

There are no silent leaks. Behind a remote-resolving proxy, Noctara does not resolve destination names at all; tests
check that the resolver isn't even called. DoH queries always go directly to the DoH server, never through the
proxy. The proxy's own name is resolved with the DoH or custom servers, so this isn't circular.

## Other settings

- **Timeouts:** the connection timeout covers TCP, the proxy and the TLS handshake. The stall timeout aborts a
  download that receives nothing for that long (`CURLOPT_LOW_SPEED_*`).
- **Connection limits:** total and per server.
- **Retries:** only idempotent requests, and only for connection problems, timeouts or 408/429/500/502/503/504.
  Exponential backoff with ±20 % jitter, honouring `Retry-After`, capped at 30 s. Never after streamed data has
  reached the caller.
- **HTTP version:** Auto uses HTTP/2 over TLS; HTTP/3 falls back to older versions.
- **TLS:** minimum version 1.2 (or 1.3). An optional custom CA bundle replaces the system store, for TLS-inspecting
  networks. Peer and host verification are always on; there is no setting to turn them off.
- **Speed limit:** one token bucket for all transfers together (a quarter-second burst), implemented by pausing
  transfers.
- **Metered connection:** Auto follows NetworkManager's metered flag. While metered, requests marked `background`
  (prefetch) fail with `MeteredDeferred`.
- **Offline mode:** every request fails immediately with `Offline`, and running transfers are aborted.

All settings apply live. DNS, proxy and TLS changes make new requests use a fresh curl connection pool, while
running transfers finish on the old one, which is then closed ("drained").

## Diagnostics

The Network page shows:

- **Status:** connectivity (NetworkManager), metered and offline state, the DNS and proxy in effect, and the DNS ↔
  proxy hint.
- **Test DNS:** resolves `api-v2.soundcloud.com`, `soundcloud.com`, `cf-media.sndcdn.com` and `i1.sndcdn.com`
  bypassing the cache, and shows per host the addresses, which resolver answered, and how long it took.
- **Test connection:** a HEAD request to the API, media and image hosts through the current proxy, with
  translated, specific errors.
- **Recent requests:** the last 50 attempts with phase timings (DNS, connect, TLS, first byte, total), protocol,
  route and error.
- **Copy report:** a text report for bug reports.

Everything shown or copied is redacted. URLs lose user-info and every query value (signed media URLs and API calls
carry tokens there), and `Authorization`, `Proxy-Authorization` and `Cookie` values are never recorded.

## Errors

The `net` layer returns `ErrorCode`s, and `i18n/ErrorMessages` turns them into translated messages:
`DnsResolutionFailed`, `DnsServerUnreachable`, `DohUnreachable`, `DohInvalidResponse`, `DohInsecureUrl`,
`ProxyNotConfigured`, `ProxyConnectFailed`, `ProxyAuthRequired`, `TlsHandshakeFailed`, `TlsCertificateInvalid`,
`ConnectionFailed`, `ConnectionRefused`, `Timeout`, `Offline`, `MeteredDeferred`, `Cancelled`. HTTP error statuses
are responses, not errors; the API client (slice e) interprets 401/403/429.

## Tests

- **`net.*` unit suites:** address parsing, the DNS wire format against byte-exact fixtures (CNAME chains,
  compression, NXDOMAIN with SOA, malformed and malicious messages), cache clamping and eviction, proxy URLs, bypass
  rules, environment selection, retry policy, `Retry-After`, the token bucket, redaction, curl error mapping, the
  settings snapshot and validators.
- **`net.resolver` / `net.transport` / `net.service`:** run against local mock servers (`tests/mock/`): HTTP/1.1 and
  HTTPS origins with a generated certificate, DNS over UDP and TCP, DoH, an HTTP proxy with CONNECT and Basic auth,
  and SOCKS4/4a/5/5h with RFC 1929 auth. They cover:
  - strict mode with a poisoned system resolver, fallback, failover, TCP on truncation, single flight
  - absolute-form vs CONNECT, socks5h sending the name vs socks5 sending an address
  - certificate failures and a custom CA bundle
  - retries and redirects, streaming, cancellation, offline and metered modes, the speed limit
  - live reconfiguration through the settings on the real network thread

## Known limitations

- The mock DoH server speaks HTTP/1.1. DoH over HTTP/2 against real providers is negotiated by ALPN but isn't
  covered by an automated test, since a local HTTP/2 server would need nghttp2 server-side code in the tests.
- PAC/WPAD evaluation inside libproxy (System proxy mode with desktop settings) uses the system resolver. For a
  strict no-leak setup, use a manual proxy or the environment variables. Manual PAC URLs are not supported.
- Bypass rules match host names and literal addresses only; a host name is never resolved just to check CIDR rules
  (that would itself be a lookup).
- `getaddrinfo` cannot be interrupted. Shutting down while a system lookup hangs waits for the OS resolver's own
  timeout.
- The custom CA bundle replaces the system store rather than adding to it, matching curl's `CURLOPT_CAINFO`.
- The DRM helper (slice i) will receive the same proxy and secure-DNS configuration; that part is not done yet.
