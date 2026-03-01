#!/bin/bash
# =============================================================================
# Cloud Desktop Launch Script v1.3
# Использование: ./cloud-desktop.sh [start|stop|status|log] [--asan] [--gdb]
# Sunshine интегрирован как шаг 7/7
#   --asan  Включить AddressSanitizer (требует ASan-сборку gamescope)
#   --gdb   Запускать gamescope под gdb (автоматический backtrace при crash)
# =============================================================================

# Функция добавления timestamp к каждой строке лога
# Использование: command 2>&1 | add_ts >file.log &
add_ts() {
    while IFS= read -r line; do
        printf '[%s] %s\n' "$(date '+%H:%M:%S.%3N')" "$line"
    done
}

GAMESCOPE_BIN="/usr/local/bin/gamescope"
SUNSHINE_BIN="/usr/bin/sunshine"
SUNSHINE_CONF="/home/user/.config/sunshine/sunshine.conf"

# Опции отладки (по умолчанию выключены)
USE_ASAN=false
USE_GDB=false

# Парсинг опций из всех аргументов
for arg in "$@"; do
    case "$arg" in
        --asan) USE_ASAN=true ;;
        --gdb)  USE_GDB=true ;;
    esac
done
# Стартовое (дефолтное) разрешение и частота.
# При подключении клиента Moonlight разрешение перезаписывается
# динамически через gamescope-resize.sh (Sunshine prep_cmd)
# с помощью протокола gamescope_control.set_output_mode (v7).
RESOLUTION_W=1920
RESOLUTION_H=1080
REFRESH_HZ=60

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

do_stop() {
    echo "[stop] Останавливаю стек..."
    # Sunshine первым — он держит захват
    pkill -9 -u "$(id -u)" sunshine 2>/dev/null || true
    sleep 0.5
    pkill -9 -u "$(id -u)" polkit-kde-auth 2>/dev/null || true
    pkill -9 -u "$(id -u)" kded6 2>/dev/null || true
    pkill -9 -u "$(id -u)" plasmashell 2>/dev/null || true
    pkill -9 -u "$(id -u)" kactivitymanag 2>/dev/null || true
    pkill -9 -u "$(id -u)" kwin_wayland 2>/dev/null || true
    pkill -9 -u "$(id -u)" gamescope 2>/dev/null || true
    # Аудио стек
    pkill -9 -u "$(id -u)" pipewire-pulse 2>/dev/null || true
    pkill -9 -u "$(id -u)" wireplumber 2>/dev/null || true
    pkill -9 -u "$(id -u)" pipewire 2>/dev/null || true
    systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber.service pipewire.service pipewire-pulse.service 2>/dev/null || true
    # Убить dbus-daemon от текущего пользователя
    pkill -9 -u "$(id -u)" dbus-daemon 2>/dev/null || true
    sleep 1
    rm -f "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/gamescope-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pipewire-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pulse/native 2>/dev/null || true
    # Ротация логов: .log → .01.log → .02.log ... → .05.log (макс 5)
    for base in gamescope kwin plasma kactivity pipewire wireplumber pipewire-pulse gamescope-resize sunshine; do
        local f="$LOG_DIR/${base}.log"
        [ -f "$f" ] || continue
        # Сдвигаем: .04 → .05, .03 → .04, ..., .01 → .02
        rm -f "$LOG_DIR/${base}.05.log" 2>/dev/null
        for i in 4 3 2 1; do
            local next=$((i + 1))
            [ -f "$LOG_DIR/${base}.$(printf '%02d' $i).log" ] && \
                mv "$LOG_DIR/${base}.$(printf '%02d' $i).log" "$LOG_DIR/${base}.$(printf '%02d' $next).log"
        done
        mv "$f" "$LOG_DIR/${base}.01.log"
    done
    rm -f "$LOG_DIR"/dbus_address 2>/dev/null || true
    echo "[stop] Очистка завершена"
}

