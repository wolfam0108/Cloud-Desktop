#!/bin/bash
# gamescope-resize.sh — Sunshine prep_cmd скрипт
# Вызывается Sunshine перед каждым стримом.

LOG="/tmp/cloud-desktop/gamescope-resize.log"

W="${SUNSHINE_CLIENT_WIDTH:-1920}"
H="${SUNSHINE_CLIENT_HEIGHT:-1080}"
FPS="${SUNSHINE_CLIENT_FPS:-60}"

# Полный путь к gamescopectl
GAMESCOPECTL="/home/user/gamescope/build/src/gamescopectl"

# XDG_RUNTIME_DIR нужен для wl_display_connect()
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export GAMESCOPE_WAYLAND_DISPLAY="${GAMESCOPE_WAYLAND_DISPLAY:-gamescope-0}"

{
    echo "=== $(date) ==="
    echo "Client: ${W}x${H}@${FPS}fps"
    echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR}"
    echo "GAMESCOPE_WAYLAND_DISPLAY=${GAMESCOPE_WAYLAND_DISPLAY}"
    echo "Socket: $(ls -la ${XDG_RUNTIME_DIR}/${GAMESCOPE_WAYLAND_DISPLAY} 2>&1)"
    echo "Env dump:"
    env | grep -iE 'SUNSHINE|GAMESCOPE|WAYLAND|XDG' | sort

    if [ ! -x "$GAMESCOPECTL" ]; then
        echo "ERROR: gamescopectl not found at $GAMESCOPECTL"
        exit 1
    fi

    if [ ! -S "${XDG_RUNTIME_DIR}/${GAMESCOPE_WAYLAND_DISPLAY}" ]; then
        echo "ERROR: socket not found"
        exit 1
    fi

    echo "Calling: $GAMESCOPECTL set-mode $W $H $FPS"
    "$GAMESCOPECTL" set-mode "$W" "$H" "$FPS" 2>&1
    RC=$?
    echo "gamescopectl exit code: $RC"
    echo "=== done ==="
} >> "$LOG" 2>&1
