---
name: product-spec-full
description: "Complete verbatim text of the user's original noctara prompt (all sections, slices (a)–(j), deliverables, working method, DoD); the authoritative statement of the project's aims"
metadata:
  node_type: memory
  type: reference
  originSessionId: 131aabcb-69d1-438d-a134-38c794bc2a95
  modified: 2026-10-28T07:09:55.354Z
---

The user re-pasted the complete original prompt on 2026-10-28 (during slice (e)) "to get the aims better". Everything below is verbatim. Condensed version: [[product-spec]]. Later corrections from the user override it: [[design-feedback]], [[autonomous-slices]].

# ROLE
You are a senior C++ systems/UI engineer. Build a production-quality, native SoundCloud desktop client in modern C++ with Qt 6. Code quality, performance, and maintainability are the top priorities. Work incrementally: plan first, then implement module by module, building and testing after each step.

# PRODUCT GOAL
A fast, lightweight, beautiful, deeply customizable native desktop SoundCloud client:
- Browse/search tracks, playlists, users, likes, feed, history, charts.
- Playback queue, gapless-ish transitions, seek, volume, shuffle/repeat, media keys, MPRIS (Linux).
- Looks and feels like a premium modern app, not a "toolkit demo".
- Two first-class themes: "Midnight" (dark) and "Frost" (light), plus System-follow.
- Fully localized UI (English + Russian minimum) with live language switching.
- Nearly everything configurable from the Settings UI AND from a human-editable config file.
- Works with GPU acceleration (default) and also without it (software fallback), user-controllable.
- Smart caching: tracks (non-DRM only) are cached in RAM and on disk under the standard user cache directory (~/.cache on Linux), so replays, seeking and next-track starts are instant and traffic is minimized.
- A dedicated Network settings section with a fully working DNS stack (system / custom / DNS-over-HTTPS) and proxy support (HTTP / HTTPS / SOCKS5), applied to ALL of the app's traffic.
- Idle CPU ≈ 0%, low RAM, fast cold start, GPU cost of the glass effects strictly budgeted (see UI PERFORMANCE).

# DESIGN DIRECTION: "MIDNIGHT GLASS" (dark) + "FROST" (light)
Glassmorphism with translucent frosted/liquid-glass surfaces floating above a living backdrop derived from the current track's cover art, soft glows, generous rounded corners, restrained motion that feels physical. Both themes share the same component set, geometry, motion and token structure; only token VALUES differ.

## Visual language (shared)
- Surfaces ("glass"): translucent panels with backdrop blur, 1px inner highlight border (light source top-left), subtle inner shadow, soft outer shadow, tiny noise overlay. Three elevation levels (base / raised / overlay) defined as tokens.
- Radii: large and consistent (e.g. 12 / 20 / 28 / pill), defined as tokens.
- Typography (fixed default font pack, all OFL):
  * Headings/display: Manrope (variable font).
  * Body/UI text: Inter (variable font).
  * Monospace metadata (timings, bitrate, counters, hex input): JetBrains Mono.
  Bundle the font files in Qt resources, load via QFontDatabase, enable tabular numerals (tnum) for all time/count displays, verify Latin AND Cyrillic rendering (all three families support Cyrillic), define explicit fallback chains, include OFL license texts in the repo and About dialog. Type scale (display / title / body / caption / mono-meta) as tokens. Users may override font families and scale from settings/config (see CUSTOMIZATION), falling back to the bundled fonts if a font is missing.
- Iconography: one consistent SVG icon set (stroke-based, 1.5px, rounded caps), loaded as cached/pre-rasterized assets, tinted via theme color tokens (works in both themes); no icon fonts.
- Motion: spring/ease-out-expo style curves, 120–320 ms for UI, 400–600 ms for accent/backdrop/theme crossfades; hover/press feedback on everything interactive; shared-element transition for cover art between list ↔ player ↔ full-screen; skeleton loaders (shimmer) instead of spinners. All motion respects "Reduce motion" and a global animation speed multiplier (0 = off).
- Layout: floating navigation rail (glass), central content with large cover-art cards and dense track rows, bottom floating "now playing" glass bar with waveform scrubber, expandable full-screen player with large art, synced backdrop, queue drawer, and waveform.
- Waveform: use the waveform data SoundCloud provides; render as a single GPU geometry (QSGGeometryNode or equivalent), played/unplayed portions via shader/uniform, hover-preview and smooth seek; no per-frame allocations and no Canvas. A software-rendering fallback implementation is required (see GPU ACCELERATION). The waveform may also show which part of the track is already buffered/cached.

## Theme: Midnight (dark)
Deep midnight background, never pure black: blue-violet-tinted neutrals. Backdrop: large, heavily blurred, slowly drifting (very slow, GPU-cheap, pausable) version of the current cover art + dark gradient scrim + fine grain. Glass is dark-tinted with brighter top-left rim highlight, glows allowed.

## Theme: Frost (light)
Milky frosted-white glass over a pale, cool-tinted backdrop: the cover-art backdrop is heavily lightened/desaturated-washed (target lightness range defined as tokens) so it never harms text legibility. Glass tint is white with higher opacity than in dark mode; dark text on glass; shadows are softer and tinted by the accent instead of pure black; rim highlight is bright white on top-left with a faint darker/cool edge on bottom-right; glow is replaced by soft colored tint/bloom; grain amount is lower. Liquid Glass parameters (specular, fresnel, rim darkening, saturation lift) have separate light-mode token values so refraction looks clean and not washed out. Waveform, selection, focus rings, skeletons, toasts, dialogs and shadows all have light-mode values. No dark-mode leftovers (hardcoded dark colors, white-only icons, etc.).

## Theme mode setting
Settings → Appearance → "Theme": [System / Midnight (dark) / Frost (light)], default System (follows QStyleHints::colorScheme and reacts live to OS changes). Switching crossfades over ~400 ms (single progress value; respects Reduce motion) without restart. Optional: "Schedule" (light by day / dark by night) as an extra setting if cheap to implement. All semantic tokens are defined as light/dark pairs (or generated from the palette generator per scheme), never as scattered conditionals in components.

## Tech constraints for the UI layer
- Qt Quick / QML for presentation ONLY, C++ for all logic (view-models/controllers exposed to QML). Prefer Qt 6.7+ (QQuickRhiItem, Qt Shader Tools). RHI-based scene graph for the accelerated path; shaders authored in GLSL and compiled with qsb at build time (xmake rule/task), never at runtime.
- One QML singleton "Theme" containing ALL design tokens (colors per scheme, spacing, radii, elevations, typography, durations, easings, blur parameters). No magic numbers in components.
- Reusable component library: GlassPanel, GlassButton, IconButton, Card, TrackRow, CoverArt, Slider, WaveformView, Tag, Skeleton, Toast, Dialog, Menu, NavRail, SegmentedControl, ColorPicker, Switch, ComboBox/LanguagePicker, SettingsRow/SettingsSection (generated from the settings schema). Keyboard navigation + focus visuals, HiDPI-correct, accessible names/roles (accessible names are translated).

