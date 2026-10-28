# Noctara: implementation plan

Noctara is a native SoundCloud desktop client written in C++23 with Qt 6 Quick, built with xmake, clang and mold.

## Verified environment (2026-10-27)

| Component | Version | Notes |
|---|---|---|
| clang / mold / xmake | 23.1.4 / 3.0.0 / 3.1.2 | `xmake f --qt=/usr/lib/qt6` is required; otherwise xmake picks Qt 5.15 |
| Qt | 6.12.0 | QQuickRhiItem, qsb, lrelease and `QFont` features (tnum) are all available. `<rhi/qrhi.h>` lives in the versioned private include directory |
| libcurl | 8.23 (system) | OpenSSL, HTTP/2 (nghttp2), HTTP/3 (ngtcp2), AsynchDNS (threaded resolver), HTTPS-proxy, DoH |
| FFmpeg | 9.0 (libavformat 63) | used by the native backend |
| TagLib / libsecret / sqlite / libproxy / c-ares / toml++ / simdjson | system | found through pkg-config |
| doctest | xrepo | |
| castlabs Electron | not installed | downloaded in slice (i) |

## Decisions (made without blocking; revisit if wrong)

1. **App id**: `noctara`. Config lives in `~/.config/noctara/config.toml`, cache in `~/.cache/noctara/` and state (crash sentinel) in `~/.local/state/noctara/`.
2. **QML type registration**: types are registered manually from C++ (`qmlRegisterSingletonInstance` etc.) and QML loads from generated `.qrc` files. The xmake `qt.qmltyperegistrar` rule targets plugins, and manual registration keeps ownership explicit.
3. **Generated resources**: custom xmake rules handle the build-time steps. `.vert/.frag` files are compiled to `.qsb` by `qsb`, `.ts` files to `.qm` by `lrelease`, and both are compiled into the binary through generated `.qrc` files. QML, fonts, icons and data files are listed in `.qrc` files generated at configure time.
4. **Settings docs**: `noctara-schema-doc`, a small tool linking only `core` and `settings`, prints the schema as Markdown. The `docs-config` task writes the output to `docs/configuration.md`, and CI diffs the result.
5. **Config file rewrite**: on save the file is regenerated from the schema. Every key is documented. Keys left at their default are written commented out, explicitly set keys are written as values, and unknown keys and tables are preserved verbatim through toml++. User comments are not preserved (documented).
6. **Warnings**: strict warnings apply only to our own sources through per-file flags. Qt and third-party headers are system includes, and generated code (moc, rcc) is excluded. `-Wuseless-cast` is GCC-only and clang has no equivalent, so it is documented and omitted.
7. **Network transport (confirmed by the slice (c) spike, see [network.md](network.md))**: libcurl multi on a dedicated network thread plus our own DNS layer (system / UDP+TCP custom servers / RFC 8484 DoH with TTL cache). Every host curl connects to, IP literals included, is pinned with `CURLOPT_RESOLVE`, and a resolver-start guard fails any transfer for which curl would resolve by itself. This makes strict mode provable and gives us TTL clamping, negative caching, EDNS padding and per-host diagnostics, none of which curl's built-in `CURLOPT_DOH_URL` exposes. Redirects are followed by Noctara, not curl, so every new host goes through the same rules.
8. **Native playback (tentative)**: FFmpeg demux/decode reading through a custom `AVIOContext` from `StreamSource`, with `libswresample` and output to `QAudioSink`. `QMediaPlayer` cannot guarantee that every byte goes through our transport and cache, and its seek and buffering control is coarse.
9. **Tests**: doctest. Qt-dependent tests create a `QCoreApplication` or `QGuiApplication` (offscreen) in a custom `main`.
10. **UI scale**: applied live through Theme token multipliers rather than `QT_SCALE_FACTOR`, which would need a restart.
11. **Glass pipeline in QML**: a `ShaderEffectSource` chain (capture, Kawase levels / Gaussian passes) rather than a `QQuickRhiItem`. Each pass renders only when its input changes, so a static scene costs nothing, and all GPU APIs are covered without per-API code. Texture providers stay visible at zero size, because invisible `ShaderEffectSource`s are never updated.
12. **Panels find the blurred texture through the window** (`Window.window.backdropTexture`) and map their own fragments to window UVs from `qt_Matrix` in the vertex shader (`Graphics.ndcYUp` handles the GL/Vulkan NDC difference). This needs no position bindings, so it stays correct during scrolling and animations.
13. **Software path = Solid** (per the spec). The cover backdrop is pre-blurred on the CPU, so it also works without a GPU.
14. **Frame cap by render-thread pacing**: Qt Quick has no frame-rate API. Sleeping after presentation on the render thread caps the rate without distorting animation timing. It is never applied on the GUI thread.
15. **Presets change colours only** (Auto / Midnight / Frost; Midnight and Frost untranslated), per the user's review of slice (a).
16. **Vulkan is preferred under Auto** (user request): pipeline cache on disk, GPU timestamps, device selection for *GPU preference*. OpenGL remains the fallback, including when Vulkan is only lavapipe.
17. **Selection indicators are computed, not bound by `Binding` elements**: the nav-rail and segmented-control pills derive their geometry from the model and the control size, so they follow resizes and never lag or duplicate.
18. **Secret store**: `auth::ISecretStore` uses Secret Service (libsecret, behind the injectable `IKeyring`) on a worker thread, then an AES-256-GCM encrypted-file store under the app state directory, then memory only as a last resort. Reads fall through to the fallbacks on a keyring miss, so a secret saved without a keyring is not lost when the keyring returns; writes remove lower-tier copies; removes reach every tier. The Accounts page and the proxy credentials row distinguish keyring, encrypted-local, and memory-only protection.
19. **Network diagnostics are redacted at the source**: request records keep URLs without user-info or query values, and proxy URLs are displayed as `scheme://<redacted>@host:port`, so nothing secret can reach the UI, the copied report or logs.
20. **Audio cache pieces**: 64 KiB chunks of progressive streams at fixed offsets in a sparse file plus a checksummed piece map (resumable, validated per piece); one piece per HLS segment. Entries are keyed by track id + transcoding. One fetch per entry serves all readers.
21. **Readers are bound to a generation** of an entry's file: tagging rewrites the file into a new generation, and readers opened earlier keep the old file through their descriptor, so a decoder never sees bytes shift.
22. **Tag-independent payload hash** (MPEG frames, MP4 `mdat` contents, Ogg packets) proves tagging left the audio byte-identical; TagLib audio properties check duration, rate and channels. A full decode comparison waits for FFmpeg (slice g).
23. **HTTP cache as a transport decorator** (`CachingTransport`), so the API client gets RFC 9111 caching, revalidation and stale-if-offline without knowing about it. Keys include a digest of the credentials.
24. **A second instance runs without disk caches** (flock on `<cache>/lock`) instead of sharing them, and says so on the Storage page.
25. **A custom cache location gets a `noctara` subfolder**, and caches are wiped only on a *different* layout-version marker, so pointing the setting at an existing folder can never delete user files.

