#!/bin/bash
# =============================================================================
# Cloud Desktop Service Script v1.0
# Сервисный скрипт для systemd — автозапуск при старте контейнера.
# Не содержит интерактивных функций (log, status) — используйте cloud-desktop.sh.
# Использование: cloud-desktop-service.sh [start|stop]
# =============================================================================

# --- Конфигурация ---
GAMESCOPE_BIN="/usr/local/bin/gamescope"
SUNSHINE_BIN="/usr/bin/sunshine"
SUNSHINE_CONF="/home/user/.config/sunshine/sunshine.conf"

RESOLUTION_W=1920
RESOLUTION_H=1080
REFRESH_HZ=60

# --- Окружение ---
export HOME="/home/user"
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="/tmp/user-$(id -u)-runtime"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
fi

# KDE theming — без этих переменных Qt-приложения не подхватывают color scheme
export QT_QPA_PLATFORMTHEME=kde
export XDG_CURRENT_DESKTOP=KDE

LOG_DIR="/tmp/cloud-desktop"
mkdir -p "$LOG_DIR"

# Timestamp для логов
add_ts() {
    while IFS= read -r line; do
        printf '[%s] %s\n' "$(date '+%H:%M:%S.%3N')" "$line"
    done
}

# --- Ротация логов ---
rotate_logs() {
    for base in gamescope kwin plasma kactivity pipewire wireplumber pipewire-pulse sunshine; do
        local f="$LOG_DIR/${base}.log"
        [ -f "$f" ] || continue
        rm -f "$LOG_DIR/${base}.05.log" 2>/dev/null
        for i in 4 3 2 1; do
            local next=$((i + 1))
            [ -f "$LOG_DIR/${base}.$(printf '%02d' $i).log" ] && \
                mv "$LOG_DIR/${base}.$(printf '%02d' $i).log" "$LOG_DIR/${base}.$(printf '%02d' $next).log"
        done
        mv "$f" "$LOG_DIR/${base}.01.log"
    done
}

# --- Остановка ---
do_stop() {
    echo "[service] Останавливаю стек..."
    pkill -9 -u "$(id -u)" sunshine 2>/dev/null || true
    sleep 1
    pkill -9 -u "$(id -u)" polkit-kde-auth 2>/dev/null || true
    pkill -9 -u "$(id -u)" kded6 2>/dev/null || true
    pkill -9 -u "$(id -u)" plasmashell 2>/dev/null || true
    pkill -9 -u "$(id -u)" kactivitymanag 2>/dev/null || true
    pkill -9 -u "$(id -u)" kwin_wayland 2>/dev/null || true
    pkill -9 -u "$(id -u)" gamescope 2>/dev/null || true
    pkill -9 -u "$(id -u)" pipewire-pulse 2>/dev/null || true
    pkill -9 -u "$(id -u)" wireplumber 2>/dev/null || true
    pkill -9 -u "$(id -u)" pipewire 2>/dev/null || true
    systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber.service pipewire.service pipewire-pulse.service 2>/dev/null || true
    pkill -9 -u "$(id -u)" dbus-daemon 2>/dev/null || true
    sleep 1
    rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/gamescope-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pipewire-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pulse/native 2>/dev/null || true
    rm -f "$LOG_DIR"/dbus_address 2>/dev/null || true
    echo "[service] Очистка завершена"
}