## Dynamic accent colors (setting, ON by default)
Settings → Appearance → "Adapt accent colors to cover art" [switch, default: ON].
- ON: the accent palette is derived from the current track's cover art and crossfades smoothly (≈500 ms) when the track changes. Computed off the UI thread, cached per artwork (keyed by URL/hash, size-bounded cache), results delivered via queued signals.
- OFF: the user chooses a STATIC accent color (preset swatches + custom picker with hex input, live preview). The ENTIRE interface palette is generated from that single seed color using the same palette generator, so the UI looks equally coherent in both modes and both themes. The backdrop may still use the cover art (separate toggle "Use cover art as backdrop", default ON) but accents stay static.
- Persist settings, apply live without restart.
- AccentEngine design (C++, pure, unit-tested, no Qt Quick dependency):
  * Input: downscaled artwork (e.g. ≤ 64×64) converted to a perceptual space (OKLab/OKLCH).
  * Quantize/cluster (e.g. Wu/Celebi quantizer or k-means in OKLab), score candidates by population × chroma with penalties for near-black/near-white/dull colors; choose the seed deterministically.
  * Edge cases: grayscale or low-chroma art → tasteful default or keep the previous accent; very dark/light art → clamp lightness; never produce unreadable results.
  * Generate a tonal palette from the seed (OKLCH or HCT-like) FOR BOTH SCHEMES (dark and light): roles = accent, accentHover, accentPressed, accentContainer, onAccent, glassTint, glow/bloom, backdropGradient stops, waveformPlayed/Unplayed, selection, focusRing.
  * Enforce contrast in BOTH schemes: text/icons on glass ≥ WCAG AA (4.5:1; 3:1 for large text/graphical elements) by adjusting tone; verify with unit tests over fixture images including pathological ones (all-white, all-black, grayscale, neon, low-saturation pastel).
  * Palette is exposed to QML as a single immutable value object (Q_GADGET) to avoid dozens of property updates; animate between old and new palette with a single progress value where possible.