do_start() {
    do_stop
    sleep 1

    echo "============================================"
    echo "  Cloud Desktop v1.3"
    echo "  ${RESOLUTION_W}x${RESOLUTION_H}@${REFRESH_HZ}Hz"
    echo "============================================"

    # === 1. D-Bus ===
    echo ""
    echo "[1/7] D-Bus..."
    eval $(dbus-launch --sh-syntax)
    export DBUS_SESSION_BUS_ADDRESS
    # Сохранить адрес для внешних процессов (Sunshine)
    echo "$DBUS_SESSION_BUS_ADDRESS" > "$LOG_DIR/dbus_address"
    echo "[1/7] ✅ $DBUS_SESSION_BUS_ADDRESS"

    # === 2. PipeWire (аудио) ===
    echo ""
    echo "[2/7] PipeWire + WirePlumber..."
    # Принудительная очистка перед запуском
    pkill -9 -u "$(id -u)" pipewire-pulse 2>/dev/null || true
    pkill -9 -u "$(id -u)" wireplumber 2>/dev/null || true
    pkill -9 -u "$(id -u)" pipewire 2>/dev/null || true
    systemctl --user stop pipewire.socket pipewire-pulse.socket wireplumber.service pipewire.service pipewire-pulse.service 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pipewire-* 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/pulse/native 2>/dev/null || true
    sleep 1

    pipewire > >(add_ts > "$LOG_DIR/pipewire.log") 2>&1 &
    PW_PID=$!
    sleep 1

    wireplumber > >(add_ts > "$LOG_DIR/wireplumber.log") 2>&1 &
    WP_PID=$!
    sleep 1

    pipewire-pulse > >(add_ts > "$LOG_DIR/pipewire-pulse.log") 2>&1 &
    PP_PID=$!
    sleep 1

    if kill -0 "$PW_PID" 2>/dev/null && kill -0 "$PP_PID" 2>/dev/null; then
        echo "[2/7] ✅ PipeWire=$PW_PID WirePlumber=$WP_PID PipeWire-Pulse=$PP_PID"
    else
        echo "[2/7] ⚠️  Аудио стек не запустился (не критично)"
    fi

    # === 3. kactivitymanagerd (offscreen) ===
    echo ""
    echo "[3/7] kactivitymanagerd..."
    QT_QPA_PLATFORM=offscreen /usr/libexec/kactivitymanagerd \
        > >(add_ts > "$LOG_DIR/kactivity.log") 2>&1 &
    KACT_PID=$!
    sleep 2
    if kill -0 "$KACT_PID" 2>/dev/null; then
        echo "[3/7] ✅ PID=$KACT_PID"
    else
        echo "[3/7] ⚠️  упал (не критично)"
    fi

    # === 4. Gamescope ===
    echo ""
    local gs_mode=""
    $USE_ASAN && gs_mode="${gs_mode}ASan "
    $USE_GDB  && gs_mode="${gs_mode}gdb "
    [ -z "$gs_mode" ] && gs_mode="native "
    echo "[4/7] Gamescope (${gs_mode%% })..."

    # Базовые env vars
    local GS_ENV=(
        GAMESCOPE_SUNSHINE_VBLANK_MODE=direct
        GAMESCOPE_SUNSHINE_DUMP_FRAME=1
        GAMESCOPE_SUNSHINE_CURSOR_TEST=1
    )

    # ASan (только если --asan)
    if $USE_ASAN; then
        GS_ENV+=( ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:log_path=$LOG_DIR/asan" )
    fi

    # Базовая команда gamescope
    local GS_CMD=(
        "$GAMESCOPE_BIN" --backend sunshine --expose-wayland
        -W "$RESOLUTION_W" -H "$RESOLUTION_H" -r "$REFRESH_HZ"
    )

    if $USE_GDB; then
        # gdb batch mode: автоматический backtrace при crash
        cat > /tmp/cloud-desktop/gdb_cmds.txt << 'GDBEOF'
set pagination off
set confirm off
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
run
set logging file /tmp/cloud-desktop/gamescope_crash.log
set logging enabled on
echo \n=== CRASH BACKTRACE ===\n
bt full
echo \n=== ALL THREADS ===\n
thread apply all bt full
echo \n=== REGISTERS ===\n
info registers
set logging enabled off
quit
GDBEOF
        env "${GS_ENV[@]}" gdb -batch -x /tmp/cloud-desktop/gdb_cmds.txt \
            --args "${GS_CMD[@]}" \
            > >(add_ts > "$LOG_DIR/gamescope.log") 2>&1 &
    else
        # Прямой запуск — максимальная производительность
        env "${GS_ENV[@]}" "${GS_CMD[@]}" \
            > >(add_ts > "$LOG_DIR/gamescope.log") 2>&1 &
    fi
    GS_PID=$!

    for i in $(seq 1 15); do
        if [ -S "$XDG_RUNTIME_DIR/gamescope-0" ]; then
            echo "[4/7] ✅ PID=$GS_PID socket=gamescope-0 (${i}с)"
            break
        fi
        if ! kill -0 "$GS_PID" 2>/dev/null; then
            echo "[4/7] ❌ Gamescope упал!"
            tail -20 "$LOG_DIR/gamescope.log"
            return 1
        fi
        sleep 1
    done

    # === 5. KWin ===
    echo ""
    echo "[5/7] KWin..."
    WAYLAND_DISPLAY=gamescope-0 \
        XDG_SESSION_TYPE=wayland \
        kwin_wayland --no-lockscreen --no-global-shortcuts --xwayland \
        > >(add_ts > "$LOG_DIR/kwin.log") 2>&1 &
    KWIN_PID=$!

    for i in $(seq 1 15); do
        if [ -S "$XDG_RUNTIME_DIR/wayland-0" ]; then
            echo "[5/7] ✅ PID=$KWIN_PID socket=wayland-0 (${i}с)"
            break
        fi
        if ! kill -0 "$KWIN_PID" 2>/dev/null; then
            echo "[5/7] ❌ KWin упал!"
            tail -15 "$LOG_DIR/kwin.log"
            return 1
        fi
        sleep 1
    done

    # Автоопределение XWayland display от KWin
    # KWin с --xwayland запускает Xwayland на следующем свободном :N
    # Нужно подождать — XWayland появляется ПОСЛЕ wayland-0
    sleep 3
    KWIN_XDISPLAY=""
    for i in $(seq 1 10); do
        for x in /tmp/.X11-unix/X*; do
            [ -S "$x" ] || continue
            xnum="${x##*X}"
            # Пропускаем :0 — это XWayland Gamescope
            [ "$xnum" = "0" ] && continue
            # Нашли X-сокет не от Gamescope — это KWin XWayland
            KWIN_XDISPLAY=":$xnum"
            break 2
        done
        sleep 1
    done
    if [ -n "$KWIN_XDISPLAY" ]; then
        echo "[5/7] ✅ KWin XWayland=$KWIN_XDISPLAY"
    else
        echo "[5/7] ⚠️  KWin XWayland не обнаружен (X11 приложения не будут работать)"
    fi

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

    # Записать env vars в файл для D-Bus activated процессов (wrapper).
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

    # === 5.5. kded6 (демон рассылки настроек KDE — без него тема не переключается) ===
    echo ""
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
        echo "[5.5/7] ⚠️  kded6 не запустился (тема может не переключаться)"
    fi

    # === 6. Plasmashell ===
    echo ""
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

    # === 7. Sunshine ===
    echo ""
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
    echo "  ✅ Cloud Desktop запущен!"
    echo "  Gamescope:  $GS_PID   KWin:    $KWIN_PID"
    echo "  Plasma:     $PLASMA_PID   Sunshine: $SUNSHINE_PID"
    echo "  PipeWire:   $PW_PID   Pulse:   $PP_PID"
    echo "  Остановка:  $0 stop"
    echo "  Логи:       $0 log [gs|kwin|plasma|sunshine|all]"
    echo "  ASan логи:  $LOG_DIR/asan.*"
    echo "============================================"

    # Foreground mode: скрипт ждёт gamescope
    trap do_stop EXIT INT TERM
    echo ""
    echo "[fg] Работаю в foreground... (Ctrl+C для остановки)"
    wait $GS_PID 2>/dev/null
    echo "[fg] Gamescope завершён"
}

do_status() {
    echo "=== Status ==="
    for proc in gamescope kwin_wayland kactivitymanag plasmashell sunshine; do
        local pid
        pid=$(pgrep -u "$(id -u)" "$proc" 2>/dev/null | head -1)
        if [ -n "$pid" ]; then
            echo "$proc: RUNNING (PID $pid)"
        else
            echo "$proc: NOT RUNNING"
        fi
    done
    echo "---"
    for sock in gamescope-0 wayland-0; do
        [ -S "$XDG_RUNTIME_DIR/$sock" ] && echo "Socket $sock: ✅" || echo "Socket $sock: ❌"
    done
    echo "---"
    echo "[gamescope sunshine-backend]:"
    grep -E "\[sunshine\]|sunshine-backend|vk-diag" "$LOG_DIR/gamescope.log" 2>/dev/null | tail -10
    echo "---"
    echo "[sunshine]:"
    tail -10 "$LOG_DIR/sunshine.log" 2>/dev/null || echo "(нет лога)"
    echo "---"
    echo "[ASan]:"
    ls -la "$LOG_DIR"/asan.* 2>/dev/null || echo "(нет ASan отчётов — хорошо!)"
    echo "---"
    echo "[frame dumps]:"
    ls -la /tmp/sunshine_frame_*.ppm 2>/dev/null || echo "(нет)"
}

do_log() {
    local target="${2:-all}"
    case "$target" in
        gs|gamescope) cat "$LOG_DIR/gamescope.log" 2>/dev/null ;;
        kwin)         cat "$LOG_DIR/kwin.log" 2>/dev/null ;;
        plasma)       cat "$LOG_DIR/plasma.log" 2>/dev/null ;;
        kact)         cat "$LOG_DIR/kactivity.log" 2>/dev/null ;;
        sun|sunshine)  cat "$LOG_DIR/sunshine.log" 2>/dev/null ;;
        asan)         cat "$LOG_DIR"/asan.* 2>/dev/null || echo "(нет ASan отчётов)" ;;
        vkdiag)       grep -E "vk-diag" "$LOG_DIR/gamescope.log" 2>/dev/null ;;
        *)
            echo "=== Gamescope (last 30) ===" && tail -30 "$LOG_DIR/gamescope.log" 2>/dev/null
            echo "" && echo "=== KWin (last 15) ===" && tail -15 "$LOG_DIR/kwin.log" 2>/dev/null
            echo "" && echo "=== Plasma (last 15) ===" && tail -15 "$LOG_DIR/plasma.log" 2>/dev/null
            echo "" && echo "=== Sunshine (last 15) ===" && tail -15 "$LOG_DIR/sunshine.log" 2>/dev/null
            echo "" && echo "=== ASan ===" && ls -la "$LOG_DIR"/asan.* 2>/dev/null || echo "(чисто)" ;;
    esac
}

case "${1:-start}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    log)    do_log "$@" ;;
    *)      echo "Использование: $0 [start|stop|status|log [gs|kwin|plasma|sunshine|asan|vkdiag]] [--asan] [--gdb]" ;;
esac
