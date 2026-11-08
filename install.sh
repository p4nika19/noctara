#!/usr/bin/env bash
# Builds Noctara from this source tree and installs it. The one-line installer (get.sh, see README.md) fetches the
# sources and runs this script; it can also be run by hand from a checkout:
#
#   ./install.sh                 into ~/.local, for your user (sudo is asked only for build dependencies)
#   ./install.sh --system        into /usr/local, for every user (sudo is also asked to copy the files)
#   ./install.sh --prefix DIR    into DIR
#   ./install.sh --no-deps       do not install build dependencies, only check them
#   ./install.sh --yes           do not ask before installing packages
#   ./install.sh --jobs N        build with N parallel jobs (default: all CPUs)
#
# Supported package managers: pacman (Arch, Manjaro, EndeavourOS, CachyOS), dnf (Fedora), zypper (openSUSE),
# apt (Debian, Ubuntu, Mint), xbps (Void), apk (Alpine), emerge (Gentoo, printed only).
set -euo pipefail

SOURCE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX="$HOME/.local"
DEPS=1
ASSUME_YES=0
JOBS=$(nproc 2>/dev/null || echo 4)

# The oldest versions Noctara builds with (checked again by compiling a probe, which is what really counts).
MIN_QT=6.8.0
MIN_CLANG=19
MIN_XMAKE=3.0.0

# --- Output -------------------------------------------------------------------------------------------------------

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) RU=1 ;; *) RU=0 ;; esac
# say "English" "Русский": prints the text in the user's language.
say() { if (( RU )) && [[ -n ${2:-} ]]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi; }
if [[ -t 1 ]]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B=; G=; Y=; R=; N=; fi
step() { printf '%s==>%s %s%s%s\n' "$G" "$N" "$B" "$(say "$@")" "$N"; }
warn() { printf '%s!!%s %s\n' "$Y" "$N" "$(say "$@")" >&2; }
die() { printf '%sxx%s %s\n' "$R" "$N" "$(say "$@")" >&2; exit 1; }

# Questions read the terminal even when the script itself arrives on stdin (curl … | bash).
ask() {  # ask "English?" "Русский?" -> 0 for yes
    (( ASSUME_YES )) && return 0
    (: < /dev/tty) 2>/dev/null || return 1
    local answer
    printf '%s [Y/n] ' "$(say "$@")" > /dev/tty
    read -r answer < /dev/tty || return 1
    [[ -z $answer || $answer =~ ^[YyДд] ]]
}

# --- Arguments ------------------------------------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="${2:?}"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --system) PREFIX=/usr/local; shift ;;
        --no-deps) DEPS=0; shift ;;
        -y|--yes) ASSUME_YES=1; shift ;;
        --jobs) JOBS="${2:?}"; shift 2 ;;
        --jobs=*) JOBS="${1#*=}"; shift ;;
        -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown option: $1 (see --help)" "Неизвестный параметр: $1 (см. --help)" ;;
    esac
