#!/bin/bash
# Linux Azahar test driver for Star 3D (equivalent of tools/run_emu.ps1 +
# tools/drive.ps1 on Windows). Usage:
#
#   tools/emu_linux.sh [-Seconds N] [-Script steps.txt] [-Keep]
#
# Launches the .3dsx in the Azahar flatpak, drives it with remote commands
# (cmd.txt, port/remote_3ds.c), takes screenshots with ImageMagick's import
# and copies the game log. The game log is Star3D.log under the flatpak's
# sdmc. Screenshots go to build/emu/.
set -u
cd "$(dirname "$0")/.."

SECONDS_RUN=15
SCRIPT=""
KEEP=0
ROM=""
while [ $# -gt 0 ]; do
    case "$1" in
        -Seconds) SECONDS_RUN=$2; shift 2 ;;
        -Script) SCRIPT=$2; shift 2 ;;
        -Rom) ROM=$2; shift 2 ;;
        -Keep) KEEP=1; shift ;;
        *) echo "unknown argument $1"; exit 2 ;;
    esac
done
[ -z "$ROM" ] && ROM=build/us_3ds/star3d.3dsx
[ -f "$ROM" ] || ROM=build-docker/us_3ds/star3d.3dsx
[ -f "$ROM" ] || ROM=../out/star3d.3dsx
[ -f "$ROM" ] || { echo "ROM not found (build first)"; exit 1; }
ROM="$(readlink -f "$ROM")"

AZ_DATA="$HOME/.var/app/org.azahar_emu.Azahar/data/azahar-emu"
GAME_LOG="$AZ_DATA/sdmc/3ds/star3d/Star3D.log"
CMD_FILE="$AZ_DATA/sdmc/3ds/star3d/cmd.txt"
OUT_DIR="build/emu"
mkdir -p "$OUT_DIR"

# Clean state
pkill -f azahar 2>/dev/null; sleep 0.5
rm -f "$GAME_LOG" "$CMD_FILE"

flatpak run org.azahar_emu.Azahar "$ROM" >"$OUT_DIR/azahar_log.txt" 2>&1 &
EMU_PID=$!
sleep 3

WIN=$(wmctrl -l | grep -i azahar | awk '{print $1}' | head -1)
echo "window: $WIN"
if [ -z "$WIN" ]; then
    # Wayland: try xdotool search
    WIN=$(xdotool search --name -i azahar 2>/dev/null | head -1)
fi
[ -n "$WIN" ] && xdotool windowactivate "$WIN" 2>/dev/null

deadline=$((SECONDS_RUN))
elapsed=0
step=1
while [ $elapsed -lt $deadline ]; do
    if [ -n "$SCRIPT" ] && [ $((elapsed % 1)) -eq 0 ]; then
        # Remote commands: send a batch, wait for the game to pick it up
        if [ -f "$SCRIPT" ] && [ ! -f "$CMD_FILE" ] && [ ! -s "$CMD_FILE.sent" ]; then
            cp "$SCRIPT" "$CMD_FILE"
        fi
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

if [ -n "$WIN" ]; then
    # X11 capture via ImageMagick, else KDE spectacle (Wayland)
    import -window "$WIN" "$OUT_DIR/screenshot.png" 2>/dev/null \
        || xdotool windowactivate --sync "$WIN" 2>/dev/null; spectacle -b -n -a -o "$OUT_DIR/screenshot.png" 2>/dev/null
fi

if [ $KEEP -eq 0 ]; then
    pkill -f azahar 2>/dev/null
    sleep 1
fi
[ -f "$GAME_LOG" ] && cp "$GAME_LOG" "$OUT_DIR/game_log.txt"
echo "done: $OUT_DIR (screenshot.png, game_log.txt, azahar_log.txt)"
