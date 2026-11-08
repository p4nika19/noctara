# GPU acceleration, fallbacks and Safe Mode

## Modes

| Setting `performance.gpu.acceleration` | Behaviour |
|---|---|
| `auto` (default) | Probe the GPU and use it unless it is missing, a CPU rasterizer, or blocklisted. Otherwise use software rendering and show a one-time notice. |
| `on` | Use the GPU even when it is a CPU rasterizer or blocklisted, as long as a device can be created. |
| `off` | Qt Quick software renderer. Glass styles fall back to *Solid*. |

The renderer is chosen before the first window exists, so changing it needs a restart. The settings page marks these rows *Restart required* and offers **Restart now**. Restart goes through `RelaunchGuard`, which calls `execve` on itself after a full orderly shutdown, so no child process is left behind. The new instance gets the environment the process started with, so the GPU selection variables the bootstrap set (`DRI_PRIME`, `QT_VK_PHYSICAL_DEVICE_INDEX`) do not pass for the user's own and a changed GPU preference applies.

Ways to set the mode: the config key, `--gpu=…`, `--disable-gpu`, `NOCTARA_PERFORMANCE_GPU_ACCELERATION`. The graphics API works the same way: `--graphics-api`, `performance.gpu.api`.

## Backends (Linux)

| API | Status |
|---|---|
| Vulkan (preferred under Auto) | explicit pipeline cache persisted between launches, QRhi GPU timestamps on every driver (the quality governor's best input), physical-device choice for *GPU preference*, lowest CPU overhead per frame |
| OpenGL | fallback: drivers without Vulkan, or when Vulkan is only a CPU rasterizer (lavapipe) |
| Software | Qt Quick software adaptation |

If an explicitly chosen API fails, the other one is tried and the user is told (`ApiFallback`). Under Auto, Vulkan is probed first. A Vulkan that turns out to be a CPU rasterizer does not end the search: lavapipe is often the only Vulkan driver installed next to a real GPU's OpenGL, and that GPU wins.

### Vulkan specifics

- **GPU preference.** On systems with several GPUs, the Vulkan probe enumerates the physical devices and selects the first integrated or discrete one for *Integrated* / *Discrete*. Qt's Vulkan backend then uses it through `QT_VK_PHYSICAL_DEVICE_INDEX`, for the probe and the window alike. The chosen index is stored in the probe cache, keyed by the preference. A value the user set themselves always wins. `DRI_PRIME` is still set for Mesa, which also covers OpenGL.
- **Pipeline cache.** `<cache>/pipelines-vulkan.bin` (and `pipelines-opengl.bin`) is loaded at startup and saved on exit through `QQuickGraphicsConfiguration`. Qt checks the file's header against the device and driver, and ignores a stale or foreign file.
- **Timestamps.** `VK_QUERY_TYPE_TIMESTAMP` is available on all desktop Vulkan drivers, so the governor works on GPU time. OpenGL ES through EGL often lacks timer queries.
- **Validation.** `QSG_RHI_DEBUG_LAYER=1` enables the Khronos validation layer when it is installed.
- **Platforms.** The offscreen and minimal platform plugins cannot host Vulkan, so the probe skips it there instead of logging Qt warnings.

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
6. Disarm the sentinel after the **first presented frame** (`frameSwapped`, queued to the GUI thread), or on orderly exit. A window that starts hidden in the tray initialises no graphics: the sentinel is disarmed at once and armed again when the window is first shown (`gfx::coverFirstFrame`).

### Probe cache

Probing costs about 0.9 s on the dev machine (NVIDIA GL), so results are cached in `~/.cache/noctara/gpu-probe.json`. The cache key is a fingerprint built from the Qt version, the platform plugin, GPU-related environment variables, the Vulkan ICD files, the GL/EGL/Vulkan loader and driver files (Mesa's versioned `libgallium-<version>.so` included, in `/usr/lib`, `/usr/lib64` and the multiarch directories), `/proc/driver/nvidia/version` and the GPUs listed under `/sys/class/drm` (PCI path, vendor and device id). A driver update or a GPU added or removed therefore invalidates the cache, and with it a cached Vulkan device index, without loading any driver at startup.

### Blocklist

`resources/data/gpu-blocklist.json` is **data, not code**. Each entry has an `id`, an optional PCI `vendorId`, regexes over `deviceName`/`driver`, an optional `api`, an `action` and a `reason`. Invalid entries are skipped and logged.

## Runtime failures

Every `QQuickWindow` has its `sceneGraphError` connected. Qt aborts the process if that signal has no receiver. On error, `GraphicsInfo::graphicsFailed` sets `acceleration = off` in the user config and shows an error toast with **Restart now**. The app does not crash.

## Safe Mode

Safe Mode means software rendering, Solid glass and no shader features. It starts automatically after a crash during graphics init, or when launched with `--safe-mode`. The notice explains how to get back: restart normally (the sentinel is cleared after a successful session), or change *Hardware acceleration* in Settings → Performance.

## Limitations of the software path

- `ShaderEffect`, custom materials and `QQuickRhiItem` don't work. Every GPU-only component has a CPU-safe fallback: Solid glass now, and the cached-image waveform and pre-blurred static backdrop in later slices.
- Blur styles and *Effects quality* are greyed out in Settings with a translated reason. Glass is Solid, and the quality governor does nothing.
- The cover-art backdrop works, because it is pre-blurred on the CPU (AccentEngine) and drawn as a plain image.
- Animations stay declarative. The drifting backdrop is disabled.

## Debugging rendering

- `QSG_INFO=1` prints the backend and adapter. `QSG_RHI_DEBUG_LAYER=1` enables Vulkan validation.
- `QSG_VISUALIZE=overdraw|batches|clip` shows scene-graph cost.
- `noctara --print-config` shows the effective acceleration settings and where each value came from.
- Delete `~/.cache/noctara/gpu-probe.json` to force a new probe.
- `xmake smoke` runs every page with `--gpu=off` and `--gpu=auto`, and every blur style on the GPU path. **The offscreen QPA does not draw `ShaderEffect`s**, even with a GPU backend, so offscreen screenshots and smoke runs do not exercise shader rendering. Use a real display: `NOCTARA_SMOKE_PLATFORM=wayland xmake smoke`, or `--screenshot` without `QT_QPA_PLATFORM=offscreen`.
- `QT_LOGGING_RULES="noctara.gfx.debug=true"` prints the per-second frame statistics: frames, fps, GPU time p90 (QRhi timestamps), frame interval p90, effective style and quality.

## Quality governor and energy saving

`gfx::QualityGovernor` is pure and unit-tested. Once per second of rendering, `FrameMonitor` reports the GPU time per frame (QRhi timestamps enabled through `QQuickGraphicsConfiguration`; the window is shown only after that) and the frame intervals. With *Effects quality* = Auto:

- GPU timestamps are not available everywhere: OpenGL ES through EGL (Wayland on NVIDIA, for example) reports none. The governor then judges the frame intervals, and the Performance page shows "Not reported yet" for the GPU time. Intervals longer than 2.25 frame budgets (and 33 ms) count as pauses between low-rate redraws, such as the 12 Hz backdrop drift, not as slow frames.
- **Down:** two consecutive overloaded windows (GPU p90 > 85% of the frame budget, or without timestamps a frame-interval p90 > 1.5 budgets) step down one tier: High → Medium (capture 1/3) → Low (1/4, one level fewer) → Dual Kawase (style cap) → Solid.
- **Up:** five consecutive healthy windows (GPU < 45% of the budget) after a cooldown of 20 s step up one tier. A step down within 30 s of a step up doubles the cooldown (up to 10 min).
- Idle windows (fewer than 30 frames) are not judged, so a static scene never moves the governor.
- A manual quality disables the governor. So does software rendering. Changing the setting resets it.

Energy saving (always, or automatically on battery through UPower) stops the backdrop drift, caps the style at Dual Kawase and the quality at Medium, and limits rendering to 30 fps, except for one second after each user input (`FramePacer::capFor`), so it slows only what plays by itself. The sleep ends as soon as an input arrives. A frame cap chosen in the settings always applies. `gfx::resolveEffects()` combines all of this. The frame cap (setting or energy saving) works by sleeping on the render thread after a frame is presented. Qt's animation driver uses wall time, so animations keep their duration (verified: a 4 s animation takes the same time capped and uncapped). It is never applied on the GUI thread, where the software renderer draws.
