#!/bin/bash
# ==============================================================================
# Auto-detect NVIDIA UVM major number для LXC контейнера
# Проблема: nvidia-uvm major number динамический, меняется при ребуте хоста
# Решение: скрипт читает актуальный major и обновляет LXC конфиг
#
# Использование:
#   ./fix-nvidia-uvm-major.sh 120      # для контейнера 120
#   ./fix-nvidia-uvm-major.sh 120 --apply  # применить + перезапустить
# ==============================================================================

set -euo pipefail

CTID="${1:-}"
APPLY="${2:-}"

if [ -z "$CTID" ]; then
    echo "Использование: $0 <CTID> [--apply]"
    exit 1
fi

CONF="/etc/pve/lxc/${CTID}.conf"
if [ ! -f "$CONF" ]; then
    echo "❌ Конфиг не найден: $CONF"
    exit 1
fi

# Определяем актуальный major number nvidia-uvm
if [ ! -c /dev/nvidia-uvm ]; then
    echo "❌ /dev/nvidia-uvm не найден"
    exit 1
fi

ACTUAL_MAJOR=$(stat -c '%t' /dev/nvidia-uvm)
ACTUAL_MAJOR_DEC=$((16#$ACTUAL_MAJOR))

echo "=== NVIDIA UVM Major Number ==="
echo "Актуальный: ${ACTUAL_MAJOR_DEC} (0x${ACTUAL_MAJOR})"

# Ищем текущие cgroup строки с nvidia-uvm-like majors (не 195, не 226, не 235, не 13)
# nvidia-uvm обычно имеет major > 200 и != 195 (nvidia) != 226 (drm) != 235 (caps)
KNOWN_MAJORS="195|226|235|13|116|10"
CURRENT=$(grep '^lxc.cgroup2.devices.allow: c [0-9]' "$CONF" | \
    grep -vE "c ($KNOWN_MAJORS):" || true)

echo ""
echo "Текущие UVM-подобные строки в $CONF:"
if [ -n "$CURRENT" ]; then
    echo "$CURRENT"
else
    echo "(не найдены)"
fi

# Проверяем, есть ли уже правильный major
if grep -q "lxc.cgroup2.devices.allow: c ${ACTUAL_MAJOR_DEC}:" "$CONF" 2>/dev/null; then
    echo ""
    echo "✅ Major ${ACTUAL_MAJOR_DEC} уже прописан — ничего менять не нужно"
    exit 0
fi

echo ""
echo "⚠️  Major ${ACTUAL_MAJOR_DEC} НЕ прописан в cgroup!"

if [ "$APPLY" = "--apply" ]; then
    echo ""
    echo "Применяю изменения..."

    # Удаляем старые UVM строки (всё что не в known majors)
    # и добавляем новую с правильным major
    TMPCONF=$(mktemp)
    grep -vE "^lxc.cgroup2.devices.allow: c [0-9]" "$CONF" > "$TMPCONF" || true

    # Возвращаем известные majors
    grep "^lxc.cgroup2.devices.allow:" "$CONF" | \
        grep -E "c ($KNOWN_MAJORS):" >> "$TMPCONF" || true

    # Добавляем актуальный UVM major
    echo "lxc.cgroup2.devices.allow: c ${ACTUAL_MAJOR_DEC}:* rwm" >> "$TMPCONF"

    cp "$TMPCONF" "$CONF"
    rm "$TMPCONF"

    echo "✅ Конфиг обновлён: c ${ACTUAL_MAJOR_DEC}:* rwm"
    echo ""
    echo "Для применения перезапустите контейнер:"
    echo "  pct stop ${CTID} && pct start ${CTID}"
else
    echo ""
    echo "Для автоматического применения:"
    echo "  $0 $CTID --apply"
    echo ""
    echo "Или вручную замените в $CONF:"
    echo "  lxc.cgroup2.devices.allow: c ${ACTUAL_MAJOR_DEC}:* rwm"
fi
