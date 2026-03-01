# Cloud Desktop — Gamescope Fork

<p align="center">
  <strong>Облачный рабочий стол KDE Plasma внутри LXC-контейнера</strong><br/>
  <em>Zero-copy GPU pipeline · NVIDIA NVENC · Moonlight streaming</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/base-Gamescope%203.16.20-blue" />
  <img src="https://img.shields.io/badge/Wayland-only-green" />
  <img src="https://img.shields.io/badge/GPU-zero--copy-orange" />
  <img src="https://img.shields.io/badge/NVIDIA-580.x-76B900?logo=nvidia" />
  <img src="https://img.shields.io/badge/license-BSD--2--Clause-lightgrey" />
</p>

---

## Что это

Форк [Gamescope 3.16.20](https://github.com/ValveSoftware/gamescope/tree/3.16.20) с кастомным **Sunshine-бэкендом**, превращающий Gamescope в **системный Wayland-композитор для облачного рабочего стола**.

### Проблема

Gamescope от Valve — микрокомпозитор для SteamOS, заточенный под одну задачу: показать Steam Big Picture на физическом мониторе через DRM/KMS. Он не имеет бэкенда для стриминга, не предназначен для запуска полноценных десктопных сред и не работает в контейнерных окружениях без DRM Master.

Sunshine (ранее Moonlight Host) — стриминг-сервер, который умеет захватывать экран через KMS, PipeWire, X11 или Wayland screencopy. Но у Sunshine нет собственного композитора — ему нужен готовый экран для захвата.

KWin — композитор KDE Plasma. Полноценный Wayland-сервер, поддерживающий все десктопные протоколы. Но KWin ожидает конкретный набор Wayland-протоколов от родительского композитора, работает с subsurfaces и посылает dummy-буферы — всё то, что Gamescope в оригинале не обрабатывает.

### Решение

Этот форк превращает Gamescope в **промежуточный слой** между KWin и Sunshine:

```
┌─────────────────────────────────────────────────────────────────┐
│ Оригинальный Gamescope          │ Cloud Desktop (этот форк)     │
│─────────────────────────────────│───────────────────────────────│
│ DRM/KMS backend → монитор       │ Sunshine backend → стрим      │
│ Один Xwayland клиент (Steam)    │ KWin + Plasma + любые приложения │
│ Требует DRM Master              │ Работает без DRM Master (LXC) │
│ PipeWire capture (отдельный путь)│ Zero-copy GBM → screencopy → NVENC │
│ X11 focus model                 │ XDG + subsurface forwarding   │
│ Курсор от Xwayland              │ Wayland cursor от KWin        │
│ Нет input bridge (libinput → seat)│ Input bridge: uinput → KWin (~0.3ms) │
└─────────────────────────────────────────────────────────────────┘
```

Внутри LXC-контейнера поверх Gamescope запускается **полноценный KDE Plasma + KWin**, а стриминг на клиент (Moonlight) идёт через Sunshine — с аппаратным кодированием **NVENC** и **полностью в VRAM** (zero-copy DMA-BUF pipeline).

### Реальная эксплуатация

Стек прошёл тестирование в продакшен-конфигурации:

- **Steam** запускается нативно, игры стримятся через Remote Play
- **Firefox**, Dolphin, Konsole и другие KDE-приложения работают без ограничений
- Аудио через PipeWire → Sunshine → Moonlight
- Полный ввод мыши/клавиатуры с задержкой ~0.3ms
- Shutdown/Reboot через стандартный интерфейс KDE Plasma
- Стабильная работа на NVIDIA RTX 3090 с драйвером 580.x

---

## Архитектура

### Общая схема

```
╔══════════════════════════════════════════════════════════════════════╗
║  LXC Container (Fedora 43 / Proxmox)                               ║
║                                                                      ║
║  ┌────────────────────────── VRAM (GPU) ──────────────────────────┐  ║
║  │                                                                │  ║
║  │  KWin EGL Render ─► DMA-BUF ─► Gamescope Vulkan ─► GBM Slots  │  ║
║  │  (desktop scene)   (subsurface)  (compute shader)  (3 буфера)  │  ║
║  │                                                      │         │  ║
║  │                                              DMA-BUF fd        │  ║
║  │                                                      │         │  ║
║  │                              Sunshine EGL import ◄───┘         │  ║
║  │                                      │                         │  ║
║  │                              CUDA → NVENC                      │  ║
║  │                              (H.264/HEVC encode)               │  ║
║  └────────────────────────────────────────────────────────────────┘  ║
║                                                                      ║
║  Gamescope (gamescope-0)       ◄── system compositor                 ║
║    │                                                                 ║
║    ├── KWin 6 (wayland-0)      ◄── session compositor (nested)       ║
║    │     ├── Plasmashell       ◄── desktop shell                    ║
║    │     ├── Steam             ◄── игровой клиент                   ║
║    │     ├── Firefox           ◄── браузер                          ║
║    │     └── XWayland          ◄── X11-совместимость               ║
║    │                                                                 ║
║    └── Sunshine                ◄── screencopy client → стрим         ║
║                                                                      ║
╚══════════════════════════════════════════════════════════════════════╝
         │                                          ▲
         ▼ H.264/HEVC stream                        │ Input (mouse, keyboard)
    ┌──────────┐                               ┌──────────┐
    │ Moonlight│                               │ Moonlight│
    │ (client) │                               │ (client) │
    └──────────┘                               └──────────┘
```

### Двойной композитор: зачем это нужно

В архитектуре сознательно используются **два Wayland-композитора**:

| Роль | Композитор | Сокет | Что делает |
|---|---|---|---|
| **System** | Gamescope | `gamescope-0` | Управляет GPU pipeline, GBM буферами, screencopy. Видит KWin как **единственный** XDG toplevel. |
| **Session** | KWin 6 | `wayland-0` | Управляет desktop: окна, декорации, панели, workspace. Видит Plasma и все приложения как своих клиентов. |

KWin подключается к Gamescope как обычный Wayland-клиент, создаёт `xdg_toplevel` и рендерит весь desktop в **один subsurface**. Для Gamescope это «одно окно» — вся сложность KDE Plasma скрыта за одним DMA-BUF буфером.

### Zero-Copy Pixel Pipeline

Весь путь кадра от KWin до Moonlight — **без единого копирования в RAM**:

```
KWin рендерит desktop (EGL → DMA-BUF)
    │
    ▼ wl_surface.commit(subsurface, DMA-BUF fd)
    │
Gamescope wlserver: принимает DMA-BUF fd через Wayland
    │
    ▼ subsurface forwarding → xdg surface matching
    │
steamcompmgr: import_commit() → VkImage из DMA-BUF (zero-copy import)
    │
    ▼ paint_all() → vulkan_screenshot() (compute shader, imageStore)
    │
Present(): результат записан в GBM slot (VRAM, tiled NVIDIA modifier)
    │
    ▼ write(eventfd, 1) → сигнал Wayland thread
    │
sunshine_commit_handler(): wlr_output_commit + wlr_output_send_frame (0ms)
    │
    ▼ screencopy → Sunshine получает DMA-BUF fd слота
    │
Sunshine: EGL import → CUDA context → NVENC encode → H.264/HEVC stream
    │
    ▼ сеть
    │
Moonlight: декодирование → отображение
```

| Этап | Память | Метод | Копирование |
|---|---|---|---|
| KWin → Gamescope | DMA-BUF (VRAM) | wl_surface commit | zero-copy |
| Vulkan import | VkImage из DMA-BUF | `vkCreateImage` с external memory | zero-copy |
| Composite → GBM slot | GBM bo (VRAM, tiled) | compute shader `imageStore` | GPU-to-GPU |
| GBM → Sunshine | DMA-BUF fd | screencopy protocol | zero-copy |
| Sunshine → NVENC | EGL/CUDA texture | EGL import → CUDA map | zero-copy |
| NVENC → stream | H.264/HEVC bitstream | hardware encoder | — |

Ни на одном этапе пиксели не покидают VRAM и не копируются в системную память. Это обеспечивает минимальную задержку и нулевую нагрузку на CPU для pixel pipeline.

### GBM Slot Pool

Gamescope использует пул из **3 GBM буферов** (triple-buffering):

```
┌─────────┐  ┌─────────┐  ┌─────────┐
│ Slot 0  │  │ Slot 1  │  │ Slot 2  │
│ GBM bo  │  │ GBM bo  │  │ GBM bo  │
│ VkImage │  │ VkImage │  │ VkImage │
│ DMA-BUF │  │ DMA-BUF │  │ DMA-BUF │
│ wlr_buf │  │ wlr_buf │  │ wlr_buf │
└─────────┘  └─────────┘  └─────────┘
     ▲              ▲            ▲
     │              │            │
  rendering     committed    free/idle
  (Present)    (Sunshine      (available
               holds fd)     for acquire)
```

Каждый слот содержит:
- **GBM bo** — буфер в VRAM (`gbm_bo_create` с `GBM_BO_USE_RENDERING`, без `LINEAR` — NVIDIA не поддерживает linear)
- **VkImage** — Vulkan текстура, импортированная из DMA-BUF (с флагами `bSampled + bStorage + bTransferSrc`)
- **DMA-BUF fd** — файловый дескриптор для передачи через Wayland
- **wlr_buffer** — обёртка wlroots для screencopy

Ротация: `acquire() → render → commit → release old → acquire next`.

Слоты управляются через **Slot Release Callback Pattern** — `wlserver.cpp` не знает о GBMSlot, работает с `void*` и callback-функцией, зарегистрированной бэкендом.

### Потоковая модель

```
┌─────────────────────────┐     ┌──────────────────────────┐
│  steamcompmgr thread    │     │    Wayland thread         │
│  (gamescope-xwm)        │     │    (gamescope-wl)         │
│                         │     │                          │
│  • check_new_xdg_res()  │     │  • wl_display_dispatch()  │
│  • import_commit()      │     │  • xwayland_surface_commit│
│  • paint_all()          │     │  • sunshine_commit_handler│
│  • vulkan_composite()   │     │  • wlr_output_commit()   │
│  • Present()            │     │  • wlr_output_send_frame │
│                         │     │                          │
│  Vulkan render          │     │  screencopy dispatch     │
│  GBM slot acquire       │     │  frame callbacks         │
│                         │     │                          │
└───────┬─────────────────┘     └──────────┬───────────────┘
        │                                  ▲
        │  write(eventfd, 1)               │
        └──────────────────────────────────┘

┌─────────────────────────┐
│  libinput thread        │
│  (gamescope-libinput)   │
│                         │
│  • CLibInputHandler     │
│  • 1000 Hz poll         │
│  • wlserver_mousemotion │
│  • wlserver_key         │
│  • scroll / accel       │
└─────────────────────────┘
```

**Критическое правило**: функции wlroots (`wlr_output_commit_state`, `wlr_output_send_frame`) можно вызывать **только из Wayland thread**. Поэтому `Present()` не отправляет кадр напрямую, а сигнализирует через `eventfd` — Wayland thread подхватывает сигнал и делает commit.

### Eventfd Bridge: межпоточный handoff кадра

```
steamcompmgr thread:                    Wayland thread:
  paint_all()                             poll(wayland_fd, eventfd, ...)
    → vulkan_screenshot(layers → slot)      │
    → Present(frameInfo)                    │ eventfd readable!
      → lock(sunshine_lock)                 │
      → sunshine_pending_slot = slot        ▼
      → unlock()                          sunshine_commit_handler():
      → write(eventfd, 1) ─────────────►   │ lock()
                                            │ slot = pending
                                            │ unlock()
                                            │ release(old_committed)
                                            │ wlr_output_commit_state()
                                            │ wlr_output_send_frame() ← 0ms
                                            │ committed = slot
                                            ▼
                                          → Sunshine screencopy
                                            получает DMA-BUF fd
```

---

## Проблемы KWin ↔ Gamescope и их решения

Запустить KWin поверх Gamescope «из коробки» невозможно — KWin ведёт себя не так, как Steam (единственный ожидаемый клиент Gamescope). Ниже описаны все обнаруженные несовместимости и их решения.

### 1. Subsurface Forwarding

**Проблема**: KWin 6 рендерит весь рабочий стол не в main surface, а в **subsurface** (дочерний surface). Main surface получает dummy-буфер 1×1 пиксель. Gamescope ожидает контент в main surface — и показывает пустоту.

**Как устроено**:

```
KWin создаёт:
  main_surface (xdg_toplevel) ← 1×1 dummy buffer
    └── subsurface             ← реальный desktop 1920×1080 DMA-BUF
```

Gamescope в оригинале обрабатывает commit только для surface, привязанного к `xdg_surface`. Subsurface от того же клиента не матчится ни с каким окном — commit игнорируется.

**Решение**: В `xwayland_surface_commit()` добавлено перенаправление: если commit пришёл от surface, которая не привязана ни к одному XDG/X11 окну, проверяем — является ли она subsurface. Если да, используем parent surface для matching (чтобы привязать к XDG toplevel KWin), но сохраняем **оригинальный** буфер subsurface для текстуры:

```
commit от subsurface:
  surf для matching  = subsurface→parent (XDG toplevel)
  buf для текстуры   = subsurface→buffer (реальный desktop)
```

> **Критический нюанс**: если подменить И surface И buffer на parent — получим текстуру 1×1 dummy вместо реального контента. Разделение surf/buf — ключевое.

### 2. Dummy Buffer Filter

**Проблема**: KWin периодически делает commit в main surface с буфером 1×1 пиксель (dummy). Этот commit матчится с XDG toplevel и перезаписывает `HELD_COMMIT_BASE` — следующий `paint_all()` рисует 1×1 пиксель вместо полного десктопа. Результат — чёрный экран.

**Решение**: В `check_new_xdg_res()` — фильтрация буферов ≤4×4 пикселей **до** matching и Vulkan import:

```cpp
if (buf && buf->width <= 4 && buf->height <= 4) {
    wlr_buffer_unlock(buf);
    continue;  // пропускаем dummy
}
```

### 3. XDG Configure Handshake

**Проблема**: При первом подключении клиента Gamescope отправляет `xdg_surface.configure` **без** указания размера (KWin-клиент сам решает). KWin по умолчанию создаёт окно с минимальным размером, что ломает масштабирование и расположение.

**Решение**: Перед `schedule_configure()` явно задаём размер через `wlr_xdg_toplevel_set_size()` и активируем toplevel:

```
wlr_xdg_toplevel_set_size(toplevel, 1920, 1080)
wlr_xdg_toplevel_set_activated(toplevel, true)
wlr_xdg_surface_schedule_configure(xdg_surface)
```

KWin получает configure с нужным разрешением и сразу рендерит в правильном размере.

### 4. Frame Callback Bootstrap Deadlock

**Проблема**: Классический deadlock при инициализации:

```
Gamescope ждёт commit от KWin → чтобы вызвать paint_all() → чтобы отправить frame_done
KWin ждёт frame_done от Gamescope → чтобы рендерить следующий кадр → чтобы сделать commit
```

Ни одна сторона не делает первый шаг. Результат — KWin молча зависает.

**Решение**: В `steamcompmgr_flush_frame_done()` — безусловная отправка `frame_done` всем XDG surfaces при bootstrap, даже если у них ещё нет фокуса. Это разрывает deadlock: KWin получает frame callback → рендерит → делает commit → Gamescope получает контент.

### 5. Screencopy VkImage Usage Flags

**Проблема**: Moonlight подключается, видео stream активен, но экран **чёрный**. Диагностика через Vulkan readback показала: пиксели в GBM слоте — нулевые.

**Root cause**: `vulkan_screenshot()` использует **compute shader** (`SHADER_TYPE_BLIT` → `bindTarget()` → `dispatch()` → `imageStore()`). Compute shader требует `VK_IMAGE_USAGE_STORAGE_BIT` на целевом VkImage. Но `vulkan_create_texture_from_dmabuf()` по умолчанию создаёт VkImage только с `VK_IMAGE_USAGE_SAMPLED_BIT`. Без `STORAGE_BIT` compute shader молча не записывает данные — VkImage остаётся нулевым.

**Методология отладки**:

| Шаг | Инструмент | Результат |
|---|---|---|
| 1 | GPU readback 4 пикселей через `vkCmdCopyImageToBuffer` | SRC и DST — нули |
| 2 | Standalone тест `test_vk_readback.c` (GBM→Vulkan→Clear→Readback) | 5/5 тестов PASS |
| 3 | Логирование адресов VkImage в Present и screencopy | Адреса совпадают |
| 4 | Анализ `vulkan_screenshot()` — compute shader `bindTarget()` | **Нужен STORAGE_BIT** |

**Решение**: При создании VkImage для GBM слота передавать явные флаги:

```cpp
CVulkanTexture::createFlags slotTexFlags;
slotTexFlags.bSampled    = true;   // для чтения в shader
slotTexFlags.bStorage    = true;   // для compute shader imageStore
slotTexFlags.bTransferSrc = true;  // для vkCmdCopyImage (screencopy)
```

### 6. Targeted Vulkan Mutex (Race Condition)

**Проблема**: Screencopy path (`frame_dma_copy`) вызывает `g_device.submit()`/`wait()` из Wayland thread, конкурируя с основным render pipeline (`paint_all`) в steamcompmgr thread. Без синхронизации — GPF в `std::_Rb_tree_increment` или NULL dereference в `CVulkanCmdBuffer::reset()`.

```
Wayland thread: screencopy → submit() → m_pendingCmdBufs.push()
    ↕ RACE CONDITION
steamcompmgr:  paint_all → submit() → m_pendingCmdBufs.push()
                   → garbage_collect → m_pendingCmdBufs.erase()
```

**Решение**: `m_cmdBufMutex` в `CVulkanDevice` — lock на `commandBuffer()`, `submit()`, `resetCmdBuffers()`. Операция `wait()` (vkWaitSemaphores) оставлена **без** mutex — thread-safe по спецификации Vulkan.

### 7. Screencopy DMA-BUF Re-Import

**Проблема**: Sunshine через screencopy делает `renderer_texture_from_buffer()` на каждом кадре. Стандартный путь wlroots создаёт **новый VkImage** через `vulkan_create_texture_from_dmabuf()` — это повторный import DMA-BUF без синхронизации с оригинальным VkImage из GBMSlot. На NVIDIA с tiled modifiers это даёт нулевые пиксели.

**Решение**: Cached VkImage pattern — при создании wlr_texture helper `sunshine_get_buffer_vulkan_tex()` извлекает **оригинальный** `CVulkanTexture*` из GBMSlot и кешируется в `VulkanWlrTexture_t::cachedVulkanTex`. В render_pass используется cached VkImage, а не re-imported.

---

## Input Bridge

### Полный путь ввода

```
Moonlight (клиент)
  │  input events (mouse, keyboard)
  ▼
Sunshine (сервер)
  │  создаёт виртуальные устройства через /dev/uinput
  ▼
CLibInputHandler (gamescope-libinput thread, 1000 Hz poll)
  │  libinput_dispatch() → libinput_get_event()
  │  POINTER_MOTION, KEY, SCROLL, BUTTON
  ▼
Direct path (v2):
  │  wlserver_lock()
  │  wlserver_mousemotion(dx, dy, seq) → KWin
  │  wlserver_unlock()
  │  ⏱️ ~0.3ms задержка
  ▼
KWin (session compositor)
  │  обрабатывает motion → перемещает окна, hover, click
  ▼
Приложения (Steam, Firefox, ...)
```

### Эволюция Input Pipeline

| Версия | Архитектура | Задержка | Комментарий |
|---|---|---|---|
| **v1** | Batch: atomic CAS → VBlank Timer flush (120Hz) | ~9ms | Создан на ошибочной теории о «лавине KWin commits» |
| **v2** (текущая) | Direct: lock → `wlserver_mousemotion()` из libinput thread | **~0.3ms** | Исходная проблема лагов — BT мышь на клиенте |

### Pointer Focus для XDG окон

KWin не получает pointer events без `wlserver_mousefocus()`. В оригинальном Gamescope pointer focus устанавливается только для X11 окон. Добавлена автоматическая установка pointer focus для XDG окон в `determine_and_apply_focus()` — без этого KWin не может задать cursor через `wl_pointer.set_cursor()`.

### Настройка мыши через D-Bus

Параметры мыши (scroll factor, pointer speed, acceleration profile, natural scrolling, left-handed mode) настраиваются через D-Bus интерфейс, экспонируемый из `CLibInputHandler`. Настройки сохраняются в файл и восстанавливаются при перезапуске.

Графический интерфейс — [KCM Cloud Mouse](https://github.com/wolfam0108/kcm-cloud-mouse) (KDE System Settings → Input Devices → Cloud Mouse).

---

## Wayland Cursor Rendering

### Проблема

В оригинальном Gamescope курсор рисуется из X11 root cursor — это работает для Steam (XWayland клиент), но не для KWin (Wayland клиент). KWin задаёт курсор через `wl_pointer.set_cursor()` — Gamescope в оригинале это игнорирует.

### Решение

```
KWin: wl_pointer.set_cursor(surface, hotspot_x, hotspot_y)
    │
    ▼ request_set_cursor handler (wlserver.cpp)
    │
wlserver.wayland_cursor_surface = event->surface    (24×24 Breeze cursor)
wlserver.wayland_cursor_hotspot_x = event->hotspot_x (3)
wlserver.wayland_cursor_hotspot_y = event->hotspot_y (1)
wlserver.wayland_cursor_changed = true
    │
    ▼ paint_all() [steamcompmgr.cpp]
    │
Условие: inputFocusWindow->type == XDG && ShouldDrawCursor()
    │
    ▼ vulkan_create_texture_from_wlr_buffer(cursor_surface)
      → Vulkan текстура из SHM буфера
      → cursor layer с позицией (mouse_x - hotspot_x, mouse_y - hotspot_y)
      → zpos = g_zposCursor (поверх всех слоёв)
      → alpha blending: PREMULTIPLIED
```

**Важно**: чтение cursor данных — **lock-free** (аналогично `mouse_surface_cursorx/y`). `waylock` — non-recursive mutex, его нельзя брать внутри `paint_all()` — это вызовет deadlock.

---

## Adaptive Frame Signal

### Direct Mode (OPT-1, default)

`wlr_output_send_frame()` вызывается **напрямую** из eventfd handler — 0ms задержка между Present() и посылкой кадра в screencopy.

VBlank Timer оставлен **только** для:
- **Heartbeat**: если 500ms без новых кадров → принудительный repaint (idle recovery)
- **Статистика**: каждые 5 секунд → лог fps/drops

### Режимы

Переключение через `GAMESCOPE_SUNSHINE_VBLANK_MODE`:

| Режим | Frame signal | Задержка | Когда использовать |
|---|---|---|---|
| `direct` / `0` | Из eventfd handler | **0ms** | Default — максимальная производительность |
| `4x` / `4` | Из VBlank Timer 240 Hz | 4ms | Fallback при проблемах |
| `2x` / `2` | Из VBlank Timer 120 Hz | 8ms | Fallback |
| `1x` / `1` | Из VBlank Timer 60 Hz | 16ms | Legacy / совместимость |

### Force Repaint

KWin рендерит ~1 fps без user activity (курсор не двигается, нет анимаций). Для стабильного потока кадров:

```bash
GAMESCOPE_SUNSHINE_FORCE_REPAINT=60  # принудительная отрисовка 60 fps
GAMESCOPE_SUNSHINE_FORCE_REPAINT=10  # 10 fps (для отладки)
GAMESCOPE_SUNSHINE_FORCE_REPAINT=0   # отключено (default)
```

---

## Shutdown / Reboot через KDE

### Проблема

В LXC контейнере кнопки «Выключение» и «Перезагрузка» в Plasma по умолчанию не работают по двум причинам:
1. **D-Bus activation** — greeter (`ksmserver-logout-greeter`) запускается через D-Bus без env vars (`DISPLAY`, `WAYLAND_DISPLAY`) → crash
2. **Polkit** — `systemd-logind` требует авторизацию для poweroff/reboot

### Решение

```
Plasma: кнопка "Выключение"
  │
  ▼ D-Bus: activate org.kde.LogoutPrompt
  │
ksmserver-logout-greeter-wrapper:
  │  source /tmp/cloud-desktop/desktop_env
  │  (теперь имеет DISPLAY, WAYLAND_DISPLAY, QT_QPA_PLATFORM)
  │
  ▼ exec ksmserver-logout-greeter
  │  Диалог подтверждения → пользователь нажимает "Выключить"
  │
  ▼ systemd-logind: PowerOff()
  │  polkit: 50-cloud-desktop.rules → YES (без пароля)
  │
  ▼ systemd: poweroff
    ExecStop → cloud-desktop-service.sh stop
    (корректное завершение всех компонентов)
```

---

## Steam и игры

Steam запускается как обычное приложение внутри KDE Plasma:

- **Native Linux игры** — работают напрямую через Vulkan/OpenGL → XWayland
- **Proton/Wine** — работает через XWayland, gamescope в режиме Sunshine не конфликтует
- **Remote Play** — Steam видит KDE desktop и позволяет стримить через свой собственный протокол (поверх Moonlight)
- **Gamepad** — Sunshine создаёт виртуальный контроллер через uinput → доступен в Steam

---

## Environment Variables

| Переменная | Описание | По умолчанию |
|---|---|---|
| `GAMESCOPE_SUNSHINE_VBLANK_MODE` | Режим frame signal (`direct`, `4x`, `2x`, `1x`) | `direct` |
| `GAMESCOPE_SUNSHINE_FORCE_REPAINT` | Принудительная частота отрисовки, Hz | `0` (off) |
| `GAMESCOPE_SUNSHINE_DUMP_FRAME` | Dump PPM скриншотов для верификации | `0` |
| `GAMESCOPE_SUNSHINE_CURSOR_TEST` | Тест позиционирования курсора (warp + dump) | `0` |

---

## Компоненты проекта

| Компонент | Репозиторий | Описание |
|---|---|---|
| **Gamescope (этот форк)** | Вы здесь | Sunshine backend, протоколы, input pipeline |
| **KCM Cloud Mouse** | [kcm-cloud-mouse](https://github.com/wolfam0108/kcm-cloud-mouse) | KDE модуль настроек мыши |
| **Sunshine** | [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) | Стриминг-сервер (не модифицируется) |
| **Moonlight** | [moonlight-stream.org](https://moonlight-stream.org/) | Клиент для подключения |

---

## Изменённые файлы

Все модификации сосредоточены в Gamescope:

| Файл | Изменение |
|---|---|
| `src/Backends/SunshineBackend.cpp` | **NEW** — полный backend: GBM slot pool, Vulkan render, eventfd bridge, screencopy, PPM dump |
| `src/backends.h` | Enum `Sunshine` + forward declaration |
| `src/main.cpp` | CLI парсинг `--backend sunshine` |
| `src/meson.build` | Добавлен SunshineBackend.cpp |
| `src/wlserver.hpp` | Поля sunshine_* (eventfd, slots, lock, output), wayland cursor (surface, hotspot, listener) |
| `src/wlserver.cpp` | Subsurface forwarding, XDG configure, cursor handler, eventfd loop, input D-Bus, VBlank Timer |
| `src/steamcompmgr.cpp` | Dummy buffer filter, cursor layer, pointer focus, force repaint, frame callback bootstrap |
| `src/rendervulkan.hpp` | createFlags параметр для DMA-BUF import, `m_cmdBufMutex` |
| `src/rendervulkan.cpp` | Targeted mutex (submit/reset), screencopy cached VkImage helper |
| `src/LibInputHandler.cpp` | Direct motion path, scroll factor/speed D-Bus config, mouse settings persistence |

---

## Архитектурные ограничения

Проект следует строгим архитектурным принципам:

| ID | Ограничение | Обоснование |
|---|---|---|
| **P1** | Только Wayland | KDE Plasma через KWin Wayland с GPU-ускорением |
| **P2** | Захват полного десктопа | Стриминг всех окон, включая XWayland |
| **P3** | Полностью в VRAM | Zero-copy DMA-BUF, ни одного пикселя в RAM |
| **P4** | Без DRM Master | Работа в LXC контейнере без ядерных модулей |
| **P5** | Изменения только в Gamescope | Sunshine и KWin не модифицируются |
| **P6** | Свой Sunshine backend | Вместо модификации Sunshine — свой backend в Gamescope |

---

## Установка

Подробное руководство по установке в LXC-контейнере Proxmox:

📖 **[INSTALL-LXC.md](INSTALL-LXC.md)** — пошаговая инструкция (18 разделов): создание контейнера, NVIDIA драйвер, сборка, KDE Plasma, Sunshine, автозапуск.

### Быстрый старт (если среда уже настроена)

```bash
# Сборка
git clone --recurse-submodules https://github.com/wolfam0108/Cloud-Desktop.git gamescope
cd gamescope
meson setup -Dpipewire=disabled build .
ninja -C build -j$(nproc)
sudo ninja -C build install

# Запуск
gamescope --backend sunshine --expose-wayland -W 1920 -H 1080 -r 60
```

---

## Upstream

Форк основан на [Gamescope 3.16.20](https://github.com/ValveSoftware/gamescope/releases/tag/3.16.20) от Valve Software.

Gamescope — микрокомпозитор для SteamOS, лицензия BSD-2-Clause.

---

## Лицензия

[BSD-2-Clause](LICENSE) — как и оригинальный Gamescope.
