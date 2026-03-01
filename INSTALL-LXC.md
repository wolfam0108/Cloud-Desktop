# Установка Cloud Desktop в LXC-контейнере Proxmox

Полное руководство по созданию облачного рабочего стола (Cloud Desktop) на базе **Gamescope + KWin + Plasma + Sunshine** внутри LXC-контейнера Proxmox с пробросом NVIDIA GPU.

> [!NOTE]
> Руководство проверено на: **Proxmox VE 8.x**, ядро **6.17**, драйвер **NVIDIA 580.126.09**, **Fedora 43**, GPU **RTX 3090**.
> Адаптируйте пути устройств и версии под вашу конфигурацию.

---

## Содержание

1. [Требования](#1-требования)
2. [Создание LXC-контейнера](#2-создание-lxc-контейнера)
3. [Настройка конфигурации LXC](#3-настройка-конфигурации-lxc)
4. [Подготовка контейнера](#4-подготовка-контейнера)
5. [Установка NVIDIA драйвера](#5-установка-nvidia-драйвера)
6. [Патчи NvFBC и NVENC](#6-патчи-nvfbc-и-nvenc)
7. [Установка CUDA Toolkit](#7-установка-cuda-toolkit)
8. [Сборка Gamescope](#8-сборка-gamescope)
9. [Сборка Sunshine](#9-сборка-sunshine)
10. [Установка KDE Plasma](#10-установка-kde-plasma)
11. [Настройка скриптов и конфигов](#11-настройка-скриптов-и-конфигов)
12. [Настройка Sunshine](#12-настройка-sunshine)
13. [Shutdown/Reboot через интерфейс KDE](#13-shutdownreboot-через-интерфейс-kde)
14. [Настройка мыши — KCM Cloud Mouse (опционально)](#14-настройка-мыши--kcm-cloud-mouse-опционально)
15. [Systemd-сервис и автозапуск](#15-systemd-сервис-и-автозапуск)
16. [Очистка build-зависимостей](#16-очистка-build-зависимостей)
17. [Подключение через Moonlight](#17-подключение-через-moonlight)
18. [Решение проблем](#18-решение-проблем)

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

## 7. Установка CUDA Toolkit

CUDA необходим **только для сборки** Sunshine с поддержкой аппаратного кодирования.

### 7.1 Передача и установка

С хоста:

```bash
pct push <CTID> /path/to/cuda_13.0.0_580.65.06_linux.run /tmp/cuda_13.0.0_580.65.06_linux.run
```

Внутри контейнера:

```bash
chmod +x /tmp/cuda_13.0.0_580.65.06_linux.run
/tmp/cuda_13.0.0_580.65.06_linux.run --toolkit --silent --no-drm --override
```

### 7.2 Патч заголовков (для glibc 2.41+)

На Fedora 43+ с glibc ≥ 2.41 необходимо пропатчить заголовки CUDA:

```bash
CUDA_MATH="/usr/local/cuda-13.0/targets/x86_64-linux/include/crt/math_functions.h"

# Создаём резервную копию
cp "$CUDA_MATH" "${CUDA_MATH}.bak"

# Применяем патч
sed -i '629s/rsqrt(double x);/rsqrt(double x) noexcept(true);/' "$CUDA_MATH"
sed -i '653s/rsqrtf(float x);/rsqrtf(float x) noexcept(true);/' "$CUDA_MATH"
```

### 7.3 Проверка

```bash
/usr/local/cuda-13.0/bin/nvcc --version
```

> [!NOTE]
> CUDA Toolkit будет удалён на этапе [очистки](#16-очистка-build-зависимостей) после сборки.

---

## 8. Сборка Gamescope

### 8.1 Установка build-зависимостей

```bash
dnf install -y \
  meson ninja-build gcc-c++ cmake pkg-config \
  libX11-devel libXdamage-devel libXcomposite-devel libXcursor-devel \
  libXrender-devel libXext-devel libXfixes-devel libXxf86vm-devel \
  libXtst-devel libXres-devel libXmu-devel libXi-devel \
  libdrm-devel mesa-libgbm-devel mesa-libEGL-devel \
  vulkan-devel vulkan-headers \
  libinput-devel libxkbcommon-devel \
  wayland-devel wayland-protocols-devel \
  pixman-devel libcap-devel libei-devel \
  libdisplay-info-devel libseat-devel libdecor-devel \
  luajit-devel systemd-devel \
  xorg-x11-server-Xwayland-devel \
  xcb-util-wm-devel xcb-util-errors-devel \
  glslang
```

### 8.2 Клонирование и сборка

```bash
su - user

# Клонирование из форка
cd ~
git clone https://github.com/wolfam0108/Cloud-Desktop.git gamescope
cd gamescope

# Настройка сборки
meson setup -Dpipewire=disabled build .

# Компиляция (используем все ядра)
ninja -C build -j$(nproc)
```

Должно завершиться: `[603/603] Linking target src/gamescope`

### 8.3 Установка

```bash
# От root
exit  # если вы под user
ninja -C /home/user/gamescope/build install
```

Gamescope установится в `/usr/local/bin/gamescope`.

---

## 9. Сборка Sunshine

### 9.1 Дополнительные build-зависимости

```bash
dnf install -y \
  glib2-devel pipewire-devel \
  libnotify-devel libayatana-appindicator-gtk3-devel \
  npm doxygen graphviz \
  opus-devel pulseaudio-libs-devel \
  libstdc++-static rpm-build \
  openssl-devel libcurl-devel miniupnpc-devel \
  libevdev-devel libva-devel \
  boost-devel numactl-devel
```

### 9.2 Клонирование и сборка

```bash
su - user
cd ~

git clone --recurse-submodules https://github.com/LizardByte/Sunshine.git
cd Sunshine

# Настройка (с CUDA)
export PATH=/usr/local/cuda-13.0/bin:$PATH

cmake -B build -G Ninja -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_EXECUTABLE_PATH=/usr/bin/sunshine \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCMAKE_CUDA_COMPILER=/usr/local/cuda-13.0/bin/nvcc \
  -DSUNSHINE_ENABLE_DRM=ON \
  -DSUNSHINE_ENABLE_WAYLAND=ON \
  -DSUNSHINE_ENABLE_X11=ON \
  -DSUNSHINE_ENABLE_PORTAL=ON

# Компиляция
ninja -C build -j$(nproc)
```

### 9.3 Патч postinst (для LXC)

В LXC не нужен `CAP_SYS_ADMIN`, поэтому отключаем его в post-install скрипте:

```bash
# Находим строки с cap_sys_admin в postinst и заменяем на заглушку
sed -i '/cap_sys_admin/s/^/#/' src_assets/linux/misc/postinst

# Добавляем заглушку после закомментированного блока (чтобы if-else не сломался)
sed -i '/^#.*cap_sys_admin+p.*sunshine/a\    true  # cap_sys_admin disabled for LXC' src_assets/linux/misc/postinst
```

### 9.4 Создание и установка RPM

```bash
cpack -G RPM --config ./build/CPackConfig.cmake

# Установка
exit  # если вы под user
dnf install -y /home/user/Sunshine/build/cpack_artifacts/Sunshine.rpm
```

---

## 10. Установка KDE Plasma

```bash
dnf install -y \
  kwin-wayland plasma-workspace plasmashell \
  kactivitymanagerd kded \
  polkit-kde \
  xorg-x11-server-Xwayland \
  pipewire pipewire-pulseaudio wireplumber \
  dbus-daemon \
  gsettings-desktop-schemas
```

---

## 11. Настройка скриптов и конфигов

Все необходимые файлы находятся в каталоге [`deploy/`](deploy/README.md) клонированного репозитория.

### 11.1 Скрипты запуска

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

### 11.2 Проверка путей в скриптах

Убедитесь, что `gamescope-resize.sh` ссылается на правильный путь `gamescopectl`:

```bash
# Должен быть:
# GAMESCOPECTL="/usr/local/bin/gamescopectl"
grep GAMESCOPECTL /home/user/gamescope-resize.sh
```

---

## 12. Настройка Sunshine

### 12.1 Конфигурация

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

### 12.2 Приложения

```bash
cp /home/user/gamescope/deploy/sunshine/apps.json ~/.config/sunshine/apps.json
```

> [!TIP]
> `origin_web_ui_allowed = lan` — позволяет доступ к веб-интерфейсу Sunshine с других машин в локальной сети. Если нужен доступ только с localhost, установите `pc`.

---

## 13. Shutdown/Reboot через интерфейс KDE

По умолчанию KDE Plasma в контейнере не может завершать/перезагружать систему. Для решения этой проблемы используется D-Bus-активируемый wrapper:

### 13.1 Wrapper для ksmserver-logout-greeter

```bash
cp /home/user/gamescope/deploy/scripts/ksmserver-logout-greeter-wrapper.sh \
   /usr/local/bin/ksmserver-logout-greeter-wrapper
chmod +x /usr/local/bin/ksmserver-logout-greeter-wrapper
```

### 13.2 D-Bus сервис

```bash
su - user
mkdir -p ~/.local/share/dbus-1/services
cp /home/user/gamescope/deploy/systemd/org.kde.LogoutPrompt.service \
   ~/.local/share/dbus-1/services/
```

### 13.3 Polkit правила (shutdown/reboot без пароля)

```bash
cp /home/user/gamescope/deploy/polkit/50-cloud-desktop.rules \
   /etc/polkit-1/rules.d/
```

---

## 14. Настройка мыши — KCM Cloud Mouse (опционально)

Модуль настроек мыши для KDE System Settings, позволяющий настраивать скорость, ускорение и прокрутку напрямую через рабочий стол.

📦 Репозиторий: [github.com/wolfam0108/kcm-cloud-mouse](https://github.com/wolfam0108/kcm-cloud-mouse)

### Установка

```bash
# Build-зависимости
dnf install -y \
  extra-cmake-modules \
  qt6-qtbase-devel qt6-qtdeclarative-devel \
  kf6-kcmutils-devel kf6-ki18n-devel kf6-kcoreaddons-devel

# Сборка
su - user
cd ~
git clone https://github.com/wolfam0108/kcm-cloud-mouse.git
cd kcm-cloud-mouse
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Установка (от root)
exit
ninja -C /home/user/kcm-cloud-mouse/build install
```

После установки модуль появится в **System Settings → Input Devices → Cloud Mouse**.

---

## 15. Systemd-сервис и автозапуск

### 15.1 Установка сервиса

```bash
cp /home/user/gamescope/deploy/systemd/cloud-desktop.service \
   /etc/systemd/system/cloud-desktop.service
```

### 15.2 Активация автозапуска

```bash
systemctl daemon-reload
systemctl enable cloud-desktop.service
systemctl start cloud-desktop.service
```

### 15.3 Проверка статуса

```bash
systemctl status cloud-desktop.service
```

---

## 16. Очистка build-зависимостей

После успешной сборки и тестирования можно удалить ~10 ГБ build-артефактов:

```bash
# 1. CUDA Toolkit (4.5 ГБ)
rm -rf /usr/local/cuda-13.0 /usr/local/cuda

# 2. Исходники (4.6 ГБ)
rm -rf /home/user/gamescope /home/user/Sunshine /home/user/kcm-cloud-mouse
rm -f /home/user/*.tar.gz

# 3. Файлы установщиков
rm -f /tmp/NVIDIA-Linux-*.run /tmp/cuda_*.run
rm -rf /tmp/nvidia-patch

# 4. Build-пакеты
dnf remove -y \
  gcc-c++ gcc cpp meson ninja-build cmake cmake-data \
  npm nodejs nodejs-full-i18n nodejs-docs nodejs-npm \
  doxygen graphviz rpm-build libstdc++-static git \
  extra-cmake-modules

# 5. Devel-пакеты
dnf remove -y $(rpm -qa '*-devel' | tr '\n' ' ')

# 6. Автоматическая очистка
dnf autoremove -y
dnf clean all
```

> [!WARNING]
> После удаления `-devel` пакетов проверьте, что `plasmashell` не был удалён каскадно:
> ```bash
> which plasmashell || dnf install -y plasmashell kactivitymanagerd kded polkit-kde
> ```

### Результат

| Метрика | До очистки | После очистки |
|---|---|---|
| Занято на диске | ~13 ГБ | ~3 ГБ |

---

## 17. Подключение через Moonlight

1. Установите [Moonlight](https://moonlight-stream.org/) на клиентское устройство
2. Откройте веб-интерфейс Sunshine: `https://<IP_КОНТЕЙНЕРА>:47990`
3. При первом входе создайте логин и пароль
4. В Moonlight добавьте хост по IP контейнера
5. Введите PIN из Moonlight в веб-интерфейсе Sunshine для сопряжения
6. Подключайтесь — выберите приложение **«Cloud Desktop»**

---

## 18. Решение проблем

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
