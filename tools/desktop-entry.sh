#!/usr/bin/env bash
# Renders the installed desktop entry. Arguments: installation prefix, source file.
set -euo pipefail

prefix=${1:?installation prefix required}
source_file=${2:?source desktop file required}

# Quote the executable as one Exec argument. The desktop string unescape runs before Exec parsing, so a literal
# backslash needs four in the file; the other listed characters need one. A literal percent is %%, while the source
# file's %u field code must remain untouched.
path="$prefix/bin/noctara"
path=${path//\\/\\\\\\\\}
path=${path//\"/\\\"}
path=${path//\$/\\\$}
path=${path//\`/\\\`}
path=${path//%/%%}

while IFS= read -r line || [[ -n $line ]]; do
    if [[ $line == Exec=noctara* ]]; then
        printf 'Exec="%s"%s\n' "$path" "${line#Exec=noctara}"
    else
        printf '%s\n' "$line"
    fi
done < "$source_file"
