# Установка Cloud Desktop в LXC-контейнере Proxmox

Полное руководство по созданию облачного рабочего стола (Cloud Desktop) на базе **Gamescope + KWin + Plasma + Sunshine** внутри LXC-контейнера Proxmox с пробросом NVIDIA GPU.

> [!NOTE]
> Руководство проверено на: **Proxmox VE 8.x**, ядро **6.17**, драйвер **NVIDIA 580.126.09**, **Fedora 43**, GPU **RTX 3090**.
> Адаптируйте пути устройств и версии под вашу конфигурацию.

---

## Содержание

0. [Подготовка хоста (Proxmox)](#0-подготовка-хоста-proxmox)
1. [Требования](#1-требования)
2. [Создание LXC-контейнера](#2-создание-lxc-контейнера)
3. [Настройка конфигурации LXC](#3-настройка-конфигурации-lxc)
4. [Подготовка контейнера](#4-подготовка-контейнера)
5. [Установка NVIDIA драйвера](#5-установка-nvidia-драйвера)
6. [Патчи NvFBC и NVENC](#6-патчи-nvfbc-и-nvenc)
7. [Сборка и установка компонентов](#7-сборка-и-установка-компонентов)
8. [Установка KDE Plasma](#8-установка-kde-plasma)
9. [Настройка скриптов и конфигов](#9-настройка-скриптов-и-конфигов)
10. [Настройка Sunshine](#10-настройка-sunshine)
11. [Shutdown/Reboot через интерфейс KDE](#11-shutdownreboot-через-интерфейс-kde)
12. [Настройка мыши — KCM Cloud Mouse (опционально)](#12-настройка-мыши--kcm-cloud-mouse-опционально)
13. [Systemd-сервис и автозапуск](#13-systemd-сервис-и-автозапуск)
14. [Подключение через Moonlight](#14-подключение-через-moonlight)
15. [Решение проблем](#15-решение-проблем)

---

## 0. Подготовка хоста (Proxmox)

Все команды в этом разделе выполняются **на хосте Proxmox**, а не внутри контейнера.

### 0.1 NVIDIA драйвер на хосте

На хосте должен быть установлен NVIDIA драйвер с загруженными модулями ядра. Проверка:

```bash
nvidia-smi                      # GPU видна и драйвер работает
lsmod | grep nvidia              # модули загружены
```

Должны быть загружены модули:

| Модуль | Назначение |
|---|---|
| `nvidia` | Основной драйвер GPU |
| `nvidia_modeset` | Моду переключения режимов (mode setting) |
| `nvidia_drm` | DRM интеграция (DRI, render nodes) |
| `nvidia_uvm` | Unified Virtual Memory (для CUDA) |

### 0.2 nvidia-drm modeset=1

Модуль `nvidia-drm` должен быть загружен с параметром `modeset=1`. Без этого контейнер не получит доступ к render node (`/dev/dri/renderD128`).

```bash
# Проверить текущий параметр
cat /sys/module/nvidia_drm/parameters/modeset
# Должно быть: Y
```

Если `N` — создайте конфиг:

```bash
cat > /etc/modprobe.d/nvidia-drm.conf << 'EOF'
options nvidia-drm modeset=1
EOF

# Перестроить initramfs и перезагрузить хост
update-initramfs -u
reboot
```

### 0.3 Blacklist nouveau

Убедитесь, что `nouveau` заблокирован:

```bash
cat > /etc/modprobe.d/blacklist-nouveau.conf << 'EOF'
blacklist nouveau
options nouveau modeset=0
EOF
```

### 0.4 Определение major-номеров устройств

Major-номера устройств **отличаются на разных системах**. Определите ваши:

```bash
cat /proc/devices | grep -E "nvidia|drm|input|alsa|snd"
```

Типичный вывод:

```
 13 input
116 alsa
195 nvidia / nvidia-modeset / nvidiactl
226 drm
235 nvidia-caps
508 nvidia-uvm          # ← этот номер ДИНАМИЧЕСКИЙ!
```

> [!CAUTION]
> Major-номер `nvidia-uvm` **меняется при каждой перезагрузке хоста**. Используйте скрипт `deploy/nvidia/fix-nvidia-uvm-major.sh` для автоматического обновления LXC-конфига после ребута.

### 0.5 Fedora LXC: разблокировка capabilities

Proxmox'овский шаблон Fedora содержит файл `/usr/share/lxc/config/fedora.common.conf`, который по умолчанию блокирует ряд Linux capabilities:

```
lxc.cap.drop = setfcap sys_nice sys_pacct sys_rawio
```

Для Cloud Desktop необходимо разблокировать **`setfcap`** и **`sys_nice`**:

| Capability | Зачем нужна |
|---|---|
| `setfcap` | Установка file capabilities при `dnf install` (без неё RPM-транзакции падают) |
| `sys_nice` | KWin использует `nice`/`ionice` для приоритизации рендеринга |

```bash
# Бэкап
cp /usr/share/lxc/config/fedora.common.conf \
   /usr/share/lxc/config/fedora.common.conf.bak

# Убрать setfcap и sys_nice из drop-списка
sed -i 's/lxc.cap.drop = setfcap sys_nice sys_pacct sys_rawio/lxc.cap.drop = sys_pacct sys_rawio/' \
  /usr/share/lxc/config/fedora.common.conf

# Проверить результат
grep "lxc.cap.drop" /usr/share/lxc/config/fedora.common.conf
# Должно быть: lxc.cap.drop = sys_pacct sys_rawio
```

> [!WARNING]
> Это изменение влияет на **все Fedora-контейнеры** на данном хосте.
> Если контейнер уже запущен — перезапустите его: `pct stop <CTID> && pct start <CTID>`

**Проверка внутри контейнера:**

```bash
pct exec <CTID> -- bash -c '
  touch /tmp/testcap
  setcap cap_net_admin+ep /tmp/testcap 2>&1 && echo "setcap OK" || echo "setcap FAIL"
  rm -f /tmp/testcap
'
```

> [!TIP]
> Альтернативный workaround без изменения глобального конфига: `dnf install --setopt=tsflags=nocaps <packages>`. Но при этом file capabilities не устанавливаются — некоторые программы могут работать некорректно.

---

## 1. Требования

### Хост (Proxmox)

- Proxmox VE 8.x
- NVIDIA GPU с установленным **драйвером на хосте** (kernel module)
- Загруженные модули: `nvidia`, `nvidia-uvm`, `nvidia-modeset`
- Шаблон Fedora 43 (`fedora-43-default_*.tar.xz`)

### Файлы, которые потребуются

| Файл | Где взять |
|---|---|
| `NVIDIA-Linux-x86_64-<VERSION>.run` | [nvidia.com/drivers](https://www.nvidia.com/en-us/drivers/) |
| `cuda_<VERSION>_linux.run` | [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads) |

> [!IMPORTANT]
> Версия NVIDIA драйвера внутри контейнера **должна совпадать** с версией на хосте. Проверьте: `nvidia-smi` на хосте.

---

## 2. Создание LXC-контейнера

В Proxmox GUI или CLI:

```bash
# Создание контейнера (через GUI или CLI)
pct create <CTID> <STORAGE>:vztmpl/fedora-43-default_*.tar.xz \
  --hostname cloud-desktop \
  --memory 16384 \
  --cores 8 \
  --rootfs <STORAGE>:<SIZE_GB> \
  --net0 name=eth0,bridge=vmbr0,ip=dhcp \
  --ostype fedora \
  --features nesting=1 \
  --unprivileged 0
```

**Рекомендуемые параметры:**
- **RAM**: 16–24 ГБ
- **CPU**: 8–16 ядер
- **Диск**: 24 ГБ (минимум), 10 ГБ после сборки после очистки
- **Привилегированный** контейнер (`--unprivileged 0`)

---

## 3. Настройка конфигурации LXC

### 3.1 Определение устройств на хосте

Найдите ваши устройства:

```bash
# GPU — NVIDIA
ls -la /dev/nvidia*
ls -la /dev/dri/

# Определите номер вашей карты (card0, card1 и т.д.)
ls -la /dev/dri/by-path/  # PCI-адрес → card/render

# Major-номера устройств
cat /proc/devices | grep -E "nvidia|drm|input|alsa"
```

### 3.2 Конфигурация контейнера

Откройте конфигурацию контейнера на хосте:

```bash
nano /etc/pve/lxc/<CTID>.conf
```

Добавьте следующие строки. **Адаптируйте `card2` и `renderD128` под вашу систему:**

```ini
# --- GPU (NVIDIA) ---
lxc.cgroup2.devices.allow: c 195:* rwm     # /dev/nvidia*
lxc.cgroup2.devices.allow: c 226:* rwm     # /dev/dri/*
lxc.cgroup2.devices.allow: c 508:* rwm     # /dev/nvidia-uvm*
lxc.cgroup2.devices.allow: c 235:* rwm     # /dev/nvidia-caps

# --- Input (мышь, клавиатура для Sunshine) ---
lxc.cgroup2.devices.allow: c 13:* rwm      # /dev/input/*
lxc.cgroup2.devices.allow: c 10:223 rwm    # /dev/uinput

# --- Audio (ALSA для PipeWire) ---
lxc.cgroup2.devices.allow: c 116:* rwm     # /dev/snd/*

# --- GPU device nodes ---
lxc.mount.entry: /dev/nvidia0 dev/nvidia0 none bind,optional,create=file
lxc.mount.entry: /dev/nvidiactl dev/nvidiactl none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-modeset dev/nvidia-modeset none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-uvm dev/nvidia-uvm none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-uvm-tools dev/nvidia-uvm-tools none bind,optional,create=file
lxc.mount.entry: /dev/nvidia-caps dev/nvidia-caps none bind,optional,create=dir

# --- DRI (замените card2/renderD128 на ваши значения!) ---
lxc.mount.entry: /dev/dri/card2 dev/dri/card2 none bind,optional,create=file
lxc.mount.entry: /dev/dri/renderD128 dev/dri/renderD128 none bind,optional,create=file

# --- Input, Audio, Shared Memory ---
lxc.mount.entry: /dev/input dev/input none bind,optional,create=dir
lxc.mount.entry: /dev/uinput dev/uinput none bind,optional,create=file
lxc.mount.entry: /dev/snd dev/snd none bind,optional,create=dir
lxc.mount.entry: tmpfs dev/shm tmpfs rw,nosuid,nodev,mode=1777,size=4G,create=dir 0 0

# --- Security (необходимо для NVIDIA без ядерных модулей) ---
lxc.apparmor.profile: unconfined
lxc.mount.auto: sys:rw
```

> [!CAUTION]
> **Major-номера устройств** (195, 226, 508 и т.д.) зависят от вашей системы.
> Проверьте их в `/proc/devices`. Неверные номера приведут к отсутствию доступа к GPU.

### 3.3 Запуск контейнера

```bash
pct start <CTID>
```

### 3.4 Проверка устройств

```bash
pct exec <CTID> -- ls -la /dev/nvidia0 /dev/dri/renderD128
```

Если оба файла видны — конфигурация верна.

### 3.5 Фикс nvidia-uvm major после ребута хоста

Major-номер `/dev/nvidia-uvm` **динамический** — меняется при каждой перезагрузке хоста Proxmox. Если в LXC-конфиге указан устаревший номер, контейнер не получит доступ к CUDA.

Используйте скрипт из `deploy/nvidia/`:

```bash
# На хосте Proxmox
# Проверить текущий vs прописанный major
./fix-nvidia-uvm-major.sh <CTID>

# Автоматически обновить конфиг
./fix-nvidia-uvm-major.sh <CTID> --apply

# Перезапустить контейнер
pct stop <CTID> && pct start <CTID>
```

> [!TIP]
> Рекомендуется добавить вызов этого скрипта в автозапуск хоста (например, в `/etc/rc.local` или systemd timer), чтобы LXC-конфиг обновлялся автоматически при каждом ребуте.

---

## 4. Подготовка контейнера

Все команды ниже выполняются **внутри контейнера**:

```bash
pct exec <CTID> -- bash
```

### 4.1 Создание пользователя

```bash
useradd -m -s /bin/bash user
usermod -aG video,render,input,audio user
echo 'user:YOUR_PASSWORD' | chpasswd
echo 'user ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/user
chmod 440 /etc/sudoers.d/user
```

### 4.2 Настройка locale

```bash
dnf install -y glibc-langpack-ru
echo 'LANG=ru_RU.UTF-8' > /etc/locale.conf
```

> [!TIP]
> Замените `ru_RU.UTF-8` на нужную вам локаль (например, `en_US.UTF-8`).

### 4.3 Включение user linger (для systemd user services)

```bash
loginctl enable-linger user
```

---

## 5. Установка NVIDIA драйвера

### 5.1 Передача .run файла в контейнер

С хоста Proxmox:

```bash
pct push <CTID> /path/to/NVIDIA-Linux-x86_64-580.126.09.run /tmp/NVIDIA-Linux-x86_64-580.126.09.run
```

### 5.2 Установка (без kernel module)

Внутри контейнера:

```bash
chmod +x /tmp/NVIDIA-Linux-x86_64-580.126.09.run
/tmp/NVIDIA-Linux-x86_64-580.126.09.run \
  --no-kernel-module \
  --no-kernel-modules \
  --no-backup \
  --no-questions \
  --ui=none \
  --no-drm \
  --no-systemd
```

> [!IMPORTANT]
> Флаг `--no-kernel-module` **обязателен** — в LXC контейнере нет доступа к ядерным модулям, они загружены на хосте.

### 5.3 Проверка

```bash
nvidia-smi
```

Должна отобразиться ваша GPU и версия драйвера.

---

## 6. Патчи NvFBC и NVENC

Необходимы для снятия ограничений на количество сессий NvFBC/NVENC:

```bash
dnf install -y git
cd /tmp
git clone https://github.com/keylase/nvidia-patch.git
cd nvidia-patch

# Патч NvFBC (захват экрана)
bash patch-fbc.sh

# Патч NVENC (аппаратное кодирование)
bash patch.sh
```

Оба патча должны завершиться сообщением `Patched!`.

---

## 7. Сборка и установка компонентов

Gamescope и Sunshine необходимо собрать из исходников.

📖 **[BUILD.md](BUILD.md)** — полная инструкция по сборке: CUDA Toolkit, Gamescope, Sunshine, KCM Cloud Mouse, очистка build-зависимостей.

После выполнения BUILD.md у вас должны быть установлены:

| Компонент | Путь | Проверка |
|---|---|---|
| Gamescope | `/usr/local/bin/gamescope` | `gamescope --version` |
| Sunshine | `/usr/bin/sunshine` | `sunshine --version` |

> [!IMPORTANT]
> Sunshine должен быть собран **без `CAP_SYS_ADMIN`** — иначе `AT_SECURE=1` сломает сетевой стек (ENet).
> Подробности в [BUILD.md → Патч postinst](BUILD.md#33-патч-postinst-для-lxc).

---

## 8. Установка KDE Plasma

```bash
dnf install -y \
  kwin-wayland plasma-workspace plasmashell \
  kactivitymanagerd kded \
  polkit-kde \
  xorg-x11-server-Xwayland \
  pipewire pipewire-pulseaudio wireplumber \
  dbus-daemon \
  gsettings-desktop-schemas \
  plasma-desktop krdp konsole dolphin fuse \
  xdg-desktop-portal-kde binutils \
  plasma-workspace-x11 xorg-x11-server-Xorg \
  python3-xlib glx-utils xrandr \
  plasma-discover-flatpak plasma-discover-packagekit \
  xorg-x11-server-Xvfb xdpyinfo \
  ffmpeg python3-evdev \
  vulkan-tools mesa-dri-drivers pipewire-utils wget
```

---

## 9. Настройка скриптов и конфигов

Все необходимые файлы находятся в каталоге [`deploy/`](deploy/README.md) клонированного репозитория.

### 9.1 Скрипты запуска

Скопируйте скрипты из `deploy/scripts/` в `/home/user/`:

```bash
# Из клонированного репозитория gamescope
cd /home/user/gamescope
cp deploy/scripts/cloud-desktop.sh /home/user/
cp deploy/scripts/cloud-desktop-service.sh /home/user/
cp deploy/scripts/gamescope-resize.sh /home/user/

chmod +x /home/user/cloud-desktop.sh
chmod +x /home/user/cloud-desktop-service.sh
chmod +x /home/user/gamescope-resize.sh
chown user:user /home/user/*.sh
```

### 9.2 Проверка путей в скриптах

Убедитесь, что `gamescope-resize.sh` ссылается на правильный путь `gamescopectl`:

```bash
# Должен быть:
# GAMESCOPECTL="/usr/local/bin/gamescopectl"
grep GAMESCOPECTL /home/user/gamescope-resize.sh
```

---

## 10. Настройка Sunshine

### 10.1 Конфигурация

```bash
su - user
mkdir -p ~/.config/sunshine

cat > ~/.config/sunshine/sunshine.conf << 'EOF'
origin_pin_allowed = pc
origin_web_ui_allowed = lan
encoder = nvenc
adapter_name = /dev/dri/renderD128
output_name =
min_log_level = info
channels = 5
key_rightalt_to_key_win = enabled
EOF
```

### 10.2 Приложения

```bash
cp /home/user/gamescope/deploy/sunshine/apps.json ~/.config/sunshine/apps.json
```

> [!TIP]
> `origin_web_ui_allowed = lan` — позволяет доступ к веб-интерфейсу Sunshine с других машин в локальной сети. Если нужен доступ только с localhost, установите `pc`.

---

## 11. Shutdown/Reboot через интерфейс KDE

По умолчанию KDE Plasma в контейнере не может завершать/перезагружать систему. Для решения этой проблемы используется D-Bus-активируемый wrapper:

### 11.1 Wrapper для ksmserver-logout-greeter

```bash
cp /home/user/gamescope/deploy/scripts/ksmserver-logout-greeter-wrapper.sh \
   /usr/local/bin/ksmserver-logout-greeter-wrapper
chmod +x /usr/local/bin/ksmserver-logout-greeter-wrapper
```

### 11.2 D-Bus сервис

```bash
su - user
mkdir -p ~/.local/share/dbus-1/services
cp /home/user/gamescope/deploy/systemd/org.kde.LogoutPrompt.service \
   ~/.local/share/dbus-1/services/
```

### 11.3 Polkit правила (shutdown/reboot без пароля)

```bash
cp /home/user/gamescope/deploy/polkit/50-cloud-desktop.rules \
   /etc/polkit-1/rules.d/
```

---

## 12. Настройка мыши — KCM Cloud Mouse (опционально)

Модуль настроек мыши для KDE System Settings — настройка скорости, ускорения и прокрутки.

📦 Репозиторий: [github.com/wolfam0108/kcm-cloud-mouse](https://github.com/wolfam0108/kcm-cloud-mouse)

Сборка и установка описаны в [BUILD.md → KCM Cloud Mouse](BUILD.md#4-сборка-kcm-cloud-mouse-опционально).

После установки модуль появится в **System Settings → Input Devices → Cloud Mouse**.

---

## 13. Systemd-сервис и автозапуск

### 13.1 Установка сервиса

```bash
cp /home/user/gamescope/deploy/systemd/cloud-desktop.service \
   /etc/systemd/system/cloud-desktop.service
```

### 13.2 Активация автозапуска

```bash
systemctl daemon-reload
systemctl enable cloud-desktop.service
systemctl start cloud-desktop.service
```

### 13.3 Проверка статуса

```bash
systemctl status cloud-desktop.service
```

---

## 14. Подключение через Moonlight

1. Установите [Moonlight](https://moonlight-stream.org/) на клиентское устройство
2. Откройте веб-интерфейс Sunshine: `https://<IP_КОНТЕЙНЕРА>:47990`
3. При первом входе создайте логин и пароль
4. В Moonlight добавьте хост по IP контейнера
5. Введите PIN из Moonlight в веб-интерфейсе Sunshine для сопряжения
6. Подключайтесь — выберите приложение **«Cloud Desktop»**

---

## 15. Решение проблем

### GPU не видна в контейнере

```bash
ls -la /dev/nvidia0
# Если нет — проверьте LXC конфиг: major-номера и mount entries
cat /proc/devices | grep nvidia   # на хосте
```

### Sunshine: 403 Forbidden на Web UI

В `sunshine.conf` установите:

```ini
origin_web_ui_allowed = lan
```

Перезапустите Sunshine или контейнер.

### Sunshine: «Unable to find encoder»

Sunshine не может захватить экран. Убедитесь, что:
- Sunshine запущен **через `cloud-desktop.sh`** (а не напрямую) — ему нужен `WAYLAND_DISPLAY`
- `nvidia-smi` работает внутри контейнера
- NvFBC/NVENC патчи применены

### Plasmashell отсутствует после очистки

```bash
dnf install -y plasmashell kactivitymanagerd kded polkit-kde
```

### Нет звука

Проверьте, что PipeWire запущен:

```bash
su - user -c "pipewire --version && wireplumber --version"
```

### Контейнер не перезагружается через KDE

Проверьте wrapper и D-Bus service:

```bash
ls -la /usr/local/bin/ksmserver-logout-greeter-wrapper
cat ~/.local/share/dbus-1/services/org.kde.LogoutPrompt.service
```

### RPM-транзакции падают: `unable to set CAP_SETFCAP`

Fedora LXC по умолчанию блокирует capability `setfcap`. См. [секцию 0.5](#05-fedora-lxc-разблокировка-capabilities).

Быстрый workaround (не рекомендуется для постоянного использования):

```bash
dnf install --setopt=tsflags=nocaps <packages>
```

### Sunshine: Initial Ping Timeout / Hang detected!

Стандартная сборка Sunshine RPM устанавливает `cap_sys_admin+p` через `setcap`. Это активирует **secure execution** (`AT_SECURE=1`), которое блокирует работу ENet (control stream порт 47999). Результат — Moonlight не может подключиться.

| Параметр | С `setcap` (стандартный RPM) | Без `setcap` (наша сборка) |
|---|---|---|
| `AT_SECURE` | **1** ❌ | **0** ✅ |
| Control stream | **Не работает** | ✅ Работает |

**Решение**: собрать Sunshine без `cap_sys_admin` — см. [секцию 9.3](#93-патч-postinst-для-lxc).

### CUDA не работает после ребута хоста

Major-номер `nvidia-uvm` динамический. После перезагрузки хоста он меняется, и LXC-конфиг становится невалидным.

```bash
# На хосте — проверить и обновить
./fix-nvidia-uvm-major.sh <CTID> --apply
pct stop <CTID> && pct start <CTID>
```

См. [секцию 3.5](#35-фикс-nvidia-uvm-major-после-ребута-хоста).
