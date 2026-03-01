#!/bin/bash
# Wrapper для ksmserver-logout-greeter
# D-Bus activation через dbus-launch не передаёт env vars потомкам,
# поэтому читаем DISPLAY/WAYLAND из файла, созданного cloud-desktop скриптом.
ENV_FILE="/tmp/cloud-desktop/desktop_env"
if [ -f "$ENV_FILE" ]; then
    set -a
    source "$ENV_FILE"
    set +a
fi
exec /usr/libexec/ksmserver-logout-greeter "$@"
