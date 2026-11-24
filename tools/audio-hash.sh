#!/bin/sh
# audio-hash.sh [ROOT]: 16 hex digits identifying every local input of the audio plug-in.
set -e
cd "${1:-.}"
deps() {
    for c in mac/Audio/*.c; do
        xcrun --sdk macosx clang -std=c17 -MM "$c" | tr ' \\' '\n\n' | grep -v ':$' | grep . || true
    done
}
{
    for f in $( (find mac/Audio -type f; deps) | sed 's|^\./||' | while read -r f; do python3 -I -c 'import os,sys;print(os.path.relpath(os.path.realpath(sys.argv[1])))' "$f"; done | sort -u); do
        printf '== %s\n' "$f"; cat "$f"
    done
    awk '/^  HD60SAudio:/ { f = 1; print; next } f && /^  [^ ]/ { f = 0 } f' mac/project.yml
} | shasum -a 256 | cut -c1-16
