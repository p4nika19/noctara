# Noctara

Noctara is a native SoundCloud desktop client written in C++23 and Qt 6 Quick. It uses glass surfaces over a backdrop taken from the cover art, comes with two themes (Midnight and Frost), and runs with GPU acceleration or a full software fallback. The UI is localized in English and Russian.

> Status: slices (a)–(c) of the plan are done (foundations, glass pipeline, network stack: DNS/DoH, proxies, transport). See [docs/plan.md](docs/plan.md).

## Dependencies

| Dependency | Tested version | Source |
|---|---|---|
| clang, mold, xmake | 23.1, 3.0, 3.1 | system |
| Qt 6 (Core, Gui, Qml, Quick, Svg, Concurrent, Test, ShaderTools, Linguist tools) | 6.12 | system |
| toml++ (header-only use) | 3.4 | system headers |
| libcurl (with TLS, HTTP/2, IPv6; HTTP/3 optional) | 8.23 | system, pkg-config |
| libproxy | 0.5 | system, pkg-config |
| libsecret | 0.21 | system, pkg-config |
| OpenSSL (tests only: mock TLS server) | 3.6 | system, pkg-config |
| doctest | 2.4.12 | xrepo (fetched automatically) |

Arch: `pacman -S curl libproxy libsecret openssl`; Debian/Ubuntu: `libcurl4-openssl-dev libproxy-dev libsecret-1-dev
libssl-dev`. The build fails early if libcurl lacks SSL, HTTP2 or IPv6. Later slices add FFmpeg, TagLib, sqlite and
simdjson.

## Build and run

```sh
xmake f -m release --qt=/usr/lib/qt6   # or -m debug (ASan+UBSan), -m tsan
xmake                                  # builds the app; a copy is placed at build/noctara
xmake run noctara
```

Useful options: `--werror=y` (always on in release and CI), `--march=native`, `--lto=n`, `--sanitizers=n`.

## Tasks

| Task | Purpose |
|---|---|
| `xmake build -a && xmake run noctara-tests` | unit tests |
| `xmake smoke` | headless run of every page in software and GPU mode, both themes and both languages; fails on any warning |
| `xmake test-tsan` | unit tests under ThreadSanitizer (`xmake f -m tsan` first) |
| `xmake format [--check]`, `xmake lint` | clang-format, clang-tidy |
| `xmake i18n-update`, `xmake i18n-release`, `xmake i18n-check` | Qt Linguist workflow and CI completeness gate |
| `xmake docs-config [--check]` | regenerate or verify `docs/configuration.md` from the settings schema |
| `xmake pgo`, `xmake package` | profile-guided build, release tarball |

## Command line

```
noctara [--config <file>] [--print-config] [--reset-config] [--safe-mode] [--disable-gpu]
        [--set key=value ...] [--gpu=auto|on|off] [--graphics-api=auto|vulkan|opengl]
        [--theme=system|dark|light] [--lang=system|en|ru]
```

Settings are layered with this precedence: defaults < `/etc/xdg/noctara/config.toml` < `~/.config/noctara/config.toml` < `NOCTARA_*` environment variables < command line. See [docs/configuration.md](docs/configuration.md).

## Documentation

[architecture](docs/architecture.md) · [design and tokens](docs/design.md) · [configuration](docs/configuration.md) · [GPU and fallbacks](docs/gpu.md) · [localization](docs/localization.md) · [performance](docs/performance.md)

## Licenses

The bundled fonts (Manrope, Inter, JetBrains Mono) are under the SIL Open Font License 1.1, and the icons (Lucide) under the ISC License. The license texts are in `resources/` and in the app's About page.
