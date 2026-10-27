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
7. **Network transport (tentative, confirmed by the spike in slice c)**: libcurl multi plus our own DNS layer (system / UDP+TCP custom servers / RFC 8484 DoH with TTL cache). Resolved addresses are pinned with `CURLOPT_RESOLVE`, so curl never calls `getaddrinfo` for app traffic. This makes strict mode provable and gives us TTL clamping, negative caching, EDNS padding and per-host diagnostics, none of which curl's built-in `CURLOPT_DOH_URL` exposes.
8. **Native playback (tentative)**: FFmpeg demux/decode reading through a custom `AVIOContext` from `StreamSource`, with `libswresample` and output to `QAudioSink`. `QMediaPlayer` cannot guarantee that every byte goes through our transport and cache, and its seek and buffering control is coarse.
9. **Tests**: doctest. Qt-dependent tests create a `QCoreApplication` or `QGuiApplication` (offscreen) in a custom `main`.
10. **UI scale**: applied live through Theme token multipliers rather than `QT_SCALE_FACTOR`, which would need a restart.
11. **Glass pipeline in QML**: a `ShaderEffectSource` chain (capture, Kawase levels / Gaussian passes) rather than a `QQuickRhiItem`. Each pass renders only when its input changes, so a static scene costs nothing, and all GPU APIs are covered without per-API code. Texture providers stay visible at zero size, because invisible `ShaderEffectSource`s are never updated.
12. **Panels find the blurred texture through the window** (`Window.window.backdropTexture`) and map their own fragments to window UVs from `qt_Matrix` in the vertex shader (`Graphics.ndcYUp` handles the GL/Vulkan NDC difference). This needs no position bindings, so it stays correct during scrolling and animations.
13. **Software path = Solid** (per the spec). The cover backdrop is pre-blurred on the CPU, so it also works without a GPU.
14. **Frame cap by render-thread pacing**: Qt Quick has no frame-rate API. Sleeping after presentation on the render thread caps the rate without distorting animation timing. It is never applied on the GUI thread.
15. **Presets change colours only** (Midnight / Frost, untranslated names), per the user's review of slice (a).

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

Done: (a), (b).

## Open items flagged for verification
- castlabs Electron version and the `configureHostResolver` secure-DNS support (slice i).
- Current SoundCloud api-v2 behaviour, including `client_id` discovery and transcodings (slice e).
- Glass GPU timings on a mainstream iGPU. The dev machine has a GTX 1070, so iGPU numbers need another machine; documented as such.
