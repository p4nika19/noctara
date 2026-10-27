#!/bin/sh
# Runs qsb and fails if it prints any diagnostics (warnings are treated as errors).
qsb="$1"; shift
out=$("$qsb" "$@" 2>&1)
status=$?
if [ $status -ne 0 ] || [ -n "$out" ]; then
    printf '%s\n' "$out" >&2
    [ $status -ne 0 ] && exit $status
    echo "qsb produced diagnostics; treating as error" >&2
    exit 1
fi
