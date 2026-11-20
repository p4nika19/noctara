# Noctara

Noctara is a native SoundCloud desktop client written in C++23 and Qt 6 Quick. It uses glass surfaces over a backdrop taken from the cover art, comes with two themes (Midnight and Frost), and runs with GPU acceleration or a full software fallback. The UI is localized in English and Russian.

> Status: slices (a)–(i) of the plan are done (foundations, glass pipeline, network stack, caches, SoundCloud api-v2 models, authentication, native playback, pages and customization, and protected playback through a castlabs Electron helper). See [docs/plan.md](docs/plan.md).

## Install

Paste this into a terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/p4nika19/noctara/main/get.sh | bash
```

It fetches the sources into `~/.cache/noctara-source`, offers to install the build dependencies with your package
manager (pacman, dnf, zypper, apt, xbps, apk; Gentoo gets the `emerge` line to run), builds Noctara for your CPU and
installs it for your user into `~/.local` (app menu entry included). Nothing runs as root except the package
installation (and copying the files for `--system`). Building takes a few minutes.

- **Update:** run the same line again. When Noctara is installed, the installer asks whether to update it, remove it
  or leave.
- **Uninstall:** `~/.local/lib/noctara/uninstall.sh` (it asks whether to keep your settings, cache and sign-in;
  `--purge` deletes them, `--keep` keeps them).
- **Options** go after `bash -s --`: `--system` (into `/usr/local` for every user), `--prefix DIR`, `--update`,
  `--uninstall [--purge]`, `--yes` (no questions), `--no-deps`. From a checkout, `./install.sh --help` and
  `./uninstall.sh --help` list the same.
- **Needs** x86_64 and a current toolchain: Qt 6.12 or newer, clang 19 or newer with GCC 14's C++ library, xmake 3
  (installed into `~/.local/bin` with your consent when the repositories lack it) and the libraries below. Today
  that is Arch and its derivatives (EndeavourOS, CachyOS), openSUSE Tumbleweed, Fedora rawhide and Gentoo; Fedora 44,
  Debian sid, Manjaro stable, Alpine edge and Void ship Qt 6.11 and older releases older still. The installer checks
  everything first and says what is missing. Protected (Go+) tracks also need glibc.

## Dependencies

| Dependency | Tested version | Source |
|---|---|---|
| clang, mold, xmake | 23.1, 3.0, 3.1 | system |
| Qt 6 (Core, Gui, Qml, Quick, Svg, Concurrent, Test, ShaderTools, Linguist tools) | 6.12 | system |
| toml++ (header-only use) | 3.4 | system headers |
| libcurl (with TLS, HTTP/2, IPv6; HTTP/3 optional) | 8.23 | system, pkg-config |
| libproxy | 0.5 | system, pkg-config |
| libsecret | 0.21 | system, pkg-config |
| TagLib | 2.3 | system, pkg-config |
| xxHash | 0.8 | system, pkg-config |
| OpenSSL (cookie/secret encryption and mock TLS tests) | 3.6 | system, pkg-config |
| sqlite3 (read-only browser cookie snapshots) | 3.54 | system, pkg-config |
| libarchive (unpacking the protected player Noctara sets up) | 3.8 | system, pkg-config |
| doctest | 2.4.12 | xrepo (fetched automatically) |

Arch (everything needed to build, the same list CI installs):

```sh
pacman -S --needed clang mold xmake pkgconf \
    qt6-base qt6-declarative qt6-svg qt6-shadertools qt6-tools qt6-translations qt6-multimedia tomlplusplus \
    curl libarchive libproxy libsecret openssl sqlite simdjson taglib xxhash ffmpeg
```

`qt6-tools` provides `lrelease`/`lupdate` (translations) and `qt6-shadertools` provides `qsb` (shaders); without
them configuring stops with "… not found in the Qt installation".

Debian/Ubuntu: `clang mold xmake pkgconf qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-svg-dev
qt6-shadertools-dev qt6-shader-baker qt6-l10n-tools qt6-tools-dev qt6-multimedia-dev libtomlplusplus-dev
libcurl4-openssl-dev libarchive-dev libproxy-dev libsecret-1-dev libssl-dev libsqlite3-dev libsimdjson-dev
libtag-dev libxxhash-dev libavformat-dev libavcodec-dev libavutil-dev libswresample-dev` (package names vary
between releases; the Arch list is the tested one).

The build fails early if libcurl lacks SSL, HTTP2 or IPv6. Native playback needs FFmpeg
(libavformat, libavcodec, libavutil, libswresample) and Qt Multimedia. Protected (DRM) tracks play in a castlabs Electron helper that Noctara downloads and sets up by itself
(pinned and verified, about 125 MB once); `xmake drm-helper` builds the same thing into `build/drm-helper` for
development (needs `curl` and `unzip`). See [docs/drm-helper.md](docs/drm-helper.md).

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
| `xmake test-tsan` | unit tests under ThreadSanitizer, one process per suite (`xmake f -m tsan` first) |
| `xmake format [--check]`, `xmake lint` | clang-format, clang-tidy |
| `xmake i18n-update`, `xmake i18n-release`, `xmake i18n-check` | Qt Linguist workflow and CI completeness gate |
| `xmake docs-config [--check]` | regenerate or verify `docs/configuration.md` from the settings schema |
| `xmake drm-helper` | download (pinned, SHA-256 verified) and assemble the protected-playback helper in `build/drm-helper` |
| `xmake build noctara-bench` | hot-path benchmarks (run the release build; see [performance](docs/performance.md)) |
| `xmake f --trace=y` | compile in the trace layer: a Chrome trace JSON (`$NOCTARA_TRACE_FILE`) is written at exit |
| `xmake pgo`, `xmake package` | profile-guided build; release tarball with desktop file and icon in `build/dist` (`xmake f -m release` first) |

## Command line

```
noctara [--config <file>] [--print-config] [--reset-config] [--safe-mode] [--disable-gpu]
        [--set key=value ...] [--gpu=auto|on|off] [--graphics-api=auto|vulkan|opengl]
        [--theme=system|dark|light] [--lang=system|en|ru]
```

Settings are layered with this precedence: defaults < `/etc/xdg/noctara/config.toml` < `~/.config/noctara/config.toml` < `NOCTARA_*` environment variables < command line. See [docs/configuration.md](docs/configuration.md).

## Documentation

[architecture](docs/architecture.md) · [design and tokens](docs/design.md) · [configuration](docs/configuration.md) · [GPU and fallbacks](docs/gpu.md) · [network](docs/network.md) · [caching](docs/caching.md) · [SoundCloud API](docs/api.md) · [authentication](docs/auth.md) · [playback](docs/playback.md) · [protected playback](docs/drm-helper.md) · [helper IPC](docs/ipc.md) · [themes](docs/themes.md) · [localization](docs/localization.md) · [performance](docs/performance.md)

## Licenses

Noctara is distributed under the [Mozilla Public License 2.0](LICENSE).

The bundled fonts (Manrope, Inter, JetBrains Mono) are under the SIL Open Font License 1.1, and the icons (Lucide) under the ISC License. The license texts are in `resources/` and in the app's About page. The protected-playback helper is castlabs Electron (MIT, with Chromium's licenses in its zip) and hls.js (Apache-2.0), downloaded by `xmake drm-helper`, not part of this repository.
