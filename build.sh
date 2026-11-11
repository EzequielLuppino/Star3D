#!/bin/sh
# Build Star 3D (New 3DS port) with Docker.
# Put your own GameCube disc image (.iso, PAL GSAP01) next to this file.
# With no arguments, a menu of the usual build options is shown. Arguments
# are passed to make instead (for example: ./build.sh help, ./build.sh clean).
set -e
cd "$(dirname "$0")"

docker build -t star3d-builder .

if [ $# -gt 0 ]; then
    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" star3d-builder "$@"
    exit 0
fi

echo
echo "Star 3D build menu"
echo "=================="
echo "  1) Game data    Convert the disc into out/star3d-gamedata.zip"
echo "                  (players extract this once per SD card)"
echo "  2) .3dsx        Code-only build for the Homebrew Launcher"
echo "  3) .cia         Code-only build, install with FBI"
echo "  4) Everything   The whole release set: game data + .3dsx + .cia"
echo "  5) Clean        Remove the build outputs"
echo "  0) Exit"
echo
printf "Choice: "
read choice
case "$choice" in
    1) target=gamedata ;;
    2) target=3dsx ;;
    3) target=cia ;;
    4) target=all ;;
    5) target=clean ;;
    0) echo "Nothing built."; exit 0 ;;
    *) echo "Unknown choice: $choice" >&2; exit 1 ;;
esac
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" star3d-builder "$target"
