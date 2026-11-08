#!/usr/bin/env bash
# Removes Noctara. The installer copies this script to <prefix>/lib/noctara/uninstall.sh; run it from there (the
# one-line installer offers it too when Noctara is already installed).
#
#   uninstall.sh            removes the application; asks whether to delete your settings, cache and sign-in
#   uninstall.sh --purge    also deletes them (settings, caches, saved lists, the protected-playback helper, the
#                           build sources kept for updates, and the sign-in token in your keyring)
#   uninstall.sh --keep     keeps them without asking
#   uninstall.sh --yes      does not ask (closes a running Noctara)
#   uninstall.sh --prefix DIR   if it was installed there (found automatically when run from <prefix>/lib/noctara)
set -euo pipefail

case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in ru*) RU=1 ;; *) RU=0 ;; esac
say() { if (( RU )) && [[ -n ${2:-} ]]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi; }
if [[ -t 1 ]]; then B=$'\e[1m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'; else B=; G=; Y=; R=; N=; fi
step() { printf '%s==>%s %s%s%s\n' "$G" "$N" "$B" "$(say "$@")" "$N"; }
warn() { printf '%s!!%s %s\n' "$Y" "$N" "$(say "$@")" >&2; }
die() { printf '%sxx%s %s\n' "$R" "$N" "$(say "$@")" >&2; exit 1; }

ASSUME_YES=0
PURGE=ask
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ $here == */lib/noctara ]]; then PREFIX=${here%/lib/noctara}; else PREFIX="$HOME/.local"; fi

# ask "English?" "Русский?" default -> 0 for yes; default is y or n. Reads the terminal (stdin may be a pipe).
ask() {
    local default=$3 answer hint='[y/N]'
    [[ $default == y ]] && hint='[Y/n]'
    if (( ASSUME_YES )) || ! (: < /dev/tty) 2>/dev/null; then [[ $default == y ]]; return; fi
    printf '%s %s ' "$(say "$1" "$2")" "$hint" > /dev/tty
    read -r answer < /dev/tty || answer=
    [[ -z $answer ]] && answer=$default
    [[ $answer =~ ^[YyДд] ]]
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) PREFIX="${2:?}"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --purge) PURGE=1; shift ;;
        --keep) PURGE=0; shift ;;
        -y|--yes) ASSUME_YES=1; shift ;;
        -h|--help) sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "Unknown option: $1 (see --help)" "Неизвестный параметр: $1 (см. --help)" ;;
    esac
done
PREFIX=${PREFIX%/}
LIBDIR="$PREFIX/lib/noctara"
MANIFEST="$LIBDIR/manifest"
(( EUID != 0 )) || [[ $PREFIX != "$HOME"* ]] || die "Run this as your user, not as root." "Запускайте от своего пользователя, не от root."
[[ -f $MANIFEST ]] || die "No Noctara installation found in $PREFIX." "В $PREFIX установленная Noctara не найдена."

SUDO=()
if [[ ! -w $LIBDIR ]]; then
    if command -v sudo >/dev/null; then SUDO=(sudo); elif command -v doas >/dev/null; then SUDO=(doas); else
        die "$PREFIX needs root to change, and sudo is missing." "Для изменений в $PREFIX нужны права root, а sudo нет."
    fi
fi

if pgrep -x noctara >/dev/null 2>&1; then
    if ask "Noctara is running. Close it now?" "Noctara запущена. Закрыть её сейчас?" y; then
        pkill -x noctara || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -x noctara >/dev/null || break; sleep 0.5; done
        pkill -9 -x noctara 2>/dev/null || true
    else
        die "Close Noctara first, then run this again." "Сначала закройте Noctara, затем запустите удаление снова."
    fi
fi

if [[ $PURGE == ask ]]; then
    if ask "Also delete your settings, cache, saved lists and sign-in?" \
           "Удалить также настройки, кэш, сохранённые списки и вход в аккаунт?" n; then PURGE=1; else PURGE=0; fi
fi

step "Removing Noctara from $PREFIX" "Удаление Noctara из $PREFIX"
# Only what the installer recorded, and only below the prefix.
mapfile -t files < "$MANIFEST"
for file in "${files[@]}"; do
    [[ -n $file && $file == "$PREFIX"/* && $file != *..* ]] || continue
    [[ $file == "$MANIFEST" ]] && continue
    "${SUDO[@]}" rm -f -- "$file"
done
"${SUDO[@]}" rm -f -- "$MANIFEST"
"${SUDO[@]}" rmdir -- "$LIBDIR" 2>/dev/null || true
if [[ -f $PREFIX/share/applications/mimeinfo.cache ]] && command -v update-desktop-database >/dev/null; then
    "${SUDO[@]}" update-desktop-database -q "$PREFIX/share/applications" || true
fi
if [[ -f $PREFIX/share/icons/hicolor/icon-theme.cache ]] && command -v gtk-update-icon-cache >/dev/null; then
    "${SUDO[@]}" gtk-update-icon-cache -qtf "$PREFIX/share/icons/hicolor" || true
fi

if (( PURGE )); then
    rm -rf -- "${XDG_CONFIG_HOME:-$HOME/.config}/noctara" "${XDG_CACHE_HOME:-$HOME/.cache}/noctara" \
              "${XDG_STATE_HOME:-$HOME/.local/state}/noctara" "${XDG_DATA_HOME:-$HOME/.local/share}/noctara" \
              "${XDG_CACHE_HOME:-$HOME/.cache}/noctara-source"
    # The sign-in token (and saved proxy credentials) live in the keyring under Noctara's schema.
    if command -v secret-tool >/dev/null; then secret-tool clear xdg:schema io.noctara.Secret 2>/dev/null || true; fi
    step "Noctara is removed, together with your settings, cache and sign-in." \
         "Noctara удалена вместе с настройками, кэшем и входом в аккаунт."
else
    step "Noctara is removed." "Noctara удалена."
    say "  Your settings and cache are kept (~/.config/noctara, ~/.cache/noctara, ~/.local/share/noctara); run with --purge to delete them." \
        "  Настройки и кэш сохранены (~/.config/noctara, ~/.cache/noctara, ~/.local/share/noctara); чтобы удалить их, запустите с --purge."
fi
