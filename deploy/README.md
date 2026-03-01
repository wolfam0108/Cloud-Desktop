# deploy/ — файлы развёртывания Cloud Desktop

Готовые скрипты, конфиги и сервисы для установки Cloud Desktop в LXC-контейнере.

Полная инструкция: [INSTALL-LXC.md](../INSTALL-LXC.md)

---

## Структура

```
deploy/
├── scripts/
│   ├── cloud-desktop.sh                 # Основной скрипт запуска (интерактивный)
│   ├── cloud-desktop-service.sh         # Автозапуск через systemd
│   ├── gamescope-resize.sh              # Смена разрешения перед стримом (Sunshine prep-cmd)
│   └── ksmserver-logout-greeter-wrapper.sh  # Wrapper для Shutdown/Reboot в KDE
│
├── systemd/
│   ├── cloud-desktop.service            # Systemd unit для автозапуска
│   └── org.kde.LogoutPrompt.service     # D-Bus service для greeter wrapper
│
├── polkit/
│   └── 50-cloud-desktop.rules           # Polkit: poweroff/reboot без пароля для user
│
├── sunshine/
│   └── apps.json                        # Конфигурация Sunshine приложений
│
└── nvidia/
    └── fix-nvidia-uvm-major.sh          # Фикс динамического NVIDIA UVM major в LXC
```

---

## Назначение файлов

### scripts/

| Файл | Куда ставить | Описание |
|---|---|---|
| `cloud-desktop.sh` | `/home/user/` | Интерактивный запуск всего стека с логированием. Поддерживает debug, ASan, gdb |
| `cloud-desktop-service.sh` | `/home/user/` | Автоматический запуск/остановка без TTY. Используется systemd unit |
| `gamescope-resize.sh` | `/home/user/` | Вызывается Sunshine перед каждым стримом — устанавливает разрешение клиента |
| `ksmserver-logout-greeter-wrapper.sh` | `/usr/local/bin/` | Проброс env vars для D-Bus activated greeter |

### systemd/

| Файл | Куда ставить | Описание |
|---|---|---|
| `cloud-desktop.service` | `/etc/systemd/system/` | Unit для автозапуска стека от пользователя `user` |
| `org.kde.LogoutPrompt.service` | `~/.local/share/dbus-1/services/` | Перенаправляет D-Bus activation greeter на wrapper |

### polkit/

| Файл | Куда ставить | Описание |
|---|---|---|
| `50-cloud-desktop.rules` | `/etc/polkit-1/rules.d/` | Разрешает `user` выполнять poweroff/reboot без пароля |

### sunshine/

| Файл | Куда ставить | Описание |
|---|---|---|
| `apps.json` | `~/.config/sunshine/` | Конфигурация Sunshine: приложение "Cloud Desktop" + prep-cmd для resize |

### nvidia/

| Файл | Куда ставить (хост Proxmox) | Описание |
|---|---|---|
| `fix-nvidia-uvm-major.sh` | хост PVE | Определяет актуальный nvidia-uvm major и обновляет LXC cgroup. Нужен при ребуте хоста |

---

## Быстрая установка

```bash
# Скрипты → /home/user/
cp deploy/scripts/cloud-desktop.sh /home/user/
cp deploy/scripts/cloud-desktop-service.sh /home/user/
cp deploy/scripts/gamescope-resize.sh /home/user/
chmod +x /home/user/cloud-desktop*.sh /home/user/gamescope-resize.sh

# Wrapper → /usr/local/bin/
cp deploy/scripts/ksmserver-logout-greeter-wrapper.sh /usr/local/bin/ksmserver-logout-greeter-wrapper
chmod +x /usr/local/bin/ksmserver-logout-greeter-wrapper

# Systemd
cp deploy/systemd/cloud-desktop.service /etc/systemd/system/
systemctl daemon-reload && systemctl enable cloud-desktop

# D-Bus service (от user)
mkdir -p /home/user/.local/share/dbus-1/services
cp deploy/systemd/org.kde.LogoutPrompt.service /home/user/.local/share/dbus-1/services/

# Polkit
cp deploy/polkit/50-cloud-desktop.rules /etc/polkit-1/rules.d/

# Sunshine config
mkdir -p /home/user/.config/sunshine
cp deploy/sunshine/apps.json /home/user/.config/sunshine/
```
