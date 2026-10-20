#!/bin/sh
# Build Star 3D (New 3DS port) with Docker.
# Put your own GameCube disc image (.iso, PAL GSAP01) next to this file, then run ./build.sh.
# The result is written to out/star3d.3dsx. Arguments are passed to make
# (for example: ./build.sh clean).
set -e
cd "$(dirname "$0")"

docker build -t star3d-builder .
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" star3d-builder "$@"
