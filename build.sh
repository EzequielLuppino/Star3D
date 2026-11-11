#!/bin/sh
# Build Star 3D (New 3DS port) with Docker.
# Put your own GameCube disc image (.iso, PAL GSAP01) next to this file.
#
# Double-clicking this script in a file manager reopens it in a terminal
# window with a menu of the usual build options. Running it from a terminal
# shows the same menu; arguments are passed to make instead (for example:
# ./build.sh help, ./build.sh clean).
#
# Only "Game data" and "Everything" need the .iso. The .cia uses the disc's
# Arwing banner when an .iso is present and falls back to the flat committed
# banner (tools/ci/) otherwise.
set -e
cd "$(dirname "$0")"

# Double-clicked from a file manager: reopen in a terminal window so the
# menu stays usable. Piped runs (scripting) and headless sessions are
# left alone.
if [ ! -t 0 ] && [ ! -p /dev/stdin ] && { [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; }; then
    S3D_IN_TERMINAL=1
    export S3D_IN_TERMINAL
    if [ "$(uname)" = "Darwin" ] && command -v osascript >/dev/null 2>&1; then
        exec osascript -e "tell application \"Terminal\" to do script \"S3D_IN_TERMINAL=1 '$PWD/build.sh' $*\""
    elif command -v gnome-terminal >/dev/null 2>&1; then
        exec gnome-terminal -- bash "$0" "$@"
    elif command -v konsole >/dev/null 2>&1; then
        exec konsole -e bash "$0" "$@"
    elif command -v xfce4-terminal >/dev/null 2>&1; then
        exec xfce4-terminal -e bash "$0" "$@"
    elif command -v kitty >/dev/null 2>&1; then
        exec kitty bash "$0" "$@"
    elif command -v xterm >/dev/null 2>&1; then
        exec xterm -e bash "$0" "$@"
    else
        echo "error: no terminal emulator found to show the menu; run this script from a terminal." >&2
        exit 1
    fi
fi

run() {
    docker build -t star3d-builder .

    if [ $# -gt 0 ]; then
        docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" star3d-builder "$@"
        return
    fi

    echo
    echo "Star 3D build menu"
    echo "=================="
    echo "  1) Game data    Convert the disc into out/star3d-gamedata.zip (needs the .iso)"
    echo "                  (players extract this once per SD card)"
    echo "  2) .3dsx        Code-only build for the Homebrew Launcher"
    echo "  3) .cia         Code-only build, install with FBI. With an .iso present"
    echo "                  the banner gets the Arwing; without one the flat banner"
    echo "                  is used."
    echo "  4) Everything   The whole release set: game data + .3dsx + .cia (needs the .iso)"
    echo "  5) Clean        Remove the build outputs"
    echo "  0) Exit"
    echo
    printf "Choice: "
    choice=0
    read choice || choice=0
    case "$choice" in
        1) target=gamedata ;;
        2) target=3dsx ;;
        3) target=cia ;;
        4) target=all ;;
        5) target=clean ;;
        0) echo "Nothing built."; return 0 ;;
        *) echo "Unknown choice: $choice" >&2; return 1 ;;
    esac
    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/workspace" star3d-builder "$target"
}

status=0
run "$@" || status=$?
if [ -n "${S3D_IN_TERMINAL:-}" ]; then
    echo
    printf "Press Enter to close this window..."
    read -r _ || true
fi
exit $status
