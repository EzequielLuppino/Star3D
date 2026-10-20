#!/bin/bash
# Entry point of the Docker build image (see Dockerfile in the repository root).
# Builds the 3DS port from /workspace and writes out/star3d.3dsx.
#
# Extra arguments are passed to make, e.g. "clean" or "code".
# NO_CIA=1 in the environment (docker run -e NO_CIA=1) skips the .cia.
set -euo pipefail

WORKSPACE=/workspace
cd "$WORKSPACE/3DS"

shopt -s nullglob nocaseglob
isos=("$WORKSPACE"/*.iso)
shopt -u nullglob nocaseglob

needs_iso=true
case "${1:-}" in
    clean|code) needs_iso=false ;;
esac

if $needs_iso; then
    if [ ${#isos[@]} -eq 0 ]; then
        echo "error: no GameCube disc image (.iso) found." >&2
        echo "       Copy your .iso into the repository folder (next to the Dockerfile) and run the build again." >&2
        exit 1
    fi
    if [ ${#isos[@]} -gt 1 ]; then
        echo "error: more than one .iso in the repository folder; keep only one:" >&2
        printf '         %s\n' "${isos[@]##*/}" >&2
        exit 1
    fi
fi

ISO_ARG=()
[ ${#isos[@]} -eq 1 ] && ISO_ARG=(ISO="${isos[0]}")

make -j"$(nproc)" BUILD_ROOT=build-docker "${ISO_ARG[@]}" "$@"

if [ -f build-docker/us_3ds/star3d.3dsx ] && { [ $# -eq 0 ] || [ "$1" = "all" ]; }; then
    mkdir -p "$WORKSPACE/out"
    cp -u build-docker/us_3ds/star3d.3dsx "$WORKSPACE/out/star3d.3dsx"
    cp -u build-docker/us_3ds/star3d.elf "$WORKSPACE/out/star3d.elf"
    echo
    echo "Done: out/star3d.3dsx"
    if [ "${NO_CIA:-0}" = "1" ]; then
        # Do not leave an older .cia next to the new .3dsx
        rm -f "$WORKSPACE/out/star3d.cia"
        echo "      (.cia skipped: NO_CIA=1)"
    elif [ -f build-docker/us_3ds/star3d.cia ]; then
        cp -u build-docker/us_3ds/star3d.cia "$WORKSPACE/out/star3d.cia"
        echo "      out/star3d.cia"
    fi
    echo "3dsx: copy it to the SD card at /3ds/star3d.3dsx and start it from the Homebrew Launcher."
    echo "cia:  install it with FBI (or another title manager); it shows up on the HOME Menu."
    echo "Log and profiling data: sdmc:/3ds/star3d/Star3D.log"
    echo "(out/star3d.elf has the symbols for the crash addresses in the log.)"
fi
