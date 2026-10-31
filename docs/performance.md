# Performance notes and measurements

Measurements are collected as slices land. Dev machine: 4 cores, 15 GB RAM, NVIDIA GTX 1070 (driver 580.196), Linux 7.2, Qt 6.12. Numbers marked *(dev)* come from this machine only. Mainstream-iGPU numbers are still **pending**: they need other hardware.

## Slice (a)

| Metric | Value | Notes |
|---|---|---|
| GPU probe, OpenGL (uncached) | ~0.9 s *(dev)* | one-time; cached in `gpu-probe.json` and invalidated by a driver fingerprint |
| GPU probe (cached) | ~0 ms | file read + stat calls only |
| Palette generation | < 1 ms | pure C++ with contrast enforcement; also covered by the 3.4k-assertion test matrix |

## Slice (b)

GPU times come from QRhi timestamps (`QT_LOGGING_RULES="noctara.gfx.debug=true"`). The setup is Vulkan, a ~950×1020 window on a 175 Hz display, Midnight, the aurora drifting, quality High, and a release build.

| Glass style | GPU time per frame (p90) | Glass cost vs Solid |
|---|---|---|
| Solid | 0.18 ms | — |
| Gaussian | 0.25 ms | +0.07 ms |
| Liquid Glass | 0.29 ms | +0.11 ms |
| Dual Kawase | 0.30 ms | +0.12 ms |

| Metric | Value | Notes |
|---|---|---|
| Idle CPU, static scene | 0.2% | release; nothing is redrawn (0 frames per second) |
| Idle CPU, aurora drifting | 3.1% | 12 Hz drift clock, about 24 frames/s (each drift step also refreshes the blur layer chain). Before: about 13% at the 175 Hz display rate with declarative animations |
| Home page with placeholders | static | skeleton shimmer now runs only while something loads; it used to redraw at 175 fps forever |
| AccentEngine (600 px artwork) | ~3.9 ms | worker thread; k-means plus two tone-mapped backdrops |
| RSS after startup | ~180 MB | release, Vulkan, NVIDIA (mostly driver allocations) |

Frost costs the same as Midnight: the pipeline and shaders are identical, and only uniform values differ. The ≤ 1 ms budget for blur plus glass on a mainstream iGPU at 1440p is still **unverified**, because it needs other hardware. On the GTX 1070, glass costs about 0.1 ms at roughly 1 MP.

Cold start, idle RSS and CPU, and software-mode CPU are measured in slice (j) below. Glass GPU cost per mode is the
table above; the themes cost the same.

## Slice (c): network

