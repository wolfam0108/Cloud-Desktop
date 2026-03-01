# Сборка компонентов Cloud Desktop

Руководство по сборке всех компонентов из исходников внутри LXC-контейнера.

> [!NOTE]
> Этот документ дополняет [INSTALL-LXC.md](INSTALL-LXC.md).
> Все команды выполняются **внутри контейнера**, если не указано иное.

> [!IMPORTANT]
> Перед сборкой выполните шаги 0–5 из [INSTALL-LXC.md](INSTALL-LXC.md):
> подготовка хоста, создание контейнера, NVIDIA драйвер, патчи NvFBC/NVENC.

---

## Содержание

1. [CUDA Toolkit](#1-cuda-toolkit)
2. [Сборка Gamescope](#2-сборка-gamescope)
3. [Сборка Sunshine](#3-сборка-sunshine)
4. [Сборка KCM Cloud Mouse (опционально)](#4-сборка-kcm-cloud-mouse-опционально)
5. [Очистка build-зависимостей](#5-очистка-build-зависимостей)

---

## 1. CUDA Toolkit

CUDA необходим **только для сборки** Sunshine с поддержкой аппаратного кодирования. После сборки его можно удалить.

### 1.1 Передача и установка

С хоста:

```bash
pct push <CTID> /path/to/cuda_13.0.0_580.65.06_linux.run /tmp/cuda_13.0.0_580.65.06_linux.run
```

Внутри контейнера:

```bash
chmod +x /tmp/cuda_13.0.0_580.65.06_linux.run
/tmp/cuda_13.0.0_580.65.06_linux.run --toolkit --silent --no-drm --override
```

### 1.2 Патч заголовков (для glibc 2.41+)

На Fedora 43+ с glibc ≥ 2.41 необходимо пропатчить заголовки CUDA:

```bash
CUDA_MATH="/usr/local/cuda-13.0/targets/x86_64-linux/include/crt/math_functions.h"

# Создаём резервную копию
cp "$CUDA_MATH" "${CUDA_MATH}.bak"

# Применяем патч
sed -i '629s/rsqrt(double x);/rsqrt(double x) noexcept(true);/' "$CUDA_MATH"
sed -i '653s/rsqrtf(float x);/rsqrtf(float x) noexcept(true);/' "$CUDA_MATH"
```

### 1.3 Проверка

```bash
/usr/local/cuda-13.0/bin/nvcc --version
```

---

## 2. Сборка Gamescope

### 2.1 Установка build-зависимостей

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

### 2.2 Клонирование и сборка

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

### 2.3 Установка

```bash
# От root
exit  # если вы под user
ninja -C /home/user/gamescope/build install
```

Gamescope установится в `/usr/local/bin/gamescope`.

---

## 3. Сборка Sunshine

### 3.1 Дополнительные build-зависимости

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

### 3.2 Клонирование и сборка

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

### 3.3 Патч postinst (для LXC)

В LXC не нужен `CAP_SYS_ADMIN`, более того — он **ломает** сетевой стек Sunshine (ENet). При стандартной сборке RPM устанавливает `cap_sys_admin+p` через `setcap`, что активирует `AT_SECURE=1` — и ENet не может принять соединения от Moonlight.

| Параметр | С `setcap` (стандартный RPM) | Без `setcap` (наша сборка) |
|---|---|---|
| `AT_SECURE` | **1** ❌ | **0** ✅ |
| Control stream | **Не работает** | ✅ Работает |

Отключаем `cap_sys_admin`:

```bash
# Находим строки с cap_sys_admin в postinst и заменяем на заглушку
sed -i '/cap_sys_admin/s/^/#/' src_assets/linux/misc/postinst

# Добавляем заглушку после закомментированного блока (чтобы if-else не сломался)
sed -i '/^#.*cap_sys_admin+p.*sunshine/a\    true  # cap_sys_admin disabled for LXC' src_assets/linux/misc/postinst
```

### 3.4 Создание и установка RPM

```bash
cpack -G RPM --config ./build/CPackConfig.cmake

# Установка
exit  # если вы под user
dnf install -y /home/user/Sunshine/build/cpack_artifacts/Sunshine.rpm
```

---

## 4. Сборка KCM Cloud Mouse (опционально)

Модуль настроек мыши для KDE System Settings — позволяет настраивать скорость, ускорение и прокрутку через D-Bus API Gamescope.

📦 Репозиторий: [github.com/wolfam0108/kcm-cloud-mouse](https://github.com/wolfam0108/kcm-cloud-mouse)

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

## 5. Очистка build-зависимостей

После успешной сборки и тестирования можно удалить ~10 ГБ build-артефактов.

> [!CAUTION]
> **Не используйте** `dnf remove $(rpm -qa '*-devel')` — это каскадно удалит runtime-библиотеки
> (`libseat`, `libinput`, `libxkbcommon` и т.д.), которые нужны Gamescope для работы!

```bash
# 1. CUDA Toolkit (4.5 ГБ)
rm -rf /usr/local/cuda-13.0 /usr/local/cuda

# 2. Исходники (4.6 ГБ)
rm -rf /home/user/gamescope /home/user/Sunshine /home/user/kcm-cloud-mouse
rm -f /home/user/*.tar.gz

# 3. Файлы установщиков
rm -f /tmp/NVIDIA-Linux-*.run /tmp/cuda_*.run
rm -rf /tmp/nvidia-patch

# 4. Build-пакеты (компиляторы, генераторы, тулинг)
dnf remove -y \
  gcc-c++ gcc cpp meson ninja-build cmake cmake-data \
  npm nodejs nodejs-full-i18n nodejs-docs nodejs-npm \
  doxygen graphviz rpm-build libstdc++-static \
  extra-cmake-modules

# 5. Devel-пакеты (ТОЛЬКО безопасные — без runtime-зависимостей Gamescope)
dnf remove -y \
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
  glib2-devel pipewire-devel \
  libnotify-devel libayatana-appindicator-gtk3-devel \
  opus-devel pulseaudio-libs-devel \
  openssl-devel libcurl-devel miniupnpc-devel \
  libevdev-devel libva-devel \
  boost-devel numactl-devel \
  qt6-qtbase-devel qt6-qtdeclarative-devel \
  kf6-kcmutils-devel kf6-ki18n-devel kf6-kcoreaddons-devel

# 6. Автоматическая очистка
dnf autoremove -y
dnf clean all
```

### Проверка runtime-зависимостей

После очистки **обязательно** проверьте, что Gamescope и Plasma запускаются:

```bash
# Gamescope — все библиотеки на месте?
ldd /usr/local/bin/gamescope | grep "not found"

# Plasma — не удалена каскадно?
which plasmashell || dnf install -y plasmashell kactivitymanagerd kded polkit-kde

# Ключевые runtime-библиотеки
rpm -q libseat libinput libxkbcommon libei libdecor dbus-x11
```

Если `ldd` показывает `not found` — установите недостающие пакеты:

```bash
# Пример: libseat удалена каскадно
dnf install -y libseat libinput libxkbcommon
```

### Результат

| Метрика | До очистки | После очистки |
|---|---|---|
| Занято на диске | ~13 ГБ | ~3 ГБ |
