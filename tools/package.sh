#!/bin/sh
# Packs a release build into a tarball laid out as an install prefix: bin/noctara, the desktop file and the icon.
# Usage: tools/package.sh <binary> <version> <output directory>   (run through `xmake package`)
set -eu
binary="$1"
version="$2"
outdir="$3"
root=$(cd "$(dirname "$0")/.." && pwd)
name="noctara-$version-linux-$(uname -m)"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
install -Dm755 "$binary" "$stage/$name/bin/noctara"
install -Dm644 "$root/resources/app/noctara.desktop" "$stage/$name/share/applications/noctara.desktop"
install -Dm644 "$root/resources/app/noctara.svg" "$stage/$name/share/icons/hicolor/scalable/apps/noctara.svg"
mkdir -p "$outdir"
tar -C "$stage" -czf "$outdir/$name.tar.gz" "$name"
echo "wrote $outdir/$name.tar.gz"
