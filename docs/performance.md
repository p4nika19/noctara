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

Not yet measured (scheduled for slice j): cold start to first frame, idle RSS, idle CPU, glass GPU cost per mode and theme, software-mode CPU.

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

## Design choices that affect cost

- Colors cross-fade by updating one gadget per frame for 400–500 ms. Icons do not re-rasterize per frame: they bind to `targetColors` and crossfade two cached images.
- Pages and settings categories are created on first visit and kept alive afterwards.
- Config writes are debounced (400 ms) and happen off the GUI thread.

## Sanitizers and the regression checklist

- Debug builds run ASan + UBSan. LSan suppressions (`src/core/LsanSuppressions.hpp`) cover third-party GPU-driver allocations only; our code is never suppressed.
- TSan (`xmake test-tsan`): the distro Qt isn't instrumented, so Qt-internal synchronization is suppressed (`tools/tsan.supp`). TSan's thread registry aborts (`CHECK live_.try_emplace`) on thread-ID reuse once a process has started and stopped many threads, so `test-tsan` runs every suite in its own process, and the suites that start and stop `QThread`s or QML loader threads repeatedly (`settings.file`, `ui.settings`, `ui.presets`, `ui.icons`, `cache.service`) are excluded. The audio cache's multi-threaded readers, tagging and image decoding run under TSan (`cache.audio`, `cache.image`, `cache.integration`). Cross-thread hand-overs in `net` go through `core/Handoff.hpp`, which in TSan builds makes Qt's event-queue ordering visible with one extra mutex, so the network suites run without suppressions for our own code. **Known gap:** full TSan coverage needs a TSan-instrumented Qt build. Under TSan threads occasionally stall for several seconds (seen in the theme library's folder scan, normally under 1 ms, and in cache tagging) without any race being reported, and every test process also spends seconds in the NVIDIA GL driver's start-up; `test::waitUntil` therefore allows 10× its timeouts in TSan builds.
- Before each commit: `xmake format --check`, a debug build with `--werror=y`, unit tests, `xmake smoke`, `xmake i18n-check`, `xmake docs-config --check`, `xmake lint`.
