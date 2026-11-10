#!/bin/sh
# Build Star 3D (New 3DS port) with Docker.
# Put your own GameCube disc image (.iso, PAL GSAP01) next to this file, then run ./build.sh.
# The release set is written to out/: the code-only star3d.3dsx and star3d.cia plus
# star3d-gamedata.zip (the game data for the SD card; see README.md and RELEASING.md).
# Arguments are build targets, for example:
#   ./build.sh           the release set (all)
#   ./build.sh code      code-only .3dsx (no ISO needed)
#   ./build.sh cia       only the .cia
#   ./build.sh gamedata  only the game data zip
#   ./build.sh full      .3dsx + .cia with the data embedded
#   ./build.sh help      list every target
set -e
cd "$(dirname "$0")"

docker build -t star3d-builder .
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" star3d-builder "$@"
