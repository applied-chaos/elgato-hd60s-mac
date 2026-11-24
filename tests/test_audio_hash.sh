#!/bin/sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
mkdir -p "$t/mac/Audio" "$t/src"
cp "$root/mac/Audio/"* "$t/mac/Audio/"
cp "$root/mac/project.yml" "$t/mac/"
h() { "$root/tools/audio-hash.sh" "$t"; }
fail() { echo "FAIL: $1" >&2; exit 1; }
base=$(h)
[ ${#base} = 16 ] || fail "16 hex digits ($base)"
[ "$(h)" = "$base" ] || fail "deterministic"
echo '#define HD60S_X 1' > "$t/src/shared.h"
printf '#include "../../src/shared.h"\n' | cat - "$root/mac/Audio/HD60SAudio.c" > "$t/mac/Audio/HD60SAudio.c"
inc=$(h); [ "$inc" != "$base" ] || fail "new include changes it"
echo '#define HD60S_X 2' > "$t/src/shared.h"
[ "$(h)" != "$inc" ] || fail "included header outside mac/Audio changes it"
cp "$root/mac/Audio/HD60SAudio.c" "$t/mac/Audio/"
sed -i '' 's/OTHER_CFLAGS: \[-std=c17\]/OTHER_CFLAGS: [-std=c17, -O3]/' "$t/mac/project.yml"
[ "$(h)" != "$base" ] || fail "plug-in build setting changes it"
cp "$root/mac/project.yml" "$t/mac/"
sed -i '' 's/PRODUCT_BUNDLE_IDENTIFIER: net.mintopia.hd60s.camera$/PRODUCT_BUNDLE_IDENTIFIER: net.mintopia.hd60s.cameraX/' "$t/mac/project.yml"
grep -q cameraX "$t/mac/project.yml" || fail "fixture edit applied"
[ "$(h)" = "$base" ] || fail "another target's settings leave it"
cp "$root/mac/project.yml" "$t/mac/"
echo '#define HD60S_LOCAL 1' > "$t/mac/Audio/local.h"
[ "$(h)" != "$base" ] || fail "new file in mac/Audio changes it"
echo "audio hash tests passed"
