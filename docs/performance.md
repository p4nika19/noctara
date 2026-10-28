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

## Design choices that affect cost

- Colors cross-fade by updating one gadget per frame for 400–500 ms. Icons do not re-rasterize per frame: they bind to `targetColors` and crossfade two cached images.
- Pages and settings categories are created on first visit and kept alive afterwards.
- Config writes are debounced (400 ms) and happen off the GUI thread.

## Sanitizers and the regression checklist

- Debug builds run ASan + UBSan. LSan suppressions (`src/core/LsanSuppressions.hpp`) cover third-party GPU-driver allocations only; our code is never suppressed.
- TSan (`xmake test-tsan`): the distro Qt isn't instrumented, so Qt-internal synchronization is suppressed (`tools/tsan.supp`). TSan's thread registry aborts (`CHECK live_.try_emplace`) on thread-ID reuse once a process has started and stopped many threads, so `test-tsan` runs every suite in its own process, and the suites that start and stop `QThread`s or QML loader threads repeatedly (`settings.file`, `ui.settings`, `ui.presets`, `ui.icons`, `cache.service`) are excluded. The audio cache's multi-threaded readers, tagging and image decoding run under TSan (`cache.audio`, `cache.image`, `cache.integration`). Cross-thread hand-overs in `net` go through `core/Handoff.hpp`, which in TSan builds makes Qt's event-queue ordering visible with one extra mutex, so the network suites run without suppressions for our own code. **Known gap:** full TSan coverage needs a TSan-instrumented Qt build.
- Before each commit: `xmake format --check`, a debug build with `--werror=y`, unit tests, `xmake smoke`, `xmake i18n-check`, `xmake docs-config --check`, `xmake lint`.