26. **The API is its own module (`api`), not part of `net`**: it maps tracks to the cache's `TrackInfo`, so it sits
    above `cache`, and `net` stays free of SoundCloud knowledge. List models are a separate `models` module. See
    [api.md](api.md).
27. **simdjson (DOM, one parser per thread) behind `api/Parse`**, parsing into compact structs on the API thread.
    A 50-track page parses into structs about 3.4× faster than `QJsonDocument` builds its tree alone. URLs are
    kept as `QString`: building `QUrl`s cost more than the parse itself (see performance.md).
28. **One `CollectionModel` for tracks, playlists and users** (`Entry` = variant + timestamp + reposter), since
    search, the feed and likes mix kinds. Rows asked for while still stubs are hydrated through coalesced
    `/tracks?ids=` lookups.
29. **client_id: override → stored → discovered** (hydration data, then the last ≤ 8 asset scripts), single-flight and
    throttled for 30 s after a failed discovery; all site-specific knowledge lives in `api/ClientId.cpp`.
30. **The OAuth token goes only to SoundCloud's API hosts over https**, so a manipulated `next_href` or waveform URL
    cannot leak it. `api::Session` holds the token in memory; auth/ fills it from the secret store (slice f).
31. **Cover shapes are cut on the CPU by the image provider** (`image://cover/mask/<fraction>/…`, on Qt's image
    loader thread), so rounded and circular covers look the same with and without shader effects.
