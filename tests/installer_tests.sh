#!/usr/bin/env bash
# Regression checks for desktop-entry rendering; no build or installation is performed.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT

prefix='/tmp/a b&c|d%e\f$g`h"i'
bash "$ROOT/tools/desktop-entry.sh" "$prefix" "$ROOT/resources/app/noctara.desktop" > "$temporary/noctara.desktop"
expected='Exec="/tmp/a b&c|d%%e\\\\f\$g\`h\"i/bin/noctara" --play %u'
actual=$(grep '^Exec=' "$temporary/noctara.desktop")
[[ $actual == "$expected" ]] || {
    printf 'Expected: %s\nActual:   %s\n' "$expected" "$actual" >&2
    exit 1
}
[[ $(grep -c '^Exec=' "$temporary/noctara.desktop") == 1 ]]
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "$temporary/noctara.desktop"
fi