## Blur / glass rendering (Settings → Appearance → "Glass style")
One interface (IGlassRenderer / a single QML-facing "GlassPanel" component that picks the implementation), switchable live:
1. **Dual Kawase blur** (default). Downsample/upsample chain on a shared mip pyramid (5-tap down, 8-tap up), configurable iterations + offset mapped to a "Blur strength" slider; very cheap, smooth, no banding thanks to the grain overlay. Reference: the standard dual-filter blur algorithm (Marius Bjørge, ARM, SIGGRAPH 2015).
2. **Gaussian** (separable, two-pass, linear-sampling optimization) at reduced resolution: higher "classic" quality, higher cost.
3. **Liquid Glass**. A refractive glass style implemented as an ORIGINAL shader (do not copy any vendor's assets or code): rounded-rect SDF per surface; near-edge refraction/lensing that displaces the sampled backdrop along the SDF gradient (magnitude and thickness as tokens); mild chromatic dispersion at the rim; specular highlight and fresnel-like rim light driven by a light direction (optionally subtly following the cursor, disabled with Reduce motion); slight saturation/brightness lift of the refracted backdrop; inner glow and soft shadow; subtle "squash/morph" micro-animations on press. Backed by the same blurred backdrop texture (lightly blurred for the refracted sample). Applied only to a LIMITED set of surfaces (now-playing bar, nav rail, popovers, toasts, full-screen player controls), never to every list row. Separate parameter sets for dark and light themes.
4. **Solid (no blur)**: opaque tinted surfaces; automatic fallback when the GPU is weak/absent, when blur is unsupported, on "Reduce transparency", when hardware acceleration is OFF, or when the performance governor forces it.
Additional settings: Blur strength, Glass opacity/tint amount, Grain amount, Reduce transparency, Reduce motion, Quality (Auto/Low/Medium/High).

## UI PERFORMANCE (the glass look must be cheap)
- SINGLE shared backdrop pipeline: capture the backdrop ONCE per frame-when-dirty into a half/quarter-resolution texture, blur it once, and let ALL glass panels sample that shared result (region + tint + noise). NEVER one blur per panel, NEVER per-item ShaderEffect blur in lists.
- The blur is recomputed only when its source changes (scrolling content under glass, cover/backdrop crossfade, theme crossfade, window resize). Static scenes cost zero GPU blur work; cap the update rate while scrolling if needed; the blurred cover-art backdrop is a cached pre-blurred texture generated off-thread on track change.
- Budget: blur + glass ≤ ~1 ms GPU on a mainstream iGPU at 1440p (document the measured value, for both themes); total frame ≤ 8.3 ms target for 120 Hz displays where available, always sustain 60 FPS.
- Adaptive quality governor: monitor frame times (frameSwapped / render timings); if frames overrun repeatedly, step down automatically (blur resolution → iterations → liquid glass to Dual Kawase → Solid) and expose the state in Settings; step back up conservatively. "Auto" quality uses this; manual levels disable it.
- Pause backdrop animation and blur updates when the window is unfocused/minimized/occluded or on battery saver (if detectable); drop to a static pre-rendered background.
- Avoid overdraw: opaque regions marked opaque, clip aggressively, no full-screen translucent layers stacked, no layer.enabled on large items unless cached; batching-friendly scene-graph items; no per-frame QML binding churn or JS animation loops (use declarative animations/shader uniforms).
- Lists: virtualized ListView/GridView, reuseItems, tuned cacheBuffer, compact model structs; CoverArt with async decode and sourceSize clamped to display size × DPR, memory+disk LRU with byte budgets, request dedup, fade-in.
- Verify with QSG_VISUALIZE (overdraw/batches), the QML profiler, RenderDoc (shader passes), and document findings in docs/performance.md.

# GPU ACCELERATION (user-controllable, with robust fallback)
- Settings → Performance → "Hardware acceleration": [Auto (default) / On / Off]. Also available as config key, CLI flag (--disable-gpu / --gpu=auto|on|off) and environment variable override.
- Additional settings: Graphics API (Auto / Vulkan / OpenGL on Linux; D3D11/D3D12/OpenGL on Windows; Metal on macOS, only options actually available on the platform are shown), GPU preference (integrated / discrete, where selectable), VSync on/off, Frame rate cap (Auto = display refresh rate / 30 / 60 / 120 / 144 / Unlimited), "Reduce GPU usage when on battery".
- IMPORTANT constraint: the Qt graphics API/backend (RHI vs software) is chosen before the window is created, so changing hardware acceleration or the graphics API requires an app restart. The UI must clearly say so and offer "Restart now" (relaunch via RAII-managed self-exec). Do not pretend it switches live.
- Hardware acceleration OFF = Qt Quick software renderer (QT_QUICK_BACKEND=software or the QQuickWindow graphics API setting for the software adaptation). Consequences MUST be handled by design, not by crashing:
  * ShaderEffect, custom QSGMaterial and QQuickRhiItem do not work in the software backend. Every GPU-only component needs a CPU/software-safe fallback: GlassPanel → Solid tinted surface (optionally a pre-blurred static backdrop image generated on the CPU off-thread, not live); WaveformView → a QQuickPaintedItem/Canvas-free cached-QImage implementation; grain overlay → pre-rendered tiled texture; Liquid Glass/Gaussian/Kawase options are disabled and greyed out with an explanatory translated tooltip.
  * Animations are automatically simplified (lower FPS cap, fewer simultaneous animations, no drifting backdrop), and the governor is disabled.
  * Image decoding, accent extraction and pre-blur remain off-thread on the CPU in all modes; use SIMD-friendly code (std::simd or compiler auto-vectorization, verified) in hot CPU paths so the software mode stays usable.
- Auto mode: probe the graphics stack at startup (RHI device creation, driver/renderer string, software-rasterizer detection like llvmpipe/SwiftShader, known-bad driver blocklist stored as data, not code). If the GPU is missing, is a software rasterizer, or fails to initialize → fall back to software mode and show a one-time translated notice. Handle device-lost / context-loss at runtime (QQuickWindow::sceneGraphError / device lost): recover or fall back, never crash.
- Crash-loop protection: if the previous launch crashed during graphics init (sentinel file written before init, removed after the first successful frame), the next launch starts in Safe Mode (software rendering, Solid glass, no shader features) and tells the user how to return to normal mode. Also provide a --safe-mode CLI flag.
- The DRM Electron helper follows the same setting: GPU/compositing disabled by default for the helper (audio-only); never required for playback.
- docs/gpu.md: matrix of supported backends, fallbacks, known limitations of the software path, how to debug rendering issues.

# NETWORK (dedicated settings section: DNS, DoH, proxy)
Settings → Network is its own page (separate from Storage & Cache). Every option is also a config key, CLI/env-overridable, and comes from the settings schema like everything else. Network settings apply LIVE without restart (new connections use the new configuration; idle keep-alive connections are drained; the Electron helper is reconfigured or restarted), except where explicitly stated.

## Transport architecture (decide with a short spike, document the decision)
- Put ALL of the app's HTTP traffic (API, images, audio streams, client_id discovery) behind ONE interface (INetworkTransport) so DNS, proxy, TLS, timeouts, connection limits and diagnostics are implemented exactly once.
- Qt's QNetworkAccessManager has no pluggable DNS resolver and no DoH. Unless your spike proves otherwise, implement the transport on libcurl (multi interface integrated with the Qt event loop via curl_multi_socket_action + QSocketNotifier/QTimer; no blocking threads per request), which provides HTTP/2 (HTTP/3 optional), DoH (CURLOPT_DOH_URL), HTTP/HTTPS/SOCKS4/SOCKS5/SOCKS5h proxies, DNS cache and happy eyeballs. Verify the xrepo curl package is built with the required features (TLS, nghttp2, DoH, c-ares/threaded resolver) and fail the build/startup self-check with a clear message otherwise. Keep Qt Network only for what does not touch the internet (local sockets etc.).
- Native audio playback must NOT let the media framework open remote URLs itself, because that would bypass DNS/proxy/cache settings. NativeBackend reads bytes from a StreamSource (QIODevice / AVIOContext-style custom reader) that is fed by the transport through the audio cache (see CACHING).

## DNS
Settings → Network → DNS → "DNS mode": [System resolver (default) / Custom DNS servers / DNS over HTTPS (DoH)].
- System: the OS resolver (getaddrinfo), nothing special.
- Custom DNS servers: one or more IPv4/IPv6 resolvers (presets: Cloudflare 1.1.1.1, Google 8.8.8.8, Quad9 9.9.9.9, AdGuard, plus custom entries), UDP with TCP fallback on truncation, ordered failover with timeouts.
- DoH (RFC 8484, HTTP/2, wire-format dns-message): provider presets (Cloudflare, Google, Quad9, AdGuard, NextDNS-style custom) plus a custom URL (https:// only; plain http:// accepted solely for loopback/LAN with an explicit "allow insecure" switch). Bootstrap IPs for the DoH host so resolving the resolver is not circular (built-in for presets, user-supplied for custom). Option "Fallback to system DNS if DoH fails" (default ON for reliability, with a translated hint about the privacy trade-off; OFF = strict mode, requests fail with a clear DoH error instead of leaking to the system resolver). EDNS padding on, EDNS Client Subnet never sent.
- Common DNS options: IP family preference (Auto / Prefer IPv4 / Prefer IPv6 / IPv4 only / IPv6 only), parallel A+AAAA with happy eyeballs, in-process DNS cache honoring TTL with configurable min/max clamp and negative caching, "Flush DNS cache" button, optional static host overrides (domain → IP) for advanced users (config + Advanced UI).
- "Test DNS" button: resolves the app's real hosts (e.g. the SoundCloud API/CDN hosts) with the current settings and shows per-host result, resolver used and latency (translated, copyable, no secrets).
- Strict mode guarantee: when DoH strict mode is on, the system resolver must never be called for app traffic (verify with a test that uses a mock/poisoned system resolver).

## Proxy
Settings → Network → Proxy → "Proxy mode": [No proxy / System (default) / Manual].
- System: environment (http_proxy, https_proxy, all_proxy, no_proxy and uppercase variants) plus desktop settings where reliably available (e.g. GNOME/KDE via libproxy or Qt's system proxy query); show which source was detected.
- Manual: type [HTTP / HTTPS / SOCKS4 / SOCKS5 / SOCKS5 with remote DNS (socks5h)], host, port, optional username/password, bypass list (hostnames, wildcards, CIDR; "bypass local addresses" switch default ON). Optional PAC URL if feasible via libproxy; otherwise document it as a known limitation and keep the UI honest.
- Proxy credentials are stored ONLY in the secure secret store (libsecret/Keychain-style), never in the config file, logs, diagnostics or --print-config output (redacted).
- "Test connection" button: checks reachability and latency of API and media hosts through the configured proxy, with specific translated errors (auth required, refused, timeout, TLS failure, DNS failure).
- DNS ↔ proxy interaction must be explicit and documented in the UI with a translated hint: with socks5h and HTTP CONNECT proxies, hostname resolution of destination hosts happens at the proxy; DoH/custom DNS then apply only to direct connections and to resolving the proxy host itself. No silent DNS leaks: when a proxy is configured, the app must not resolve destination hostnames locally unless the proxy type requires it (plain SOCKS4/SOCKS5), and the UI says so.

## Other network settings
Connect/read timeouts, max concurrent connections (total / per host), retry policy (count, backoff), HTTP version preference (Auto / HTTP/1.1 / HTTP/2 / HTTP/3 if available), TLS minimum version (default 1.2), CA store (system default / custom CA bundle path for corporate MITM setups; certificate validation is NEVER disabled), optional bandwidth limit, "Metered connection" mode (reduces prefetch and background activity), Offline mode (serve from cache only).
- "Network diagnostics" panel: current DNS mode and resolver, active proxy (credentials redacted), recent request timings and errors, connectivity status; "Copy report" exports a redacted text report.

## Electron helper and network settings
- The headless Electron helper must honor the same network configuration so it neither leaks DNS nor bypasses the proxy: pass the proxy (e.g. --proxy-server / app.setProxy) and secure DNS settings (app.configureHostResolver with secureDnsMode/secureDnsServers where the Electron version supports it) at spawn and on live changes via IPC; verify actual behavior and document limitations in docs/network.md. If a setting cannot be honored by the helper, the UI shows a translated warning for DRM playback.

## Errors
Typed error codes (DnsResolutionFailed, DohUnreachable, DohInvalidResponse, ProxyConnectFailed, ProxyAuthRequired, TlsHandshakeFailed, Timeout, Offline, …) from net/; the UI maps them to translated, actionable messages.

## Tests
DoH wire encode/decode against fixtures and a local mock DoH server (HTTP/2), fallback and strict-mode logic, TTL/negative caching, IP-family selection, proxy rule parsing and bypass matching (wildcard/CIDR), HTTP-CONNECT and SOCKS5/5h through local mock proxies, "no system resolver call" leak test, settings validation and live reconfiguration, redaction of secrets in diagnostics and --print-config.

# CACHING (RAM + disk, standard cache directory)
Settings → Storage & Cache is its own page. Caches are disposable by definition: losing them must never break the app.

## What is cached
1. Audio of regular (NON-DRM) tracks — two tiers, below.
2. Images (cover art, avatars): memory LRU + disk LRU with byte budgets, request dedup, decode-size-aware keys.
3. API responses / metadata: honor ETag/Last-Modified/Cache-Control (never store responses marked no-store); short TTL for volatile endpoints (feed, likes), longer for immutable ones (track metadata, waveforms).

## Audio cache: tier 1, memory
- Byte-budgeted store of ENCODED audio chunks (e.g. 64–256 KiB chunks, sparse range map), not decoded PCM, for the current and prefetched tracks; seeking inside already-buffered ranges is instant; small tracks may be held fully in RAM.
- Default budget modest (e.g. 64–128 MB), configurable; respond to memory pressure; no unbounded growth; pooled/arena allocation (std::pmr) for chunk buffers if profiling justifies; zero copies on the hot path where possible (shared immutable buffers, spans).
- Prefetch: the next N queue items (configurable, default 1–2, reduced on Metered mode and when the cache is disabled) are warmed into RAM/disk without blocking the current stream; cancel prefetch when the queue changes.

## Audio cache: tier 2, disk
- Location: the platform-standard user cache directory via QStandardPaths::CacheLocation. On Linux that is ~/.cache/<app-id>/ (respect XDG_CACHE_HOME); %LOCALAPPDATA% / ~/Library/Caches elsewhere. Layout: <cache>/audio/, <cache>/images/, <cache>/http/, <cache>/electron/ (helper profile, see below), plus a version marker file to invalidate incompatible layouts. User may override the location in settings/config (validated, must be writable).
- Entries are keyed by (track id, transcoding protocol/codec/quality preset), NEVER by the signed stream URL (URLs expire; re-resolve a fresh URL when a cached entry is incomplete). Invalidate on track update/removal (updated_at / ETag change).
- Progressive streams: store by byte ranges (chunk bitmap per entry, resumable partial entries); HLS: store segments plus a normalized playlist, so a finished entry replays fully offline.
- Single-flight: concurrent readers of the same range share one network request. Reads via mmap/pread; writes off the UI thread; atomic finalization (temp file + rename); size/checksum validation on read, corrupt or truncated entries are dropped silently and refetched.
- Eviction: LRU by last access with a hard byte cap (default e.g. 2 GB, configurable, 0 = disabled) and a free-disk-space floor; handle ENOSPC gracefully (stop writing, evict, never crash, never block playback). Directory permissions 0700; lock file so multiple app instances do not corrupt the index; the index is loaded lazily/asynchronously at startup (no startup stall with large caches).
- The audio cache is an internal playback optimization: files are stored in the app's own opaque layout; do not add "export/save audio" features.
- Settings: Enable audio cache [ON], RAM budget, disk budget, cache location, prefetch count, "Clear audio cache / image cache / all" with confirmation, "Clear cache on exit" switch, usage meter with per-category breakdown (translated, locale-formatted sizes), Offline mode integration (cached tracks remain playable offline).

## Audio cache: metadata and cover art (every entry is self-describing)
- No anonymous entries. When a cache entry is created (the first byte of a track is requested), the metadata record is written FIRST, atomically (temp file + rename), as a versioned sidecar next to the entry: track id, title, artist/uploader, album or playlist context if any, genre, duration, created/release date, permalink, artwork URL, waveform reference, codec/bitrate/quality preset, content length, source updated_at/ETag, cached-at. Any entry on disk, partial or complete, resolves to correct metadata without network access.
- Cover art is fetched together with the track at high priority (original/large size plus a small thumbnail), stored once per unique image (content-hash dedup) and linked from the entry. If the cover request fails, the entry is still created and the cover is retried later; a placeholder is never persisted as if it were the real cover.
- Offline mode, the "Cached" library view, the queue, MPRIS and notifications use this sidecar data (title, artist, cover, duration), so cached tracks look and behave identically online and offline.
- Embedded tags on finalization (setting "Embed tags in cached audio", default ON): when an entry is complete and validated, write standard tags into the audio file with TagLib: ID3v2.4 for MP3, MP4 atoms for AAC/M4A, Vorbis comments + embedded picture for Opus/Ogg. Fields: title, artist, album (only if real), genre, date, permalink URL, front cover embedded as received (no re-encoding, sane max dimension). Work on a temporary copy off the UI thread, then atomic rename; audio frames must remain byte-identical (verify payload hash before/after and that duration/decoding are unchanged). If the container is not taggable without remuxing (e.g. HLS segments), keep the sidecar only and do not remux.
- Text handling: UTF-8/UTF-16 frames, never mojibake; correct Cyrillic, CJK and emoji; strip control characters; keep tags in sync with the sidecar.
- Freshness: when the track's updated_at/ETag changes, refresh the sidecar and cover, and re-tag lazily (on next access or idle). A crash or ENOSPC during tagging must leave the previous valid entry intact.
- Partial entries are never tagged (they have a sidecar only). DRM tracks never get entries, sidecars or tags (see DRM policy).
- This is an integrity/metadata feature of the internal cache, not a download feature: the layout stays opaque to users (no export/"save as" UI, no human-readable file names).

## DRM-protected tracks
- Client-side caching of DRM tracks is NOT done, in any tier. They are decrypted only inside the CDM in the Electron helper, and persisting protected media segments, keys or license data is out of scope for this project by design. No RAM cache, no disk cache, no prefetching of their media data by the C++ client; the UI marks such tracks as "not cacheable" (translated). Allowed: warming up the helper (e.g. resolving the next protected track's metadata, or loading it paused) as a latency optimization, without storing media.
- The helper itself: only Chromium's internal in-memory media buffering is used. Configure the helper with no persistent media/HTTP cache (in-memory session partition or disabled disk cache), userData under <cache>/electron/ containing only non-media profile data, cleaned according to "Clear cache" and wiped on uninstall instructions. Never persist media segments, license responses or key material.

## Tests
Chunk/range map logic, LRU eviction and byte caps, partial-entry resume, corruption recovery, ENOSPC handling, single-flight dedup, key stability across URL changes, XDG_CACHE_HOME handling, concurrent access, that no DRM track ever produces a cache entry (assert at the StreamSource layer), settings → budget wiring, and benchmarks for the memory tier hot path.
Sidecar exists before the first audio byte is stored; entry resolves title/artist/cover offline; cover failure and later retry; cover dedup by hash; tag round-trip including Cyrillic, CJK and emoji; audio payload unchanged after tagging; crash/ENOSPC during tagging leaves a valid entry; metadata refresh on updated_at change; no sidecar or tags ever created for DRM-flagged tracks.

# CUSTOMIZATION (settings UI + config file)
- Principle: ONE source of truth. A declarative settings schema (C++ registry: key, type, default, range/enum, category, label/description translation keys, "requires restart" flag, "advanced" flag, scheme-specific flag) drives (a) the Settings UI (rows are generated, not hand-written), (b) config file parsing and validation, (c) CLI/env overrides, (d) generated docs (docs/configuration.md produced from the schema by an xmake task), (e) defaults and "reset" actions. Adding a setting = one schema entry.
- SettingsStore (core/settings): typed, observable, thread-safe reads, change notifications to view-models, atomic writes (write temp + rename), debounced saves, schema versioning with migrations, unknown keys preserved on rewrite, invalid values rejected with a precise, translated UI message and a logged English message (file, line, key), never a crash. Secrets (OAuth tokens, proxy passwords) are NEVER stored in the config file.
- Config file: TOML (e.g. toml++), human-editable, commented defaults template generated on first run, located in the XDG config dir (platform-appropriate elsewhere). Hot reload: file watcher applies external edits live (validated; on error keep last good state and show a toast). Precedence: built-in defaults < system config < user config < environment variables < CLI flags. Support `--config <path>` and `--print-config` (effective config with sources, secrets redacted) and `--reset-config`.
- Example config shape (keys are stable English identifiers; the generated template documents every key):
    [network.dns]    mode = "system" | "custom" | "doh"; servers = [...]; doh_url = "..."; doh_bootstrap = [...]; fallback_to_system = true; ip_family = "auto"
    [network.proxy]  mode = "system" | "none" | "manual"; type = "socks5h"; host = "..."; port = 1080; bypass = [...]
    [cache]          audio_enabled = true; ram_mb = 128; disk_mb = 2048; dir = ""; prefetch = 2; clear_on_exit = false
- Settings UI: categorized pages (General, Appearance, Layout, Playback, Library/Content, Keyboard, Performance, Network, Storage & Cache, Accounts, Advanced, About), search box across all settings, per-setting "reset to default" and per-section reset, "Advanced" section hidden behind a toggle, import/export of the whole configuration (profile; secrets excluded) as a single file, and a "Open config file" button.
- What must be customizable (at minimum):
  * Appearance: theme mode, glass style and all glass parameters, accent mode/seed color, backdrop options, grain, corner-radius scale (sharp/default/round/custom multiplier), UI scale (zoom 75–200%), font families (heading/body/mono) and font size scale, icon weight, animation speed multiplier, reduce motion/transparency, cover-art shape (rounded/square/circle).
  * Layout: nav rail position (left/right/bottom) and compact/expanded/auto-hide mode, nav items visibility and order, density (compact/comfortable/spacious), now-playing bar position/size/what it shows (waveform, time, volume, queue button, etc.), grid vs list default per page, cards per row / card size, show/hide elements in track rows (plays, likes, duration, waveform mini, reposts), full-screen player layout options, window decorations (native / custom), start page, remember window geometry, start minimized/tray behavior.
  * Playback: audio quality preference, crossfade length, prefetch count, normalization (if feasible), default volume, behavior on end of queue, autoplay related.
  * Keyboard: fully remappable shortcuts (in-app and global media shortcuts where the platform allows), conflict detection, reset to defaults, shortcut cheat-sheet overlay.
  * Performance: everything from GPU ACCELERATION.
  * Network: everything from NETWORK. Storage & Cache: everything from CACHING.
  * Language and formatting: language, locale formatting overrides (time 12/24h, compact numbers).
- User themes: users can create theme files (TOML) in <config>/themes/ that override any subset of design tokens (colors for dark and/or light scheme, radii, spacing, fonts, durations, blur/liquid-glass parameters). Themes are selected by name in Settings → Appearance → "Custom theme", validated against the token schema (unknown token = warning, invalid value = ignored with message), hot-reloaded, shareable, with import/export. Ship 2–3 example themes in docs. SECURITY: user themes and config are DATA ONLY; the app must never load arbitrary QML/JS/shader code from user directories (no plugin execution via config).
- Customization must never break accessibility: when a user theme or accent produces contrast below AA, show a non-blocking warning in the UI (and optionally auto-correct if "Enforce readable contrast" setting is ON, default ON).
- Settings labels/descriptions are translated like every other string; config KEYS are stable English identifiers and are never translated.
- Tests: schema validation, migration, precedence order, atomic write, hot reload with bad edits, round-trip preservation of unknown keys, theme file validation, secret redaction, settings UI generated for every schema entry (no orphan or missing rows).

# LOCALIZATION (i18n / l10n)
- Supported languages at launch: English (source language) and Russian. Settings → General → "Language": [System default / English / Русский]. Default = System default (from QLocale::system()), with fallback to English. Language names are always displayed in their own language and are never translated.
- Switching language applies LIVE without restart (QTranslator install/removal + QQmlEngine::retranslate(); all bindings, accessible names, tray menu, notifications, MPRIS labels and window titles update). Persist the choice.
- Mechanism: Qt Linguist workflow. All user-visible strings use qsTr()/QT_TR_NOOP/tr() with disambiguation context where needed. NO hardcoded user-visible strings in QML or C++ (settings schema labels included). .ts files live in translations/ (app_en.ts, app_ru.ts), compiled to .qm by lrelease via an xmake rule and embedded into Qt resources. Adding a language requires only a .ts file and a registry entry.
- Plurals: Qt numerus forms (%n) with correct Russian categories (one / few / many / other); never concatenate sentence fragments; use positional placeholders (%1, %2). Tests cover Russian plurals for 0, 1, 2, 5, 11, 21, 22, 25, 101.
- Locale-aware formatting through QLocale for numbers (compact forms like "1.2K" / "1,2 тыс."), dates, relative times ("3 days ago" / "3 дня назад"), durations, byte sizes (cache usage), and sorting via QCollator. Time/duration display stays tabular and stable.
- Layout robustness: Russian is typically 20–30% longer than English; no fixed-width text containers, elide with tooltips where truncation is unavoidable, test with a pseudo-localization locale. Settings pages (especially Network and Cache with long hints) must handle long labels.
- Error handling: core/net/auth/media layers return typed error codes, never user-facing text; the UI maps codes to translated messages. Log messages stay in English.
- SoundCloud content (titles, descriptions, usernames) is shown as-is and never translated; mixed-script text (Cyrillic + Latin + emoji) must render correctly through the font fallback chain.
- Tooling and CI: xmake tasks `i18n-update` (lupdate) and `i18n-release` (lrelease); CI fails on unfinished/obsolete/missing ru translations or on source-string changes without .ts updates. Layouts RTL-friendly (anchors/LayoutMirroring) without implementing RTL now.
- Document the workflow in docs/localization.md.

# ARCHITECTURE (suggested; improve if justified, and explain why)
src/
  core/        logging, Result<T,E> (std::expected), error types, utils, threading helpers
  settings/    declarative schema, SettingsStore, TOML config I/O, hot reload, migrations, CLI/env overrides, user-theme loader
  net/         INetworkTransport (libcurl-based unless the spike decides otherwise), DNS layer (system/custom/DoH, cache), proxy resolution, TLS config, request queue, rate limiting, retry/backoff, diagnostics, SoundCloud API client
  auth/        cookie extraction from browsers, token storage (also proxy credentials), session management
  media/       playback engine abstraction + implementations, StreamSource (cache-aware byte source feeding the decoder)
  drm/         headless Electron host process manager + IPC bridge (incl. network config propagation)
  models/      QAbstractListModel-based models (lazy pagination, incremental fetch)
  cache/       tiered caches: AudioCache (RAM chunk store + disk store with range map and LRU), ImageCache, HttpCache; byte budgets, eviction, usage statistics
  theme/       AccentEngine, palette generation for dark+light, color science (OKLab/OKLCH), token tables, theme-mode manager
  gfx/         graphics bootstrap (GPU probe, RHI/software selection, safe mode, crash sentinel), glass rendering (shared backdrop pipeline, Dual Kawase / Gaussian / Liquid Glass / Solid + software fallbacks), quality governor, shaders (.glsl → .qsb)
  i18n/        LanguageManager (translator loading, live switching, locale formatting, error-code → message mapping)
  ui/          C++ view-models/controllers exposed to QML (incl. generated settings pages)
  qml/         QML components, pages, Theme singleton
  resources/   fonts, icons, shaders (compiled), images
translations/  .ts sources (en, ru) → .qm
electron/      minimal headless castlabs Electron app (see below)
tests/         unit tests (Qt Test or doctest) for core, settings, net (DNS/DoH/proxy), cache, auth, models, AccentEngine, IPC protocol, i18n, gfx bootstrap logic
bench/         benchmarks for hot paths

Principles:
- C++23 (fall back to C++20 only where compiler support is lacking). Use std::expected, std::span, std::string_view, concepts, ranges, std::jthread/stop_token, std::format where appropriate (std::format is NOT used for user-visible translated strings).
- RAII everywhere: no raw owning pointers, no manual new/delete; unique_ptr/QObject parent ownership consistently; wrap handles (sqlite3, processes, sockets, curl handles, mmap regions, file descriptors, GPU resources, file watchers) in RAII types.
- OOP where it makes sense (IPlaybackBackend, IBrowserCookieSource, IApiClient, INetworkTransport, IDnsResolver, IGlassRenderer, ISettingsSource); prefer composition over deep inheritance; avoid over-engineering and needless singletons. Dependency injection via constructors.
- const-correctness, [[nodiscard]], noexcept where correct, explicit constructors, rule of zero/five respected.
- Error handling via std::expected/typed errors at module boundaries; no silent failures; exceptions only for truly exceptional situations and never across Qt signal/slot boundaries.
- Clear naming, small functions, short files, minimal comments that explain "why", not "what". Consistent .clang-format and .clang-tidy committed; code must pass clang-tidy with a curated check set.

# SOUNDCLOUD API
- Use the web API that the SoundCloud site itself uses (api-v2) with the user's own session authorization (OAuth token from cookies). Handle client_id acquisition robustly and isolate it so breakage is easy to fix.
- Fully async on top of INetworkTransport (no blocking calls on the UI thread). Cancellation support (stop_token / QPointer guards).
- Connection reuse (HTTP/2 where available), request deduplication, coalescing, cursor pagination (next_href), exponential backoff, 401/403/429 handling with re-auth prompt. DNS, proxy, TLS, timeouts and limits come from the NETWORK settings, not from ad-hoc code.
- JSON parsing: fast and allocation-conscious (QJsonDocument only if profiling shows it is fine; otherwise simdjson behind a thin interface). Parse into compact domain structs; do not keep JSON trees alive.
- Normal (non-DRM) tracks: resolve the stream URL via API (progressive/HLS transcodings), play natively through StreamSource + AudioCache.

# PLAYBACK
- IPlaybackBackend with two implementations:
  1. NativeBackend: regular tracks (Qt Multimedia or FFmpeg-based pipeline; choose the better-performing option and justify it). The decoder reads from StreamSource (custom QIODevice / AVIOContext), which is backed by AudioCache → INetworkTransport, so cache, DNS and proxy settings always apply. HLS/progressive, buffering, seek (cache-aware), prefetch of the next queue item, cached-range display in the waveform.
  2. DrmBackend: DRM-protected tracks, delegated to the headless Electron process (never cached by the client, see CACHING).
- PlaybackController picks the backend per track, exposes unified state (position, duration, buffering, cached ranges, volume, errors) to QML, handles seamless switching.
- Media keys + MPRIS2 over D-Bus (Linux), system tray controls, optional desktop notifications.

# DRM TRACKS VIA HEADLESS CASTLABS ELECTRON
- Use castlabs Electron for Content Security (with Widevine CDM) as a minimal background helper with NO visible window, strictly minimal: no devtools, no unnecessary Chromium features, GPU/compositing disabled if audio-only allows, background throttling configured so audio is not suspended, tiny preload script, no remote content except what the SoundCloud player/EME flow requires.
- Playback of protected tracks happens INSIDE Electron via the standard EME/Widevine path, authorized with the user's own account. The C++ client controls it (load, play/pause/seek/volume; receive position, duration, ended, error). The C++ side must NOT extract, record, intercept, cache or persist decrypted or encrypted media, keys or license data, and the project must not offer any feature to save DRM-protected content.
- The helper honors the app's proxy and DNS settings (see NETWORK) and uses no persistent media/HTTP cache (see CACHING).
- IPC: JSON messages over a local channel (QLocalSocket/Unix domain socket or stdio), versioned and schema-validated, with heartbeat and clean shutdown. Define the protocol in one place (shared header + JS module). Includes a message for network configuration updates.
- DrmHost class (RAII): lazily spawns Electron only on the first DRM track, restarts on crash with backoff, shuts down after an idle timeout to save RAM, kills the process tree on app exit (no orphans).
- Document build/packaging (castlabs release download, signing/VMP notes) in docs/drm-helper.md.

# AUTH: AUTOMATIC COOKIE / OAUTH TOKEN EXTRACTION
- Goal: sign-in without typing credentials by reading the SoundCloud session (oauth_token cookie) from the user's OWN browser profiles on their own machine, with explicit consent in the UI (show which browser/profile, ask before reading).
- IBrowserCookieSource implementations: Chromium family (Chrome, Chromium, Brave, Edge, Vivaldi, Opera), Firefox family (Firefox, LibreWolf, etc.). Auto-detect installed browsers and profiles (including Flatpak/Snap paths on Linux).
- Read cookie DBs via SQLite opened read-only on a COPY of the file (the browser may hold a lock); handle Chromium cookie decryption via the OS secret store (libsecret/KWallet on Linux; interface designed so Windows DPAPI / macOS Keychain can be added later). Fail gracefully with clear, translated user-facing messages.
- Extract only what is needed (oauth token and required session cookies); never log secrets; zero buffers where practical.
- Store the token (and proxy credentials) securely (libsecret/Keychain-style storage; fallback encrypted file with a clear warning), never in the plain config file. Provide "log out / forget credentials" and a manual-token paste fallback.
- Unit-test cookie parsing/decryption with fixture databases.

# PERFORMANCE & OPTIMIZATION (first-class)
- Threading: UI thread does only UI; network, DNS, JSON parsing, image decoding, accent extraction, backdrop pre-blur, config I/O and ALL cache I/O run off-thread; marshal results back with queued connections. No blocking I/O on the UI thread.
- Memory: no leaks (ASan/LSan/Valgrind), std::pmr/arenas for hot parse and chunk-buffer paths if profiling justifies, reserve() containers, avoid needless allocations/copies, avoid QString<->std::string churn.
- Startup: lazy-init heavy subsystems (DRM host, FFmpeg/codecs, tray, MPRIS, glass pipeline variants not in use, inactive translations, settings pages not yet opened, cache indexes), restore the last session UI from cache instantly, refresh in the background. The GPU probe must be fast and cached between launches (invalidated on driver/Qt version change).
- Idle: no needless timers; event-driven everything; Electron helper shut down when unused; zero GPU work when the scene is static.
- Lightweight compile-time-switchable profiling/trace layer and benchmarks for hot paths (JSON parsing, model updates, cache memory tier, DNS cache lookups, AccentEngine, settings load, software-render path).
- Document measured numbers (startup time, RSS idle, CPU idle/playing, GPU frame time for each glass mode in both themes, software-mode CPU usage, cache hit latency, time-to-first-audio cold vs cached) in docs/performance.md and keep a regression checklist.

# BUILD SYSTEM: XMAKE
- xmake (xmake.lua), modular targets (static libs per module + executable + tests + bench), packages via xmake/xrepo where possible (e.g. toml++, simdjson, doctest, curl with the required features); Qt 6 via xmake's qt rules; custom rules/tasks to compile GLSL → .qsb with qsb, .ts → .qm with lrelease, and to generate docs/configuration.md from the settings schema; all embedded/produced at build time.
- Toolchain: clang++, linker mold (-fuse-ld=mold); configure via set_toolchains / add_ldflags properly and VERIFY it actually works.
- Modes:
  release:
    - -O3 (do NOT enable -ffast-math unless audio/DSP code is proven unaffected; color/blur math must stay deterministic)
    - -flto=thin (or full LTO if measurably better), -fvisibility=hidden, -fvisibility-inlines-hidden
    - -fno-plt, -fno-semantic-interposition, -fomit-frame-pointer, -funroll-loops only if it benchmarks better
    - Target CPU option (default -march=x86-64-v2/v3 for distribution, -march=native for local builds)
    - Link flags: -Wl,--gc-sections, -Wl,-O2, -Wl,--as-needed, -Wl,-z,relro,-z,now, split debug info
    - Hardening: -fstack-protector-strong, -D_FORTIFY_SOURCE=2 (or 3 where supported), -fPIE/-pie, -fcf-protection
    - Optional PGO workflow (instrument → run scenario → merge profdata → rebuild) as separate xmake tasks, documented
  debug:
    - -O0/-Og, -g3, ASan + UBSan (separate TSan configuration), -fno-omit-frame-pointer
  Strict diagnostics in ALL modes:
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align -Wnull-dereference -Wdouble-promotion
    -Wformat=2 -Wimplicit-fallthrough -Wunused -Wuseless-cast -Wmisleading-indentation
    -Wdeprecated -Wextra-semi -Wzero-as-null-pointer-constant -Wthread-safety
    and -Werror in CI/release (optional switch locally). Qt/third-party headers included as system headers; suppress warnings only locally and with a comment. Shaders are validated at build time (qsb must succeed with no warnings).
- Tasks: build, test, run, format (clang-format), lint (clang-tidy), bench, package (AppImage or tarball + desktop file + icons), pgo, shaders, i18n-update, i18n-release, docs-config.
- Generate compile_commands.json.

# TESTING & QUALITY
- Unit tests: core, settings (see CUSTOMIZATION), net (see NETWORK tests; recorded fixtures / mock servers), cache (see CACHING tests), auth, models, IPC protocol parsing, AccentEngine (determinism, contrast guarantees in dark AND light schemes, edge-case images), i18n (switching, plurals, locale formatting, 100% key coverage for ru), gfx bootstrap decision logic (GPU probe results → mode, blocklist, safe-mode/crash-sentinel behavior) with mocked probes.
- Integration smoke test of Electron IPC with a mock helper (including network-config updates).
- Integration test: play a track through StreamSource against a mock server twice, verify the second playback and a backward seek are served from cache with zero network requests, and that a DRM-flagged track never writes to the audio cache.
- Smoke test that launches the app headlessly in software mode (offscreen platform) and verifies all pages instantiate without GPU-only components failing.
- Visual regression (optional but preferred): headless render of key components for the matrix {Midnight, Frost} × {Dual Kawase, Gaussian, Liquid Glass, Solid, software mode} × {en, ru, pseudo-loc}, compared against reference screenshots with a tolerance.
- clang-tidy + clang-format enforced via tasks; CI (GitHub Actions) builds release and debug+sanitizers, runs tests, the i18n completeness check, and the settings docs-sync check (generated docs must match the schema).

# DELIVERABLES
1. docs/architecture.md with module diagram and data flow (shared backdrop/glass pipeline, settings flow, i18n flow, graphics bootstrap/fallback flow, network transport → cache → StreamSource → decoder flow).
2. Complete, buildable source tree.
3. xmake.lua files with the toolchain/flags above, README with build/run instructions and dependency list.
4. Electron helper (electron/), docs/ipc.md, docs/drm-helper.md.
5. docs/design.md: tokens (dark + light), components, palette generation, glass modes, fonts and licensing.
6. docs/configuration.md (generated from the schema), docs/themes.md (custom theme files + examples).
7. docs/gpu.md: acceleration modes, fallbacks, safe mode.
8. docs/network.md: transport decision (with spike results), DNS/DoH/proxy behavior, DNS↔proxy interaction, helper limitations, diagnostics.
9. docs/caching.md: tiers, layout under the cache directory, keys, eviction, what is and is not cached (incl. the DRM policy).
10. docs/localization.md: translation workflow, adding a language, plural rules.
11. docs/performance.md with measurements.

# WORKING METHOD
1. First, write a concise plan, list assumptions and open questions (ask only about blockers; otherwise decide and document).
2. Vertical slices: (a) build system (incl. qsb, lrelease and docs-config rules) + skeleton + settings schema/SettingsStore/TOML config + graphics bootstrap (GPU probe, software fallback, safe mode) + Theme tokens for BOTH Midnight and Frost + fonts + LanguageManager with en/ru → (b) glass pipeline (shared backdrop, Dual Kawase first, then Gaussian, then Liquid Glass, Solid and software fallbacks, governor) + AccentEngine for both schemes + Appearance/Performance settings UI → (c) network transport spike + INetworkTransport + DNS (system/custom/DoH) + proxy + Network settings page + diagnostics → (d) cache layer (image/HTTP first, then audio RAM + disk tiers) + Storage & Cache settings page → (e) SoundCloud API client + models → (f) auth/cookie extraction → (g) native playback via StreamSource + AudioCache → (h) UI pages + Layout/Keyboard customization + user themes → (i) DRM helper (incl. network-config propagation and no-cache policy) → (j) polish/performance pass. Every slice adds its strings, settings schema entries and light-theme values from the start; do not defer localization, theming, the software path or settings to the end.
3. After each slice: build with zero warnings, run tests and sanitizers, commit with a clear message.
4. Do not invent APIs or library functions; check headers/docs. If something is uncertain (current SoundCloud endpoint behavior, Qt version availability of QQuickRhiItem, software-backend limitations, libcurl feature flags in the xrepo build, Electron's secure-DNS support in the castlabs version), isolate it behind an interface, verify it, and flag it in notes.
5. Prefer simple, readable solutions; justify non-obvious complexity with a measurement or a clear reason.

# DEFINITION OF DONE
- Builds cleanly with clang++/mold under xmake in release and debug with zero warnings.
- Tests and sanitizers pass; no leaks or data races.
- Both themes (Midnight/Frost) and System-follow work with live crossfade; every component looks correct and meets contrast AA in both.
- All glass modes work and switch live; adaptive accent ON by default and fully replaceable by a static accent seed; Auto quality degrades gracefully.
- Hardware acceleration Auto/On/Off works (restart prompt where required); the app is fully usable in software mode and in Safe Mode; GPU init failures and device loss never crash the app.
- Settings are driven by one schema: every setting is available in the UI and in the TOML config (hot-reloaded, validated, documented), custom theme files work, config precedence is correct, nothing sensitive is stored in the config.
- Network: DNS (system/custom/DoH with strict mode and fallback) and proxy (system/manual, HTTP/HTTPS/SOCKS5/5h, auth, bypass) work and apply live to ALL traffic including the Electron helper; test buttons and diagnostics work; no DNS leaks in strict mode or behind a remote-DNS proxy.
- Caching: regular tracks are cached in RAM and on disk under the standard user cache directory (~/.cache/<app-id>/ on Linux, XDG_CACHE_HOME respected) with configurable budgets, correct eviction and instant replay/seek; DRM tracks are never cached by the client; cache I/O never blocks the UI or playback.
- The entire UI is available in English and Russian, switchable live, with correct plurals and locale formatting and no hardcoded strings; CI i18n and docs-sync checks pass.
- App starts quickly, scrolls large lists smoothly, plays regular tracks natively and protected tracks via the Electron helper, with no orphaned processes after exit.
- Measured numbers are documented. Code passes clang-tidy and clang-format and follows the principles above.
