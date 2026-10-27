# GPU acceleration, fallbacks and Safe Mode

## Modes

| Setting `performance.gpu.acceleration` | Behaviour |
|---|---|
| `auto` (default) | Probe the GPU and use it unless it is missing, a CPU rasterizer, or blocklisted. Otherwise use software rendering and show a one-time notice. |
| `on` | Use the GPU even when it is a CPU rasterizer or blocklisted, as long as a device can be created. |
| `off` | Qt Quick software renderer. Glass styles fall back to *Solid*. |

The renderer is chosen before the first window exists, so changing it needs a restart. The settings page marks these rows *Restart required* and offers **Restart now**. Restart goes through `RelaunchGuard`, which calls `execv` on itself after a full orderly shutdown, so no child process is left behind.

Ways to set the mode: the config key, `--gpu=…`, `--disable-gpu`, `NOCTARA_PERFORMANCE_GPU_ACCELERATION`. The graphics API works the same way: `--graphics-api`, `performance.gpu.api`.

## Backends (Linux)

| API | Status |
|---|---|
| OpenGL (default under Auto) | Qt Quick's best-tested backend on Linux |
| Vulkan | supported; used if OpenGL fails, or when chosen explicitly |
| Software | Qt Quick software adaptation |

If an explicitly chosen API fails, the other one is tried and the user is told (`ApiFallback`).

## Bootstrap sequence (`gfx::bootstrapGraphics`)

1. Apply the GPU preference environment (`DRI_PRIME`) before any context exists.
2. Read the request from settings, `--safe-mode` and the crash sentinel.
3. **Arm the crash sentinel** (`~/.local/state/noctara/graphics-init.sentinel`) before probing.
4. `gfx::decide()`, which is pure and unit-tested with mocked probes:
   - `--safe-mode` → software, Safe Mode, notice.
   - Sentinel found → previous launch crashed → software, Safe Mode, notice.
   - `off` → software, no notice.
   - Otherwise probe the candidate APIs in order. The probe creates a throw-away `QRhi` and reads `QRhiDriverInfo`. A CPU device or a known rasterizer name (llvmpipe, lavapipe, SwiftShader…) → software unless forced `on`. Blocklist actions: `software`, `avoid-api`, `no-liquid-glass`.
5. Apply: `QQuickWindow::setGraphicsApi`, and the default surface format (VSync swap interval).
6. Disarm the sentinel after the **first presented frame** (`frameSwapped`, queued to the GUI thread), or on orderly exit.

### Probe cache

Probing costs about 0.9 s on the dev machine (NVIDIA GL), so results are cached in `~/.cache/noctara/gpu-probe.json`. The cache key is a fingerprint built from the Qt version, the platform plugin, GPU-related environment variables, the Vulkan ICD files, the GL/EGL/Vulkan loader files and `/proc/driver/nvidia/version`. A driver update therefore invalidates the cache without loading any driver at startup.

### Blocklist

`resources/data/gpu-blocklist.json` is **data, not code**. Each entry has an `id`, an optional PCI `vendorId`, regexes over `deviceName`/`driver`, an optional `api`, an `action` and a `reason`. Invalid entries are skipped and logged.

## Runtime failures

Every `QQuickWindow` has its `sceneGraphError` connected. Qt aborts the process if that signal has no receiver. On error, `GraphicsInfo::graphicsFailed` sets `acceleration = off` in the user config and shows an error toast with **Restart now**. The app does not crash.

## Safe Mode

Safe Mode means software rendering, Solid glass and no shader features. It starts automatically after a crash during graphics init, or when launched with `--safe-mode`. The notice explains how to get back: restart normally (the sentinel is cleared after a successful session), or change *Hardware acceleration* in Settings → Performance.

## Limitations of the software path

- `ShaderEffect`, custom materials and `QQuickRhiItem` don't work. Every GPU-only component has a CPU-safe fallback: Solid glass now, and the cached-image waveform and pre-blurred static backdrop in later slices.
- Blur styles are greyed out in Settings with a translated reason.
- Animations stay declarative. The drifting backdrop is disabled.

## Debugging rendering

- `QSG_INFO=1` prints the backend and adapter. `QSG_RHI_DEBUG_LAYER=1` enables Vulkan validation.
- `QSG_VISUALIZE=overdraw|batches|clip` shows scene-graph cost.
- `noctara --print-config` shows the effective acceleration settings and where each value came from.
- Delete `~/.cache/noctara/gpu-probe.json` to force a new probe.
- `xmake smoke` runs every page with `--gpu=off` and `--gpu=auto`.
