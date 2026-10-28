# Architecture

This document grows with each slice. It currently covers slices (a) to (d).

```
            ┌──────────────── app/main.cpp (composition root) ────────────────┐
            │ CLI parse → SettingsStore layers → graphics bootstrap → QML     │
            └──────┬───────────────┬───────────────┬──────────────┬───────────┘
                   │               │               │              │
   core ◄── settings ◄── i18n      theme           gfx            ui ──► qml/
 (errors,  (schema,     (Language- (OKLCH,        (policy,       (SettingsModel/
  logging,  store,       Manager,   Palette,       GpuProbe,      Controller,
  atomic    TOML,        Formatter, Theme,         CrashSentinel, AppController,
  file I/O) ConfigFile,  Error-     FontLoader)    Relaunch)      IconProvider)
            overrides)   Messages)
```

Slice (c) adds two modules: `net` (depends on `core` and `settings`: transport, DNS, proxy, diagnostics) and `auth`
(depends on `core`: the secret store for now). Slice (d) adds `cache` (depends on `core`, `settings` and `net`: the
audio RAM and disk tiers, the image cache, the HTTP cache). `ui` depends on all of them and `app` wires them up.

Static libraries per module (`noctara_<module>`); the executable only wires them together. Dependencies point downward only: `ui` may use everything, and `core` uses nothing but Qt Core.

## Settings flow

```
BuiltinSchema.cpp ──► Schema ──┬──► SettingsStore layers: Default < System < User < Env < CLI
                               │        ▲            ▲                 ▲      ▲
                               │   ConfigFile (TOML, hot reload,       │      │
                               │   debounced atomic save on an I/O     │      │
                               │   thread, unknown keys preserved)     │      │
                               │                         collectEnvironment  collectCommandLine
                               ├──► SettingsController/SettingsModel ──► generated QML rows
                               ├──► schemaMarkdown ──► docs/configuration.md (xmake docs-config)
                               └──► --print-config (effective values with sources, redacted)
```

UI edits go through `SettingsStore::setUserValue` (validated; refused when pinned by env/CLI) → `userLayerEdited` → `ConfigFile` saves after 400 ms. External edits come in through the file watcher, are parsed off-thread and applied atomically. A syntax error keeps the whole last good state; an invalid key keeps that key's previous value; both show a toast. Each change notifies listeners (`valueChanged`), so the theme, language and other consumers react live.

## Graphics bootstrap flow

See [gpu.md](gpu.md). The order in `main()` is QGuiApplication → settings → `bootstrapGraphics` (sentinel, cached probe, decision, `setGraphicsApi`) → theme/i18n → QML → `sceneGraphError` handler → disarm the sentinel on the first frame.

## i18n flow

The setting change goes to `LanguageManager`, which swaps translators and sets `QLocale::setDefault`, then emits `languageChanged`. `QQmlEngine::retranslate()` re-evaluates `qsTr` bindings, C++ models emit `dataChanged`, and `Formatter` bumps its revision.

## Theme flow

Settings, `QStyleHints::colorScheme` and the artwork seed feed `generatePalette()`, which is pure and enforces contrast. `Theme` then crossfades in OKLab and emits `Theme.colors` (one gadget). The glass style is resolved from the requested style, GPU availability, Reduce transparency and the governor cap.

## Build notes

- `noctara.moc` replaces xmake's stock moc handling for our headers. The stock rule only tracked the moc'd header, so changing a header it *includes* left stale meta-object code behind. That caused a real crash during development, when a gadget gained members. Our rule adds the header's transitive project includes as dependencies.
- `.ts` files and moc'd headers use `override = true` rules, so the stock `qt.ts` and `qt.moc` rules don't process them a second time. A build with no changes compiles nothing.

## Glass and effects flow