32. **One sign-in entry point** (`AccountController::signInWithToken`): manual paste and consented browser cookie
    import both check the token with its own `/me` request (`ApiRequest::candidateToken`, never cached) before it
    is installed in `api::Session` and stored through `ISecretStore`, so a bad token never disturbs a working
    session and the token is never in the config file. Session expiry comes from `api::Session::rejected` (a 401
    on any request) or a 401 on the background check. See [auth.md](auth.md).

## Slices

(a) build system, skeleton, settings schema/store/TOML, graphics bootstrap, Theme tokens (Midnight and Frost), fonts, LanguageManager (en/ru), generated settings UI
(b) shared backdrop and glass pipeline (Kawase, Gaussian, Liquid Glass, Solid, software), quality governor, AccentEngine, Appearance and Performance pages
(c) transport spike, INetworkTransport, DNS/DoH, proxy, Network page and diagnostics
(d) image and HTTP caches, then the audio RAM and disk tiers, Storage page
(e) SoundCloud API client and models
(f) auth / cookie extraction / secret store
(g) native playback via StreamSource and AudioCache, MPRIS
(h) pages, Layout and Keyboard customization, user themes
(i) DRM helper
(j) polish and performance measurements

Done: (a), (b), (c), (d), (e), (f).

## Slice (f) status (auth / secret store / session)

Done on Linux after a review round (separate "Review fix (f)" commits); see [auth.md](auth.md) for the formats and
the per-browser path table.

- Manual OAuth-token paste and **consented browser cookie import** share one verification path: the candidate is
  checked with its own `/me` request and only then installed in `api::Session`, so a refused token never disturbs a
  working session. Stale asynchronous answers (slow keyring read, superseded check, save after sign-out) are dropped.
- Chrome, Chromium, Brave, Edge, Vivaldi, Opera, Firefox and LibreWolf are discovered in native, Flatpak and Snap
  locations (Opera's profile-less layout included). Discovery exposes metadata only; the selected cookie DB (with its
  WAL/journal) is snapshotted read-only after explicit consent.
- Firefox cookies, and Chromium `v10` (`peanuts`) and `v11` (Safe Storage password from the Secret Service or KWallet)
  cookies, including the `SHA-256(host_key)` prefix of cookie DB version 24+ (Chromium 130+).
- `SecretServiceStore` (keyring, behind `IKeyring`) falls back to an AES-256-GCM file store under the state directory,
  then memory; reads fall through on a keyring miss, writes clean lower tiers, removes reach every tier. The Accounts
  page and the proxy credentials row show which protection is in use.
- Startup session loading, expiry on a `401` (with a "Sign in" toast action), offline retry with backoff, sign-out
  everywhere, en/ru strings, and the Accounts page are integrated. Tests use fixture databases with the real schemas
  and a fake keyring; only the opt-in `real browser profile import` touches the user's profiles and keyring.

Windows DPAPI/app-bound cookies and macOS Keychain remain platform-specific follow-ups (another
`IBrowserCookieSource`); unknown cookie encryption versions fail honestly and keep the manual-paste path.

## Open items flagged for verification
- Chromium-family import is verified against fixture databases built from Chromium's own format description
  (`os_crypt_linux`, cookie DB version 24), not against installed browsers; the KWallet D-Bus path has not run
  against a real wallet, and the Brave/Vivaldi/Opera Snap paths follow the Snap `$HOME` convention unchecked. Check
  with `noctara-tests --no-skip -tc="real browser profile import"` on a desktop with those browsers signed in.
- DoH over HTTP/2 against a local server is not covered by automated tests (the mock speaks HTTP/1.1); real providers negotiate HTTP/2 through ALPN.
- castlabs Electron version and the `configureHostResolver` secure-DNS support (slice i).
- Live api-v2 behaviour (client_id discovery, field names, transcodings, `/charts`) is covered by fixtures only; the
  live check (`noctara-tests --no-skip -tc="real api*"`) needs internet access and has not been run from the
  development sandbox. See [api.md](api.md#verification-status).
- Glass GPU timings on a mainstream iGPU. The dev machine has a GTX 1070, so iGPU numbers need another machine; documented as such.