# --- Запуск ---
do_start() {
    rotate_logs
    do_stop
    sleep 1

    echo "============================================"
    echo "  Cloud Desktop Service v1.0"
    echo "  ${RESOLUTION_W}x${RESOLUTION_H}@${REFRESH_HZ}Hz"
    echo "============================================"

    # 1. D-Bus
    echo "[1/7] D-Bus..."
    eval $(dbus-launch --sh-syntax)
    export DBUS_SESSION_BUS_ADDRESS
    echo "$DBUS_SESSION_BUS_ADDRESS" > "$LOG_DIR/dbus_address"
    echo "[1/7] ✅ $DBUS_SESSION_BUS_ADDRESS"

    # 2. PipeWire
    echo "[2/7] PipeWire..."
    rm -f "$XDG_RUNTIME_DIR"/pipewire-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pulse/native 2>/dev/null || true
    sleep 1
    pipewire > >(add_ts > "$LOG_DIR/pipewire.log") 2>&1 &
    sleep 1
    wireplumber > >(add_ts > "$LOG_DIR/wireplumber.log") 2>&1 &
    sleep 1
    pipewire-pulse > >(add_ts > "$LOG_DIR/pipewire-pulse.log") 2>&1 &
    sleep 1
    echo "[2/7] ✅ PipeWire started"

    # 3. kactivitymanagerd
    echo "[3/7] kactivitymanagerd..."
    QT_QPA_PLATFORM=offscreen /usr/libexec/kactivitymanagerd \
        > >(add_ts > "$LOG_DIR/kactivity.log") 2>&1 &
    sleep 2
    echo "[3/7] ✅ started"

    # 4. Gamescope
    echo "[4/7] Gamescope..."
    GAMESCOPE_SUNSHINE_VBLANK_MODE=direct \
    GAMESCOPE_SUNSHINE_DUMP_FRAME=1 \
    GAMESCOPE_SUNSHINE_CURSOR_TEST=1 \
    "$GAMESCOPE_BIN" --backend sunshine --expose-wayland \
        -W "$RESOLUTION_W" -H "$RESOLUTION_H" -r "$REFRESH_HZ" \
        > >(add_ts > "$LOG_DIR/gamescope.log") 2>&1 &
    GS_PID=$!

    for i in $(seq 1 15); do
        if [ -S "$XDG_RUNTIME_DIR/gamescope-0" ]; then
            echo "[4/7] ✅ PID=$GS_PID (${i}с)"
            break
        fi
        if ! kill -0 "$GS_PID" 2>/dev/null; then
            echo "[4/7] ❌ Gamescope упал!"
            tail -20 "$LOG_DIR/gamescope.log"
            return 1
        fi
        sleep 1
    done

    # 5. KWin
    echo "[5/7] KWin..."
    WAYLAND_DISPLAY=gamescope-0 \
        XDG_SESSION_TYPE=wayland \
        kwin_wayland --no-lockscreen --no-global-shortcuts --xwayland \
        > >(add_ts > "$LOG_DIR/kwin.log") 2>&1 &
    KWIN_PID=$!

    for i in $(seq 1 15); do
        if [ -S "$XDG_RUNTIME_DIR/wayland-0" ]; then
            echo "[5/7] ✅ PID=$KWIN_PID (${i}с)"
            break
        fi
        if ! kill -0 "$KWIN_PID" 2>/dev/null; then
            echo "[5/7] ❌ KWin упал!"
            tail -15 "$LOG_DIR/kwin.log"
            return 1
        fi
        sleep 1
    done

    # XWayland от KWin
    sleep 3
    KWIN_XDISPLAY=""
    for i in $(seq 1 10); do
        for x in /tmp/.X11-unix/X*; do
            [ -S "$x" ] || continue
            xnum="${x##*X}"
            [ "$xnum" = "0" ] && continue
            KWIN_XDISPLAY=":$xnum"
            break 2
        done
        sleep 1
    done
    [ -n "$KWIN_XDISPLAY" ] && echo "[5/7] ✅ XWayland=$KWIN_XDISPLAY"

    # Пробросить env в D-Bus activation environment.
    # Без этого D-Bus activated процессы (ksmserver-logout-greeter, polkit-agent)
    # не видят DISPLAY/WAYLAND_DISPLAY и крашятся при попытке shutdown/reboot.
    dbus-update-activation-environment --all \
        DISPLAY="${KWIN_XDISPLAY}" \
        WAYLAND_DISPLAY=wayland-0 \
        XDG_SESSION_TYPE=wayland \
        QT_QPA_PLATFORM=wayland \
        QT_QPA_PLATFORMTHEME=kde \
        XDG_CURRENT_DESKTOP=KDE \
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
        DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS" \
        LANG=ru_RU.UTF-8 \
        LC_ALL=ru_RU.UTF-8 \
        2>/dev/null || true
    echo "[5/7] ✅ D-Bus activation env updated"

    # Записать env vars в файл для D-Bus activated процессов.
    # dbus-launch не поддерживает UpdateActivationEnvironment,
    # поэтому ksmserver-logout-greeter читает env из этого файла через wrapper.
    cat > "$LOG_DIR/desktop_env" << ENVEOF
export DISPLAY="${KWIN_XDISPLAY}"
export WAYLAND_DISPLAY=wayland-0
export XDG_SESSION_TYPE=wayland
export QT_QPA_PLATFORM=wayland
export QT_QPA_PLATFORMTHEME=kde
export XDG_CURRENT_DESKTOP=KDE
export XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR"
export DBUS_SESSION_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
export PULSE_SERVER="unix:$XDG_RUNTIME_DIR/pulse/native"
export LANG=ru_RU.UTF-8
export LC_ALL=ru_RU.UTF-8
ENVEOF
    echo "[5/7] ✅ desktop_env written"

    # GTK button-layout: без этого GTK-приложения (браузеры) показывают только кнопку закрытия
    /usr/bin/gsettings set org.gnome.desktop.wm.preferences button-layout 'appmenu:minimize,maximize,close' 2>/dev/null || true
    mkdir -p "$HOME/.config/gtk-3.0" "$HOME/.config/gtk-4.0"
    for gtkver in gtk-3.0 gtk-4.0; do
        local ini="$HOME/.config/$gtkver/settings.ini"
        if ! grep -q 'gtk-decoration-layout' "$ini" 2>/dev/null; then
            echo -e '[Settings]\ngtk-decoration-layout=appmenu:minimize,maximize,close' > "$ini"
        fi
    done
    echo "[5/7] ✅ GTK button-layout configured"

    # 5.5. kded6 (демон рассылки настроек KDE — без него тема не переключается)
    echo "[5.5/7] kded6..."
    DISPLAY="${KWIN_XDISPLAY}" \
    WAYLAND_DISPLAY=wayland-0 \
        XDG_SESSION_TYPE=wayland \
        QT_QPA_PLATFORM=wayland \
        kded6 > >(add_ts > "$LOG_DIR/kded6.log") 2>&1 &
    KDED_PID=$!
    sleep 2
    if kill -0 "$KDED_PID" 2>/dev/null; then
        echo "[5.5/7] ✅ PID=$KDED_PID"
    else
        echo "[5.5/7] ⚠️  kded6 не запустился"
    fi

    # 6. Plasmashell
    echo "[6/7] Plasmashell..."
    DISPLAY="${KWIN_XDISPLAY}" \
    WAYLAND_DISPLAY=wayland-0 \
        XDG_SESSION_TYPE=wayland \
        QT_QPA_PLATFORM=wayland \
        plasmashell > >(add_ts > "$LOG_DIR/plasma.log") 2>&1 &
    PLASMA_PID=$!
    sleep 8
    if kill -0 "$PLASMA_PID" 2>/dev/null; then
        echo "[6/7] ✅ PID=$PLASMA_PID"
    else
        echo "[6/7] ❌ Plasmashell упал"
        tail -15 "$LOG_DIR/plasma.log"
        return 1
    fi

    # Polkit Authentication Agent
    DISPLAY="${KWIN_XDISPLAY}" \
    WAYLAND_DISPLAY=wayland-0 \
        XDG_SESSION_TYPE=wayland \
        QT_QPA_PLATFORM=wayland \
        /usr/libexec/kf6/polkit-kde-authentication-agent-1 \
        > >(add_ts > "$LOG_DIR/polkit-agent.log") 2>&1 &
    POLKIT_PID=$!
    sleep 1
    if kill -0 "$POLKIT_PID" 2>/dev/null; then
        echo "[6/7] ✅ Polkit agent PID=$POLKIT_PID"
    else
        echo "[6/7] ⚠️  Polkit agent не запустился (не критично)"
    fi

    # 7. Sunshine
    echo "[7/7] Sunshine..."
    WAYLAND_DISPLAY=gamescope-0 \
    PULSE_SERVER=unix:$XDG_RUNTIME_DIR/pulse/native \
        "$SUNSHINE_BIN" "$SUNSHINE_CONF" \
        > >(add_ts > "$LOG_DIR/sunshine.log") 2>&1 &
    SUNSHINE_PID=$!
    sleep 3
    if kill -0 "$SUNSHINE_PID" 2>/dev/null; then
        echo "[7/7] ✅ PID=$SUNSHINE_PID"
    else
        echo "[7/7] ❌ Sunshine упал!"
        tail -20 "$LOG_DIR/sunshine.log"
        return 1
    fi

    echo ""
    echo "============================================"
    echo "  ✅ Cloud Desktop Service запущен"
    echo "  Gamescope=$GS_PID KWin=$KWIN_PID Plasma=$PLASMA_PID"
    echo "  Sunshine=$SUNSHINE_PID"
    echo "============================================"

    # Foreground: ждём gamescope (systemd Type=simple)
    trap do_stop TERM INT
    wait $GS_PID 2>/dev/null
    echo "[service] Gamescope завершён"
}

case "${1:-start}" in
    start) do_start ;;
    stop)  do_stop ;;
    *)     echo "Использование: $0 [start|stop]" ;;
esac
