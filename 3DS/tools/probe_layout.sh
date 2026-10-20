#!/bin/bash
# Find struct layout mismatches between this ARM32 build and the retail
# GameCube layout. foxhollow's STATIC_ASSERTs (mostly retail offsets, some the
# 64-bit PC layout) are a no-op outside MWCC; this compiles every game TU (with
# patches/include applied) with them mapped to _Static_assert and collects the failures.
#
#   bash tools/probe_layout.sh [jobs]
#
# Results: build/layout/summary.txt (failing assertion, count), logs in
# build/layout/logs. Structs listed in tools/layout_pc_only.txt were checked
# against SFA-Decomp and are fine; anything else needs fixing in patches/.
cd "$(dirname "$0")/.." || exit 1
FOX=../foxhollow
OUT=build/layout
JOBS=${1:-16}
rm -rf $OUT; mkdir -p $OUT/logs build/tmp
cp -r $FOX/game/include $OUT/include
for s in $(cd patches/include && find . -name '*.h.sed'); do
    h=${s%.sed}
    tr -d '\r' < $FOX/game/include/$h | sed -f patches/include/$s > $OUT/include/$h
done
sed -i 's|^#define STATIC_ASSERT(\.\.\.)\r\?$|#define STATIC_ASSERT(c) _Static_assert(c, #c)|' $OUT/include/global.h
grep -q '_Static_assert' $OUT/include/global.h || { echo "global.h: STATIC_ASSERT not found" >&2; exit 1; }

export TMP='C:\opencode\3DS\build\tmp' TEMP='C:\opencode\3DS\build\tmp'
export CC=${CC:-/c/devkitPro/devkitARM/bin/arm-none-eabi-gcc} FOX OUT
export CFLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -std=gnu11 -fsigned-char -fcommon -w \
 -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
 -Wno-error=int-conversion -Wno-error=return-mismatch \
 -DAURORA -DTARGET_PC -D__3DS__ -D_3DS -DARM11 -DTARGET_N3DS -DBUILD_VERSION=0 -DVERSION_GSAE01 -DNDEBUG=1 \
 -DMUSY_TARGET=MUSY_TARGET_PC -DMUSY_VERSION_MAJOR=2 -DMUSY_VERSION_MINOR=0 -DMUSY_VERSION_PATCH=0 \
 -include $FOX/port/include/foxhollow_compat.h \
 -I$FOX/port/include -I$FOX/extern/aurora/include -I$OUT/include -I$FOX/game \
 -I$FOX/extern/musyx/include -I${DEVKITPRO:-/c/devkitPro}/libctru/include"

probe_one() {
    src="$1"
    f=$FOX/game/src/$src
    for p in patches/$src.sed; do
        [ -f "$p" ] && { tr -d '\r' < "$f" | sed -f "$p" > "$OUT/logs/patched_$(basename "$src")"; f="$OUT/logs/patched_$(basename "$src")"; }
    done
    log="$OUT/logs/$(echo "$src" | tr '/' '_').log"
    $CC $CFLAGS -I$(dirname $FOX/game/src/$src) -fsyntax-only "$f" > "$log" 2>&1
    grep -q 'static assertion' "$log" || rm -f "$log"
}
export -f probe_one

grep -v '^ *#' $FOX/port/compiling.txt | grep -v '^ *$' | tr -d '\r' |
    xargs -P "$JOBS" -I{} bash -c 'probe_one "$@"' _ {}
cat $OUT/logs/*.log 2>/dev/null | grep 'static assertion failed' | sed -E 's/.*failed: //' |
    sort | uniq -c | sort -k2 > $OUT/summary.txt
sed -E 's/^ *[0-9]+ "(offsetof|sizeof)\(([A-Za-z0-9_]+).*/\2/' $OUT/summary.txt | sort | uniq -c | sort -rn > $OUT/by_struct.txt
grep -v '^ *#' tools/layout_pc_only.txt | tr -d '\r' | grep -v '^ *$' > $OUT/reviewed.txt
echo "$(wc -l < $OUT/summary.txt) failing assertions"
echo "reviewed (tools/layout_pc_only.txt): $(grep -cwFf $OUT/reviewed.txt $OUT/by_struct.txt) structs"
echo "unreviewed:"
grep -vwFf $OUT/reviewed.txt $OUT/by_struct.txt || echo "  none"
