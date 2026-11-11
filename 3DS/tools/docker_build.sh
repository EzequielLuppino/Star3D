#!/bin/bash
# Entry point of the Docker build image (see Dockerfile in the repository root).
# Builds the 3DS port from /workspace. Arguments are passed to make, e.g.
# "clean", "code", "3dsx", "cia", "gamedata", "all" ("help" lists them all);
# build.sh / build.bat pick one of these from their menu.
#
# By default (no arguments) this builds the release set: the code-only
# out/star3d.3dsx and out/star3d.cia plus out/star3d-gamedata.zip (the game
# data for the SD card, sdmc:/3ds/star3d/gamedata/romfs.bin, RELEASING.md).
# NO_CIA=1 in the environment (docker run -e NO_CIA=1) skips the .cia,
# NO_GAMEDATA=1 skips the data zip.
set -euo pipefail

WORKSPACE=/workspace
cd "$WORKSPACE/3DS"

shopt -s nullglob nocaseglob
isos=("$WORKSPACE"/*.iso)
shopt -u nullglob nocaseglob

# Only the game data targets need the disc image. The .cia builds without
# one too: the Arwing banner is made from the .iso when present, otherwise
# the Makefile silently uses the committed flat banner (tools/ci/).
needs_iso=true
case "${1:-}" in
    clean|code|3dsx|cia|help) needs_iso=false ;;
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

# Keep out/ current. make's gamedata/sd-romfs targets already write to out/
# themselves; cp -u only refreshes when the build is newer.
mkdir -p "$WORKSPACE/out"
collect() {  # $1 = path under 3DS/, $2 = name in out/
    if [ -f "$1" ]; then
        cp -u "$1" "$WORKSPACE/out/$2"
        echo "      out/$2"
    fi
}

if { [ $# -eq 0 ] || [ "$1" = "all" ]; }; then
    collect build-docker/us_3ds/star3d.3dsx star3d.3dsx
    collect build-docker/us_3ds/star3d.elf star3d.elf
    # The pre-decoupling name of the code-only image (star3d.3dsx is it now)
    rm -f "$WORKSPACE/out/star3d-nodata.3dsx"
    if [ "${NO_CIA:-0}" = "1" ]; then
        # Do not leave an older .cia next to the new .3dsx
        rm -f "$WORKSPACE/out/star3d.cia"
        echo "      (.cia skipped: NO_CIA=1)"
    else
        collect build-docker/us_3ds/star3d.cia star3d.cia
    fi
    if [ "${NO_GAMEDATA:-0}" = "1" ]; then
        rm -f "$WORKSPACE/out/star3d-gamedata.zip"
        echo "      (gamedata zip skipped: NO_GAMEDATA=1)"
    fi
fi

# The unpacked data file lands in out/ too
collect build-docker/sd/star3d-romfs.bin star3d-romfs.bin

echo
echo "Release set (make all):"
echo "  out/star3d.3dsx          code only, copy to /3ds/ on the SD card"
echo "  out/star3d.cia           code only, install with FBI"
echo "  out/star3d-gamedata.zip  extract once to the SD card's root"
echo "                           -> sdmc:/3ds/star3d/gamedata/romfs.bin"
echo "Updates: install the new .cia over the old one; the gamedata folder"
echo "stays (see RELEASING.md)."
echo "Log: sdmc:/3ds/star3d/Star3D.log"
echo "(out/star3d.elf has the symbols for the crash addresses in the log.)"
