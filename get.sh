#!/usr/bin/env bash
# One-line installer for Noctara:
#
#   curl -fsSL https://raw.githubusercontent.com/p4nika19/noctara/main/get.sh | bash
#
# Fetches the sources into ~/.cache/noctara-source, installs the build dependencies with your package manager
# (after asking), builds Noctara and installs it for your user into ~/.local. When Noctara is already installed it
# asks whether to update it, remove it or leave. Options go after "bash -s --", for example:
#
#   … | bash -s -- --system       install for every user into /usr/local
#   … | bash -s -- --update       update without asking        … | bash -s -- --uninstall [--purge]
#   … | bash -s -- --yes          answer yes to every question (packages, closing a running Noctara)
#
# NOCTARA_REPO_URL and NOCTARA_REF choose another repository or branch (a fork, or a local path for testing).
set -euo pipefail

REPO_URL=${NOCTARA_REPO_URL:-https://github.com/p4nika19/noctara.git}
REF=${NOCTARA_REF:-main}
SOURCE=${NOCTARA_SOURCE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/noctara-source}

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) RU=1 ;; *) RU=0 ;; esac
say() { if (( RU )) && [[ -n ${2:-} ]]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi; }
if [[ -t 1 ]]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B=; G=; Y=; R=; N=; fi
step() { printf '%s==>%s %s%s%s\n' "$G" "$N" "$B" "$(say "$@")" "$N"; }
warn() { printf '%s!!%s %s\n' "$Y" "$N" "$(say "$@")" >&2; }
die() { printf '%sxx%s %s\n' "$R" "$N" "$(say "$@")" >&2; exit 1; }

ACTION=
ASSUME_YES=0
PREFIX=
INSTALL_ARGS=()
UNINSTALL_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --update|--install) ACTION=install; shift ;;
        --uninstall|--remove) ACTION=uninstall; shift ;;
        --purge|--keep) UNINSTALL_ARGS+=("$1"); shift ;;
        --system) PREFIX=/usr/local; INSTALL_ARGS+=(--system); shift ;;
        --prefix) PREFIX="${2:?}"; INSTALL_ARGS+=(--prefix "$2"); shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; INSTALL_ARGS+=("$1"); shift ;;
        -y|--yes) ASSUME_YES=1; INSTALL_ARGS+=(--yes); UNINSTALL_ARGS+=(--yes); shift ;;
        --no-deps|--jobs=*) INSTALL_ARGS+=("$1"); shift ;;
        --jobs) INSTALL_ARGS+=(--jobs "${2:?}"); shift 2 ;;
        -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]:-$0}" 2>/dev/null | sed 's/^# \{0,1\}//' \
                       || echo "See README.md: Install."; exit 0 ;;
        *) die "Unknown option: $1" "Неизвестный параметр: $1" ;;
    esac
done
(( EUID != 0 )) || die "Run this as your user (without sudo): sudo is asked for when it is needed." \
                       "Запускайте от своего пользователя (без sudo): sudo будет запрошен, когда понадобится."

# --- An existing installation ---------------------------------------------------------------------------------------

installed=
for prefix in ${PREFIX:+"$PREFIX"} "$HOME/.local" /usr/local /usr; do
    [[ -f $prefix/lib/noctara/manifest ]] && { installed=$prefix; break; }
done

if [[ -n $installed && -z $ACTION ]]; then
    revision=$(sed -n 's/^revision=//p' "$installed/lib/noctara/version" 2>/dev/null || true)
    step "Noctara is already installed in $installed${revision:+ ($revision)}." \
         "Noctara уже установлена в $installed${revision:+ ($revision)}."
    if (( ASSUME_YES )) || ! (: < /dev/tty) 2>/dev/null; then
        die "Choose with --update or --uninstall." "Выберите действие: --update или --uninstall."
    fi
    say "  1) Update to the latest version" "  1) Обновить до последней версии" > /dev/tty
    say "  2) Remove Noctara" "  2) Удалить Noctara" > /dev/tty
    say "  3) Leave the installer" "  3) Выйти из установщика" > /dev/tty
    while :; do
        printf '%s ' "$(say "Your choice [1-3]:" "Ваш выбор [1-3]:")" > /dev/tty
        read -r choice < /dev/tty || choice=3
        case "$choice" in
            1) ACTION=install; break ;;
            2) ACTION=uninstall; break ;;
            3|q|Q|"") say "Nothing changed." "Ничего не изменено."; exit 0 ;;
        esac
    done
fi

if [[ $ACTION == uninstall ]]; then
    [[ -n $installed ]] || die "Noctara is not installed." "Noctara не установлена."
    exec bash "$installed/lib/noctara/uninstall.sh" --prefix "$installed" "${UNINSTALL_ARGS[@]}"
fi
# Updating keeps the prefix it was installed into.
if [[ -n $installed && -z $PREFIX ]]; then INSTALL_ARGS+=(--prefix "$installed"); fi

# --- Sources --------------------------------------------------------------------------------------------------------

if ! command -v git >/dev/null; then
    step "git is needed to fetch the sources." "Для загрузки исходников нужен git."
    sudo=(); command -v sudo >/dev/null && sudo=(sudo) || { command -v doas >/dev/null && sudo=(doas); }
    for pm in "pacman -S --needed --noconfirm" "dnf install -y" "zypper --non-interactive install" \
              "apt-get install -y" "xbps-install -Sy" "apk add"; do
        command -v "${pm%% *}" >/dev/null || continue
        if (( ASSUME_YES )) || { (: < /dev/tty) 2>/dev/null && printf '%s [Y/n] ' "$(say "Install git with: $pm git?" "Установить git командой: $pm git?")" > /dev/tty \
                                   && read -r answer < /dev/tty && [[ -z $answer || $answer =~ ^[YyДд] ]]; }; then
            # shellcheck disable=SC2086 # the command is a word list on purpose
            "${sudo[@]}" ${pm} git
        fi
        break
    done
    command -v git >/dev/null || die "Please install git, then run the installer again." \
                                     "Установите git и запустите установщик снова."
fi

step "Fetching Noctara ($REF)" "Загрузка Noctara ($REF)"
if [[ -d $SOURCE/.git && $(git -C "$SOURCE" config --get remote.origin.url 2>/dev/null) == "$REPO_URL" ]]; then
    # Kept between updates (with its build cache, so an update only rebuilds what changed).
    git -C "$SOURCE" fetch --quiet --depth 1 origin "$REF"
    git -C "$SOURCE" checkout --quiet --force FETCH_HEAD
    git -C "$SOURCE" clean --quiet -fdx -e build -e .xmake
else
    if [[ -e $SOURCE ]]; then
        [[ -d $SOURCE/.git || -z $(ls -A "$SOURCE" 2>/dev/null) ]] \
            || die "$SOURCE exists and is not a Noctara checkout; move it away or set NOCTARA_SOURCE_DIR." \
                   "$SOURCE существует и это не исходники Noctara; уберите его или задайте NOCTARA_SOURCE_DIR."
        rm -rf -- "$SOURCE"
    fi
    mkdir -p "$(dirname "$SOURCE")"
    git clone --quiet --depth 1 --branch "$REF" "$REPO_URL" "$SOURCE" \
        || die "Could not fetch $REPO_URL." "Не удалось загрузить $REPO_URL."
fi

exec bash "$SOURCE/install.sh" "${INSTALL_ARGS[@]}"
