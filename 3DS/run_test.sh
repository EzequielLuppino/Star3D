#!/bin/bash
# Local build helper (native devkitPro). Arguments are passed to make,
# e.g. "code" (no game data) or BUILD_ROOT=build-x.
cd "$(dirname "$0")"
if [ -d /c/devkitPro ]; then
    export DEVKITPRO=/c/devkitPro
else
    export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
fi
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
TMP_ARGS=()
if command -v cygpath >/dev/null 2>&1; then
    # devkitPro's Windows make strips TMP/TEMP; gcc needs a writable temp dir
    mkdir -p build/tmp
    export TMP=$(cygpath -w "$PWD/build/tmp") TEMP=$(cygpath -w "$PWD/build/tmp")
    TMP_ARGS=(TMP="$(cygpath -m "$PWD/build/tmp")" TEMP="$(cygpath -m "$PWD/build/tmp")")
fi
make -k -j"$(nproc)" "${TMP_ARGS[@]}" "$@" 2>&1