Release build, loopback mock servers (transport overhead only; real latency is the network's). Reproduce with
`noctara-tests --no-skip -tc="transport spike*"`.

| Measurement | Result |
|---|---|
| Sequential keep-alive GET, 1 KiB body | ≈ 0.30 ms per request |
| 200 concurrent GETs | 47 ms in total |
| Cached DNS lookup | 2.4 µs |
| Uncached DoH lookup, A + AAAA in parallel | 0.47 ms |

Idle cost: none. The network thread sleeps in its event loop; curl's timer runs only while transfers exist, the
speed-limit refill timer only while a transfer is paused, and NetworkManager state arrives as D-Bus signals.

## Slice (d): caches

Release build. Reproduce with `noctara-tests --no-skip -tc="cache benchmark*"`.

| Measurement | Result |
|---|---|
| RAM tier hit (64 KiB piece, 2048 pieces resident) | ≈ 40 ns, no copy (shared buffer) |
| RAM tier insert / insert with eviction | ≈ 80 ns / ≈ 80 ns |
| Disk hit: `pread` 64 KiB + XXH3 checksum (page cache) | p50 ≈ 9 µs, p99 ≈ 25 µs |
| Second playback and seeks of a cached track | 0 network requests (integration test) |

`std::pmr` arenas for piece buffers were not added: a piece is one 64 KiB allocation per network chunk, which is
noise next to the I/O, and hits share the buffer without copying.

Idle cost: none. The cache thread sleeps; the index flush timer runs only after a change, memory pressure arrives
as a kernel PSI event, and the Storage page re-reads usage at most once a second while the caches change.

## Slice (e): SoundCloud API

Release build. Reproduce with `noctara-tests --no-skip -tc="api parse benchmark"`.

| Measurement (50-track search page, 80 KB of JSON) | Result |
|---|---|
| simdjson DOM → `Page<Entry>` of compact structs | ≈ 180 µs (≈ 445 MB/s) |
| `QJsonDocument::fromJson`, tree only, no structs | ≈ 620 µs |
| First version, with every URL as a `QUrl` | ≈ 410 µs |

Without a profiler on the dev machine, the cost was found by elimination. Building `QUrl`s (7 per track) was 60 %
of the total, so the structs keep URLs as text and requests build `QUrl`s when they need them. A fast path for
SoundCloud's fixed ISO-8601 timestamps skips `QDateTime::fromString`; it made no measurable difference, but it is
simpler than the general parser. Parsing runs on the API thread, never on the GUI thread. Each thread keeps one
simdjson parser, so its buffers are reused. Bodies with enough spare capacity are parsed in place; anything else
is copied once.

Idle cost: none. The API thread sleeps, and the only timers are the 10 ms lookup-coalescing window and the pause
after a 429, both started only on demand.

## Slice (g): playback

Release build on the dev machine (GTX 1070, Vulkan, Hyprland), a 150-second track that SoundCloud serves as HLS
AAC 160 kbit/s (fMP4), measured from the moment the backend starts loading to the first audio frame. Reproduce with
`--play <link>` and `QT_LOGGING_RULES="noctara.media.debug=true"` (the backend logs its state changes); CPU over
10 s of playback from `/proc/<pid>/stat`.

| Measurement | Result |
|---|---|
| Time to first audio, cold (stream URL, HLS playlist, first segment; includes opening the audio device) | 1.2–1.8 s |
| Time to first audio, cached (disk tier, new process) | ≈ 270 ms |
| CPU while playing, GPU renderer, backdrop drift off | ≈ 3 % of one core |
| CPU while playing, software renderer | ≈ 2.7 % |
| CPU idle, GPU renderer, backdrop drift off / on (default) | 0.2 % / 6.7 % |
| RSS while playing (GPU / software) | ≈ 285 / 205 MiB |

Most of the playing cost is repainting the window for position updates, not decoding. Announcing the position 10
times a second cost ≈ 4.8 %; the controller now announces it 4 times a second (the waveform still moves by less
than a pixel per update on tracks of a few minutes), which brought it to ≈ 3 %. Decoding AAC and resampling to the
device rate is well under 1 %. The cold start is dominated by network round trips (resolve the stream URL, fetch the
playlist, fetch the first segment); about 0.35 s of it is opening Qt Multimedia's audio backend for the first time.

The idle number with the default *animated backdrop* (slice b) is far from "≈ 0": the slow drift redraws the window
continuously while it is visible and focused (it pauses when unfocused, minimized or on battery saver). Turning the
drift off gives 0.2 %. Whether the drift should stay on by default is an open question for the polish slice (j).

Idle cost of playback when paused: none. The backend suspends the device once the fade-out has played, and its
100 ms check stops; the decoder threads wait on a condition variable.

## Slice (j): benchmarks, tracing, whole-app numbers

### Hot paths: `noctara-bench`

`xmake f -m release && xmake build noctara-bench`, then `build/linux/x86_64/release/noctara-bench [filter]`. Each
benchmark runs a warm-up and seven batches; the median per operation is printed. Release build, dev machine:

| Benchmark | Median |
|---|---|
| API: parse a 50-track page (simdjson → structs) | 172 µs |
| Models: replace 500 rows and read every role once (what a list view does on first show) | 0.99 ms |
| Cache: RAM tier hit (64 KiB piece) / insert with eviction | 37 ns / 71 ns |
| Net: DNS cache hit (1000 names cached) | 83 ns |
| Theme: palette generation, dark + light (contrast enforcement included) | 0.60 ms |
| Accent: analyse a 600 px cover (k-means, worker thread) | 0.95 ms |
| Software path: CPU-blurred 256 px cover backdrop | 1.55 ms |
| Settings: parse and validate the complete documented config file | 0.63 ms |
| Protected player: decode and validate one helper message (4 per second while playing) | 1.5 µs |

The skipped `noctara-tests` cases `api parse benchmark`, `cache benchmark` (disk-tier hit latency) and
`transport spike measurements` remain for the comparisons that need the test fixtures and mock servers.

### Tracing

`xmake f --trace=y` compiles in `core/Trace.hpp`: `NOCTARA_TRACE_SCOPE("name")` and `NOCTARA_TRACE_INSTANT("name")`
record into a mutex-protected buffer, written at exit as Chrome trace JSON (`$NOCTARA_TRACE_FILE`, default
`noctara-trace.json`; open in Perfetto or `chrome://tracing`). Without the option the macros compile to nothing.
Instrumented: the graphics bootstrap, loading `Main.qml`, the first frame, API page parsing, artwork analysis and
the blurred backdrop, opening a decoder, and the protected player's start and handshake. The first frame is also
logged without tracing (`noctara.gfx`: "first frame N ms after start").

### The whole app

Release build, offscreen platform (OpenGL on the GTX 1070, or the software renderer), signed in, network up, a fresh
isolated profile per run, the Home page; CPU averaged over 10 s after 30 s of settling (`/proc/<pid>/stat`), two runs
each.

| Configuration | First frame after start | Idle RSS | Idle CPU |
|---|---|---|---|
| GPU, backdrop drift off | 184–200 ms | 167 MiB | 0 % (no frames) |
| GPU, backdrop drift on (default) | 197–252 ms | 167 MiB | 16.5–17 % at 12–13 frames/s |
| Software, drift off | 252 ms | 166 MiB | 0 % |
| Software, drift on | 322 ms | 166 MiB | 0 % (the software path never drifts) |

The offscreen platform renders without vsync and reads every frame back, so the drift figure is higher than on a
real display (6.7 % on Wayland in slice g); the static scene costs nothing either way. The drift's cost is the open
item for the optimisation pass (see the slice g notes). Time to first audio is in the slice (g) table.

Protected player (castlabs Electron 44.9.3, `--disable-gpu`, idle after `ready`):

| Measurement | Result |
|---|---|
| First start on a fresh profile (includes downloading the 21 MB Widevine CDM) | 2.6–3.9 s to `ready` |
| Later starts | 0.45–0.63 s to `ready` |
| Memory of its process group (8 processes), PSS | 210–237 MiB |
| Idle cost when not playing | none: it starts on the first protected track and closes after 5 minutes without one |

## Design choices that affect cost

- Colors cross-fade by updating one gadget per frame for 400–500 ms. Icons do not re-rasterize per frame: they bind to `targetColors` and crossfade two cached images.
- Pages and settings categories are created on first visit and kept alive afterwards.
- Config writes are debounced (400 ms) and happen off the GUI thread.

## Sanitizers and the regression checklist

- Debug builds run ASan + UBSan. LSan suppressions (`src/core/LsanSuppressions.hpp`) cover third-party GPU-driver allocations only; our code is never suppressed.
- TSan (`xmake test-tsan`): the distro Qt isn't instrumented, so Qt-internal synchronization is suppressed (`tools/tsan.supp`). TSan's thread registry aborts (`CHECK live_.try_emplace`) on thread-ID reuse once a process has started and stopped many threads, so `test-tsan` runs every suite in its own process, and the suites that start and stop `QThread`s or QML loader threads repeatedly (`settings.file`, `ui.settings`, `ui.presets`, `ui.icons`, `cache.service`) are excluded. The audio cache's multi-threaded readers, tagging and image decoding run under TSan (`cache.audio`, `cache.image`, `cache.integration`). Cross-thread hand-overs in `net` go through `core/Handoff.hpp`, which in TSan builds makes Qt's event-queue ordering visible with one extra mutex, so the network suites run without suppressions for our own code. **Known gap:** full TSan coverage needs a TSan-instrumented Qt build. Under TSan threads occasionally stall for several seconds (seen in the theme library's folder scan, normally under 1 ms, and in cache tagging) without any race being reported, and every test process also spends seconds in the NVIDIA GL driver's start-up; `test::waitUntil` therefore allows 10× its timeouts in TSan builds.
- Before each commit: `xmake format --check`, a debug build with `--werror=y`, unit tests, `xmake smoke`, `xmake i18n-check`, `xmake docs-config --check`, `xmake lint`.

### Regression checklist (before a release, or after touching a hot path)

1. `noctara-bench` in release: no row more than 20 % above the table above.
2. First frame (`noctara.gfx` "first frame") under 0.5 s with a warm GPU-probe cache; idle RSS within 10 % of 167 MiB.
3. Static scene (drift off, nothing loading): 0 frames per second and 0 % CPU (`QT_LOGGING_RULES="noctara.gfx.debug=true"`
   prints frames per second).
4. Playing a regular track: CPU in line with the slice (g) table; a second play and a backward seek make no requests.
5. Protected track: the helper starts only for it, closes after the idle timeout, leaves no process after exit and no
   media, cache entries or dictionaries in `<cache>/electron`.
6. Glass GPU times per mode (slice b table) with `noctara.gfx.debug`, both themes, on a real display.
7. Sanitizers: debug (ASan/UBSan) tests and `xmake test-tsan` clean.