```
Settings (glass style, strength, quality, energy saving, frame cap) ──┐
PowerMonitor (UPower: on battery?) ───────────────────────────────────┤
FrameMonitor (render thread: QRhi GPU timestamps, frame intervals;    │
              GUI thread: one summary per second of rendering) ──► QualityGovernor (pure)
                                                                      │
                                         gfx::resolveEffects (pure) ◄─┘
                                                   │  quality, style cap, backdrop animation, frame cap
                     ┌─────────────────────────────┴─────────────────────┐
                     ▼                                                   ▼
        Theme.glass (style after fallbacks, blurPlan)          FrameMonitor pacing (render thread)
                     │
       Main.qml: Backdrop ──► BackdropBlur (one capture + Kawase/Gaussian chain) ──► GlassPanel shaders

Artwork ──► AccentEngine (thread pool: k-means seed + tone-mapped blurred backdrops, LRU)
              ├─ seedChanged ──► Theme (palette crossfade)
              └─ image://artwork/<scheme>/<rev> ──► Backdrop (cover style)
```

`ui::EffectsController` (QML `Performance`) owns the governor, wires the inputs, applies the result, and writes the translated status lines of the *Glass style* and *Effects quality* rows.

## Network flow

```
settings (network.*) ──► NetworkService (GUI thread) ── config snapshot + proxy credentials (from auth::ISecretStore)
                                 │ posts to
                                 ▼
          network thread: NetworkCore
            ProxyRouter ── decides direct / which proxy (manual, env vars, libproxy)
            DnsResolver ── overrides → cache → system | custom servers (UDP/TCP) | DoH ─┐
            CurlTransport ◄── every host pinned via CURLOPT_RESOLVE; resolver guard     │
                 ▲   └──── DoH POSTs (direct, pinned to bootstrap addresses) ◄─────────┘
                 │
any module ── INetworkTransport::send(request, context, callbacks) ── results on context's thread
```

The caches (d) call `send()`; the API client (e) will use the caching transport below. See [network.md](network.md)
for details.

## Transport → cache → StreamSource → decoder

```
                         settings (cache.*) ──► CacheService (GUI thread) ── budgets, location, lock
                                                      │ owns
                                                      ▼
            cache thread:  CachingTransport (HTTP cache, <cache>/http) ──► API client (e)
                           ImageCache (RAM LRU + <cache>/images) ◄── image://cover/… (QML)
                           AudioCache (disk tier, <cache>/audio) ── one fetch per entry, Range requests
                               │  stores pieces                  └─► INetworkTransport (net thread)
                               ▼
                           MemoryTier (RAM tier, thread-safe, shared immutable pieces)
                               │
  decoder thread (g):  StreamSource ── AudioReader::read(piece) ── RAM → disk (pread + checksum) → network
```

DRM tracks never enter this path: `AudioCache::open` refuses them before anything is created, and the DRM backend
(slice i) plays them in the Electron helper. See [caching.md](caching.md).

## Ownership and threading

- Every object is owned by RAII or a QObject parent. Objects handed to QML from invokables are explicitly set to `CppOwnership`; ASan caught the case where they weren't.
- Config I/O runs on a dedicated single-thread pool, so writes stay in order. Settings reads are thread-safe (`shared_mutex`). Mutations happen on the GUI thread.
- Icons are rasterized on Qt Quick's async image threads.
- `FrameMonitor` is declared before the QML engine in `main()`, so it outlives the window's render thread, which calls into it directly. Shared samples are guarded by a mutex, and the frame cap is an atomic.
- The network stack lives on its own thread and is created and destroyed there (`NetworkService` blocks briefly on
  both), so its timers and socket notifiers belong to that thread. Callbacks reach callers through
  `CallbackChannel` (a queued signal), which Qt drops when the receiver is gone. Asynchronous continuations inside
  the thread look objects up by id or `QPointer` instead of keeping raw pointers.
- The caches live on their own thread (`CacheService`, created and destroyed there like the network stack). Audio
  readers are thread-safe: the decoder reads pieces (RAM hit, else `pread`) on its own thread and waits on the
  entry's condition variable for missing ones; everything that writes runs on the cache thread. Tagging and image
  decoding run on the thread pool and post their results back through a `Mailbox`, which drops them if the cache is
  gone.
- `AccentEngine` workers capture only copies of their inputs. Results carry a generation number, and stale ones are dropped. The backdrop images it hands to the image provider (loader threads) are guarded by a mutex.