done
PREFIX=${PREFIX%/}
[[ $PREFIX == /* ]] || die "--prefix must be an absolute path" "--prefix должен быть абсолютным путём"
(( EUID != 0 )) || die "Run this as your user, not as root: sudo is asked for when it is needed." \
                       "Запускайте от своего пользователя, не от root: sudo будет запрошен, когда понадобится."
[[ -f $SOURCE_DIR/xmake.lua && -f $SOURCE_DIR/src/app/main.cpp ]] \
    || die "install.sh must stay in the Noctara source tree." "install.sh должен лежать в исходниках Noctara."

SUDO=()
if (( EUID != 0 )); then command -v sudo >/dev/null && SUDO=(sudo) || { command -v doas >/dev/null && SUDO=(doas); }; fi

# --- Distribution and packages --------------------------------------------------------------------------------------

OS_ID=unknown; OS_LIKE=; OS_NAME="Linux"
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    OS_ID=$(. /etc/os-release && echo "${ID:-unknown}"); OS_LIKE=$(. /etc/os-release && echo "${ID_LIKE:-}")
    OS_NAME=$(. /etc/os-release && echo "${PRETTY_NAME:-$OS_ID}")
fi

package_manager() {
    local pm
    for pm in pacman dnf zypper apt-get xbps-install apk emerge nix-env; do
        command -v "$pm" >/dev/null && { echo "$pm"; return; }
    done
    echo none
}
PM=$(package_manager)

# Build and run dependencies per package manager: toolchain (clang, mold, xmake, pkg-config, git), Qt 6 (base,
# declarative, svg, shadertools for qsb, tools for lrelease/lupdate, multimedia, wayland), and the libraries
# README.md lists (curl, libarchive, libproxy, libsecret, OpenSSL, SQLite, simdjson, TagLib, xxHash, FFmpeg, toml++).
packages() {
    case "$1" in
    pacman) echo clang mold xmake pkgconf git unzip qt6-base qt6-declarative qt6-svg qt6-shadertools qt6-tools \
                 qt6-translations qt6-multimedia qt6-multimedia-ffmpeg qt6-wayland tomlplusplus curl libarchive \
                 libproxy libsecret openssl sqlite simdjson taglib xxhash ffmpeg ;;
    dnf) echo clang mold xmake pkgconf-pkg-config git unzip qt6-qtbase-devel qt6-qtbase-private-devel \
              qt6-qtdeclarative-devel qt6-qtsvg-devel qt6-qtshadertools-devel qt6-qttools-devel qt6-linguist \
              qt6-qtmultimedia-devel qt6-qtwayland qt6-qttranslations tomlplusplus-devel libcurl-devel \
              libarchive-devel libproxy-devel libsecret-devel openssl-devel sqlite-devel simdjson-devel taglib-devel \
              xxhash-devel libavformat-free-devel libavcodec-free-devel libavutil-free-devel \
              libswresample-free-devel ;;
    zypper) echo clang mold xmake pkgconf git unzip qt6-base-devel qt6-base-private-devel qt6-declarative-devel \
                 qt6-svg-devel qt6-shadertools-devel qt6-tools-devel qt6-linguist-devel qt6-multimedia-devel \
                 qt6-wayland qt6-translations tomlplusplus-devel libcurl-devel libarchive-devel libproxy-devel \
                 libsecret-devel libopenssl-devel sqlite3-devel simdjson-devel taglib-devel xxhash-devel \
                 ffmpeg-7-libavformat-devel ffmpeg-7-libavcodec-devel ffmpeg-7-libavutil-devel \
                 ffmpeg-7-libswresample-devel ;;
    apt-get) echo clang lld mold pkgconf git unzip qt6-base-dev qt6-base-private-dev qt6-base-dev-tools \
                  qt6-declarative-dev qt6-declarative-private-dev qt6-svg-dev qt6-shadertools-dev qt6-shader-baker \
                  qt6-tools-dev qt6-l10n-tools qt6-multimedia-dev qt6-wayland qt6-translations-l10n \
                  qml6-module-qtquick qml6-module-qtquick-templates qml6-module-qtquick-window \
                  qml6-module-qtquick-controls qml6-module-qtqml-workerscript libtomlplusplus-dev \
                  libcurl4-openssl-dev libarchive-dev libproxy-dev libsecret-1-dev libssl-dev libsqlite3-dev \
                  libsimdjson-dev libtag-dev libxxhash-dev libavformat-dev libavcodec-dev libavutil-dev \
                  libswresample-dev ;;
    xbps-install) echo clang mold xmake pkgconf git unzip qt6-base-devel qt6-declarative-devel qt6-svg-devel \
                       qt6-shadertools-devel qt6-tools-devel qt6-multimedia-devel qt6-wayland tomlplusplus \
                       libcurl-devel libarchive-devel libproxy-devel libsecret-devel openssl-devel sqlite-devel \
                       simdjson-devel taglib-devel xxHash-devel ffmpeg-devel ;;
    apk) echo clang mold xmake pkgconf git unzip build-base qt6-qtbase-dev qt6-qtbase-private-dev \
              qt6-qtdeclarative-dev qt6-qtsvg-dev qt6-qtshadertools-dev qt6-qttools-dev qt6-qtmultimedia-dev \
              qt6-qtwayland tomlplusplus-dev curl-dev libarchive-dev libproxy-dev libsecret-dev openssl-dev \
              sqlite-dev simdjson-dev taglib-dev xxhash-dev ffmpeg-dev ;;
    emerge) echo llvm-core/clang sys-devel/mold dev-build/xmake dev-util/pkgconf dev-vcs/git app-arch/unzip \
                 dev-qt/qtbase dev-qt/qtdeclarative dev-qt/qtsvg dev-qt/qtshadertools dev-qt/qttools \
                 dev-qt/qtmultimedia dev-qt/qtwayland dev-cpp/tomlplusplus net-misc/curl app-arch/libarchive \
                 net-libs/libproxy app-crypt/libsecret dev-libs/openssl dev-db/sqlite dev-libs/simdjson \
                 media-libs/taglib dev-libs/xxhash media-video/ffmpeg ;;
    esac
}

install_command() {
    case "$1" in
    pacman) echo "${SUDO[*]} pacman -S --needed --noconfirm" ;;
    dnf) echo "${SUDO[*]} dnf install -y --skip-unavailable" ;;
    zypper) echo "${SUDO[*]} zypper --non-interactive install --no-recommends" ;;
    apt-get) echo "${SUDO[*]} apt-get install -y --no-install-recommends" ;;
    xbps-install) echo "${SUDO[*]} xbps-install -Sy" ;;
    apk) echo "${SUDO[*]} apk add" ;;
    emerge) echo "${SUDO[*]} emerge --ask --noreplace" ;;
    esac
}

install_dependencies() {
    local list command
    case "$PM" in
    none) warn "No known package manager: install the dependencies from README.md yourself." \
               "Пакетный менеджер не распознан: установите зависимости из README.md вручную."
          return ;;
    nix-env) warn "On NixOS build inside a shell with the dependencies from README.md (nix-shell -p …)." \
                  "В NixOS собирайте в оболочке с зависимостями из README.md (nix-shell -p …)."
             return ;;
    esac
    list=$(packages "$PM")
    command=$(install_command "$PM")
    step "Build dependencies for $OS_NAME ($PM):" "Зависимости для сборки в $OS_NAME ($PM):"
    printf '    %s %s\n' "$command" "$list"
    if [[ $PM == emerge ]]; then
        warn "Gentoo compiles every package: run the command above yourself, then this installer again." \
             "В Gentoo каждый пакет компилируется: выполните команду выше сами и запустите установщик снова."
        return
    fi
    if (( ${#SUDO[@]} == 0 )); then
        warn "Neither sudo nor doas found: install the packages above as root, then run this again." \
             "Нет ни sudo, ни doas: установите пакеты выше от root и запустите установщик снова."
        return
    fi
    ask "Install them now?" "Установить их сейчас?" || { warn "Skipped." "Пропущено."; return; }
    [[ $PM == apt-get ]] && "${SUDO[@]}" apt-get update -q
    # shellcheck disable=SC2086 # the command and the list are word lists on purpose
    $command $list || warn "The package manager reported a problem; checking what is missing…" \
                           "Пакетный менеджер сообщил об ошибке; проверяю, чего не хватает…"
}

# --- Checks ---------------------------------------------------------------------------------------------------------

version_at_least() {  # version_at_least 6.10.1 6.8.0
    [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" == "$2" ]]
}

qt_tool() {  # first of the given Qt tools that exists
    local tool
    for tool in "$@"; do command -v "$tool" >/dev/null && { echo "$tool"; return; }; done
    for tool in "$@"; do
        local dir
        for dir in /usr/lib/qt6/bin /usr/lib64/qt6/bin /usr/lib/x86_64-linux-gnu/qt6/bin /usr/lib/qt6/libexec; do
            [[ -x $dir/$tool ]] && { echo "$dir/$tool"; return; }
        done
    done
}

# The Qt installation xmake must use (--qt): the parent of Qt's host bin directory. Without it xmake may pick Qt 5.
qt_dir() {
    local qmake
    qmake=$(qt_tool qmake6 qmake-qt6 qmake)
    [[ -n $qmake ]] && [[ $("$qmake" -query QT_VERSION 2>/dev/null) == 6.* ]] || return 0
    dirname "$("$qmake" -query QT_HOST_BINS)"
}

check_toolchain() {
    local problems=() qmake qtversion clang_version xmake_version probe
    command -v clang++ >/dev/null || problems+=("clang (C++ compiler, $MIN_CLANG or newer)")
    command -v mold >/dev/null || problems+=("mold (linker)")
    command -v pkg-config >/dev/null || problems+=("pkg-config")
    command -v git >/dev/null || problems+=("git")

    if command -v xmake >/dev/null; then
        xmake_version=$(xmake --version 2>/dev/null | grep -oE 'v[0-9]+\.[0-9]+\.[0-9]+' | head -n1 | tr -d v)
        version_at_least "${xmake_version:-0}" "$MIN_XMAKE" || problems+=("xmake $MIN_XMAKE or newer (found ${xmake_version:-?})")
    else
        problems+=("xmake $MIN_XMAKE or newer")
    fi

    qmake=$(qt_tool qmake6 qmake-qt6 qmake)
    if [[ -n $qmake ]] && qtversion=$("$qmake" -query QT_VERSION 2>/dev/null) && [[ $qtversion == 6.* ]]; then
        version_at_least "$qtversion" "$MIN_QT" || problems+=("Qt $MIN_QT or newer (found $qtversion)")
    else
        problems+=("Qt 6 (qmake6)")
    fi
    [[ -n $(qt_tool qsb) ]] || problems+=("Qt Shader Tools (qsb)")
    [[ -n $(qt_tool lrelease lrelease6 lrelease-qt6) ]] || problems+=("Qt Linguist tools (lrelease)")

    if command -v pkg-config >/dev/null; then
        local module
        for module in libcurl libarchive libproxy-1.0 libsecret-1 openssl sqlite3 simdjson 'taglib >= 2.0' libxxhash \
                      libavformat libavcodec libavutil libswresample; do
            pkg-config --exists "$module" || problems+=("$module (pkg-config)")
        done
    fi

    # The compiler and its C++ library must handle what the code uses: C++23 <print>, <expected>, ranges, and C23
    # #embed. Compiling a probe answers that for every distribution at once.
    if command -v clang++ >/dev/null; then
        clang_version=$(clang++ -dumpversion 2>/dev/null | cut -d. -f1)
        probe=$(mktemp -d)
        printf 'ok' > "$probe/data.txt"
        cat > "$probe/probe.cpp" <<'CPP'
#include <algorithm>
#include <expected>
#include <print>
#include <stop_token>
#include <thread>
#include <vector>
int main()
{
    static constexpr char data[] = {
#embed "data.txt"
    };
    const std::vector<int> v{1, 2};
    const std::expected<int, int> e = 1;
    std::jthread t([](std::stop_token) {});
    std::println("{}", std::ranges::contains(v, 2) && *e == 1 && sizeof(data) == 2);
}
CPP
        if ! (cd "$probe" && clang++ -std=c++23 -Wno-c23-extensions -o probe probe.cpp >/dev/null 2>&1); then
            problems+=("a clang ($MIN_CLANG+) and C++ library (GCC 14+ libstdc++) with C++23 support (found clang ${clang_version:-?})")
        fi
        rm -rf "$probe"
    fi
    (( ${#problems[@]} == 0 )) && return 0
    printf '%s\n' "${problems[@]}"
    return 1
}

# --- Main -----------------------------------------------------------------------------------------------------------

step "Installing Noctara into $PREFIX" "Установка Noctara в $PREFIX"
case "$(uname -m)" in
    x86_64) ;;
    *) warn "Protected (Go+) tracks need x86_64: on $(uname -m) they will not play." \
            "Защищённым (Go+) трекам нужна x86_64: на $(uname -m) они воспроизводиться не будут." ;;
esac
if [[ $OS_ID == alpine ]] || ldd --version 2>&1 | grep -qi musl; then
    warn "Protected (Go+) tracks need glibc: on musl systems they will not play." \
         "Защищённым (Go+) трекам нужна glibc: в системах с musl они воспроизводиться не будут."
fi

(( DEPS )) && install_dependencies
if ! missing=$(check_toolchain); then
    say "Still missing or too old:" "Всё ещё не хватает или устарело:" >&2
    while IFS= read -r line; do printf '    - %s\n' "$line" >&2; done <<< "$missing"
    if grep -q "^xmake" <<< "$missing" && ! grep -qv "^xmake" <<< "$missing"; then
        warn "Your distribution has no recent xmake: its own installer puts one into ~/.local/bin:" \
             "В вашем дистрибутиве нет свежего xmake; официальный установщик поставит его в ~/.local/bin:"
        printf '    curl -fsSL https://xmake.io/shget.text | bash\n' >&2
    fi
    die "Noctara needs a recent toolchain (Qt $MIN_QT+, clang $MIN_CLANG+, GCC 14+). Stable releases of some distributions are too old; see README.md." \
        "Noctara нужен свежий набор инструментов (Qt $MIN_QT+, clang $MIN_CLANG+, GCC 14+). Стабильные выпуски некоторых дистрибутивов для этого слишком старые; см. README.md."
fi

QT_DIR=$(qt_dir)
[[ -n $QT_DIR ]] || die "Qt 6 was not found." "Qt 6 не найдена."
step "Building (this takes a few minutes)…" "Сборка (займёт несколько минут)…"
cd "$SOURCE_DIR"
# Built for this machine (--march=native) and without -Werror: another compiler version may warn about new things.
configure_log=$(mktemp)
if ! xmake f -y -m release --qt="$QT_DIR" --march=native --release_werror=n > "$configure_log" 2>&1; then
    cat "$configure_log" >&2
    rm -f "$configure_log"
    die "Configuring the build failed (see above)." "Не удалось настроить сборку (см. выше)."
fi
rm -f "$configure_log"
xmake build -y -j "$JOBS" noctara || die "The build failed (see above)." "Сборка не удалась (см. выше)."

step "Copying files" "Копирование файлов"
INSTALL=()
if [[ ! -w $PREFIX && -d $PREFIX ]] || [[ ! -d $PREFIX && ! -w $(dirname "$PREFIX") ]]; then
    (( ${#SUDO[@]} )) || die "$PREFIX is not writable and sudo is missing." "$PREFIX недоступен для записи, а sudo нет."
    INSTALL=("${SUDO[@]}")
fi
LIBDIR="$PREFIX/lib/noctara"
SHARE="$PREFIX/share"
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT
# The desktop entry starts the installed binary by its full path: ~/.local/bin is not in every desktop's PATH.
sed "s|^Exec=noctara|Exec=$PREFIX/bin/noctara|" resources/app/noctara.desktop > "$staging/noctara.desktop"
revision=$(git -C "$SOURCE_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)
cat > "$staging/manifest" <<EOF
$PREFIX/bin/noctara
$SHARE/applications/noctara.desktop
$SHARE/icons/hicolor/scalable/apps/noctara.svg
$LIBDIR/uninstall.sh
$LIBDIR/version
$LIBDIR/manifest
EOF
printf 'revision=%s\ninstalled=%s\nprefix=%s\n' "$revision" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$PREFIX" > "$staging/version"
"${INSTALL[@]}" install -Dm755 build/noctara "$PREFIX/bin/noctara"
"${INSTALL[@]}" install -Dm644 "$staging/noctara.desktop" "$SHARE/applications/noctara.desktop"
"${INSTALL[@]}" install -Dm644 resources/app/noctara.svg "$SHARE/icons/hicolor/scalable/apps/noctara.svg"
"${INSTALL[@]}" install -Dm755 uninstall.sh "$LIBDIR/uninstall.sh"
"${INSTALL[@]}" install -Dm644 "$staging/version" "$LIBDIR/version"
"${INSTALL[@]}" install -Dm644 "$staging/manifest" "$LIBDIR/manifest"
# Refresh the desktop's caches where they exist; never create one (a new icon cache in ~/.local would hide icons other
# programs add there later without updating it).
if [[ -f $SHARE/applications/mimeinfo.cache ]] && command -v update-desktop-database >/dev/null; then
    "${INSTALL[@]}" update-desktop-database -q "$SHARE/applications" || true
fi
if [[ -f $SHARE/icons/hicolor/icon-theme.cache ]] && command -v gtk-update-icon-cache >/dev/null; then
    "${INSTALL[@]}" gtk-update-icon-cache -qtf "$SHARE/icons/hicolor" || true
fi

step "Noctara is installed ($revision)." "Noctara установлена ($revision)."
say "  Start it from your app menu or with: $PREFIX/bin/noctara" "  Запустите её из меню приложений или командой: $PREFIX/bin/noctara"
say "  Update: run the one-line installer again. Remove: $LIBDIR/uninstall.sh" \
    "  Обновление: снова выполните однострочник. Удаление: $LIBDIR/uninstall.sh"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) say "  ($PREFIX/bin is not in your PATH.)" "  ($PREFIX/bin нет в вашем PATH.)" ;; esac
