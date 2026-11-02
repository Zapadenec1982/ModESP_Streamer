[English](PANEL.md) · **Українська**

# Панель: екран, тач, енкодер, плата

Як залізо Guition JC4880P443C заходить у прошивку: секції `boards/p4_player/board.json` → HAL
(лише конфігурація) → драйвери (володіють залізом) → модуль `ui_lvgl` через шов
`components/modesp_hal/include/modesp/hal/lcd_port.h`. Екрани, навігація, мови й шрифти —
[UI.md](UI.uk.md); аудіо-шини й вихід — [AUDIO.md](AUDIO.uk.md); лінк із сателітом — [LINK_PROTOCOL.md](LINK_PROTOCOL.uk.md);
розпіновка JP1 і J5 — [CONCEPT.md](CONCEPT.uk.md), §4.

## Плата

| Вузол | Що | Де задано |
|---|---|---|
| SoC | ESP32-P4, ревізія v1.3; PSRAM 32 МБ (hex, 200 МГц); flash 16 МБ | `board.json` (`description`), `boards/p4_player/sdkconfig.board`, `partitions_p4_16mb.csv` |
| Wi-Fi | ESP32-C6 по SDIO: слот 1, 4 біти, 40 МГц, CLK 18, CMD 19, D0–D3 14–17, reset GPIO54 (LOW = скидання) | `sdkconfig.board`, [C6_UPDATE.md](C6_UPDATE.uk.md) |
| Екран | 4,3″ 480×800 ST7701S, MIPI-DSI 2 лінії | `board.json` `dsi_displays` |
| Тач | GT911 на I2C | `board.json` `i2c_touch` |
| microSD | слот 0 контролера SDMMC | `board.json` `sdmmc_slots` |
| Бортовий кодек | ES8311 — лише як мікрофон | [MIC_CAPTURE.md](MIC_CAPTURE.uk.md) |

P4 має один контролер SDMMC на два слоти: слот 1 ініціалізує ESP-Hosted для C6, тому драйвер
`sdmmc_card` підставляє порожні `host.init` / `host.deinit` — інакше збій монтування картки знімав би
і лінк із C6.

## Шов `lcd_port.h`

- `ILcdPanel` — панель: `init()`, `geometry()`, esp_lcd `panel()` / `io()`, `set_backlight(%)`,
  `is_healthy()`.
- `ITouchPort` — тач-контролер: `init()`, `handle()` для `lvgl_port_add_touch`.
- `ILcdPort` — те, що повертає фабрика драйвера категорії `lcd`; `as_panel()` / `as_touch()` кажуть,
  яку з двох ролей драйвер реалізує (один драйвер = одна роль = один hardware id).

Модуль не знає пінів, таймінгів і init-таблиць — лише отримує esp_lcd-хендли для `esp_lvgl_port`.
Це окремий шов від `display_port.h` (символьні OSD-дисплеї) і `panel::IPanelPort` (BLE LED-панель).

## `board.json`: панельні секції

| Секція | Запис |
|---|---|
| `i2c_buses` | `i2c_0` «ES_I2C»: SDA 7, SCL 8, 400 кГц; спільна з кодеком, тачем і JP1 (піни 21/23) |
| `dsi_displays` | `lcd_0`: 2 лінії по 500 Мбіт/с, DPI 34 МГц, 480×800, hsync 12 / hbp 42 / hfp 42, vsync 2 / vbp 8 / vfp 166, `color_bits` 24 (RGB888), reset GPIO5, підсвітка GPIO23 (LEDC 20 кГц, active high), LDO PHY канал 3, 2500 мВ, `rotation` 270 |
| `i2c_touch` | `touch_0`: GT911 @0x5D на `i2c_0`; INT і RST не розведені (−1) — тач опитується; `x_max` 800, `y_max` 480; `swap_xy`, `mirror_x`, `mirror_y` — `false` |
| `sdmmc_slots` | `sd_0`: слот 0, 4 біти, CLK 43, CMD 44, D0–D3 39–42, без CD, 20 МГц, LDO канал 4, 3300 мВ, точка монтування `/sd` |
| `encoders` | `enc_0`: A (`clk`) GPIO32, B (`dt`) GPIO28, підтяжка вгору |
| `gpio_inputs` | `enc_sw`: GPIO35, підтяжка вгору, інверсія (натиснуто = LOW); strapping BOOTMODE, R34 10k вгору |
| `gpio_outputs` | `amp_enable`: GPIO34, active high — TPA3255 /RESET і XSMT ЦАП (LOW = mute, зовнішній pull-down); HAL ставить її в OFF на старті, вмикає модуль `audio` |

Секції `i2s_buses` і `uart_buses` — [AUDIO.md](AUDIO.uk.md), [LINK_PROTOCOL.md](LINK_PROTOCOL.uk.md).

`bindings.json`: `lcd_0 → dsi_st7701` (роль `lcd_main`), `touch_0 → gt911` (`touch_main`),
`enc_0 → rotary_encoder` (`encoder_main`), `enc_sw → digital_input` (`encoder_sw`) — усі в модуль
`ui_lvgl`; `amp_enable → gpio_output` — у `audio`; `sd_0 → sdmmc_card` — у `storage`.

Обмеження файлів:

- `board.json` і `bindings.json` читаються в один буфер `ConfigService::MAX_JSON_SIZE` (4096 байт,
  `components/modesp_services/include/modesp/services/config_service.h`). Більший файл збірка
  відкидає (і друкує запас обох). На пристрої такий файл відкидається з ERROR `ConfigSvc`, що
  називає ліміт; `config` — CRITICAL-модуль, тож панель іде в безпечний режим: без HAL, аудіо й
  екрана, лише мережа й HTTP для ремонту. Пояснення — у `docs/`, не в `label`.
- Збірка копіює обидва файли в `data/`, а `data/` іде в `data.bin` розділу `data` (0x430000), не в
  застосунок. `idf.py app-flash` зміну `board.json` на панель не доносить — потрібна повна
  `idf.py flash`.

## Орієнтація: альбомна 800×480

`rotation: 270` — UI альбомний 800×480; мікрофон плати при цьому біля верхнього краю картинки.
Поворот екрана (`rotation`) і прапорці тача (`swap_xy` / `mirror_*`) — окремі налаштування
`board.json`.

**Екран.** DPI-панель не міняє осі сама (`esp_lcd_panel_swap_xy` на ній не підтримується), тому
повертає LVGL (`flags.sw_rotate` + `lv_display_set_rotation()`). Модуль ставить власний flush
(`modules/ui_lvgl/src/ppa_flush.cpp`): PPA повертає кожен блок прямо у фреймбуфер DPI (розділ
«Шлях кадру»). Якщо PPA-клієнт чи фреймбуфер недоступні, лишається flush `esp_lvgl_port` —
з PPA при `CONFIG_LVGL_PORT_ENABLE_PPA=y` (`sdkconfig.board`), інакше на CPU. `sdkconfig.board`
засіває лише **новий** `sdkconfig`: старий рядок `# CONFIG_… is not set` у наявному `sdkconfig`
перемагає. Що реально взялося — `build/config/sdkconfig.json`.

**Тач.** `esp_lvgl_port` тач не повертає, а LVGL 9.3 повертає: `indev_pointer_proc()`
(`src/indev/lv_indev.c`) крутить точку кожного pointer-пристрою на поворот його дисплея. GT911 віддає
координати в системі матриці, тож тач іде за `rotation` сам, а прапорці GT911 (`swap_xy` /
`mirror_*`) потрібні лише тоді, коли тач не збігається з матрицею. Стендовий `tap=` (`/api/screenshot`)
проходить той самий шлях у зворотному напрямку.

Перевірка після будь-якої зміни орієнтації: натиснути чотири кути й подивитися `ui.touch_x` /
`ui.touch_y` у `GET /api/state` (або рядок `UiLvgl: touch x,y`, розділ «Перевірка на стенді») —
має вийти прямокутник близько (0,0)…(799,479).

## Драйвери

- `drivers/dsi_st7701` (`espressif/esp_lcd_st7701` ^2.0.2, MIPI-DSI): LDO PHY → DSI-шина → DBI →
  панель з init-таблицею вендора (BSP csvke, Apache-2.0) → reset → init → display on → LEDC-підсвітка
  (10 біт, 0 % до першої команди модуля). Два фреймбуфери DPI у PSRAM, копіювання DMA2D.
- `drivers/gt911` (`espressif/esp_lcd_touch_gt911` ^1.2.1): пристрій на `i2c_0`; без чіпа `init()`
  повертає false і модуль працює без тача. `rst_gpio` / `int_gpio` з `board.json` драйвер приймає.
- `drivers/rotary_encoder`: PCNT, 2 канали (обидва фронти обох ліній — 4 імпульси на клацання EC11),
  glitch-фільтр 1 мкс, 32-бітний накопичувач в `update()`, лічильник перецентровується до
  переповнення; `read()` віддає клацання. Налаштування binding: `steps_per_detent` (4), `invert`.
- `drivers/digital_input` (кнопка енкодера), `drivers/gpio_output` (`amp_enable`).

## Шлях кадру

Модуль `ui_lvgl` (потоки, сторінки, стан `ui.*`, Kconfig, HTTP-маршрути) — [UI.md](UI.uk.md). Тут —
те, що стосується заліза панелі; шляхи — відносно `modules/ui_lvgl/`.

- Задача LVGL (`esp_lvgl_port` 2.9, LVGL 9.3) — ядро 0, пріоритет 1, стек 16 КБ у PSRAM
  (`CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`; задача не пише й не стирає flash, тож стек у PSRAM
  допустимий). Heap LVGL у PSRAM (`CONFIG_LV_USE_CUSTOM_MALLOC` + `src/lv_mem_psram.c`).
  `LV_COLOR_DEPTH` = 24, як `color_bits`.
- Два draw-буфери по 480×100 RGB888 у PSRAM (`buff_dma` вимкнено: порт приймає DMA-буфери лише для
  RGB565; PSRAM на P4 і так досяжна для DMA).
- PPA (SRM, неблокувально) повертає кожен блок прямо у фреймбуфер DPI, поки LVGL малює наступний у
  другий draw-буфер (`src/ppa_flush.cpp`). Блоки, які PPA на v1.3 не завершує цілими, ідуть смугами
  (`include/ppa_geometry.h`), найвужчі повертає CPU.
- `CONFIG_UI_DOUBLE_FB` (типово `y`): кадр збирається у фреймбуфері, якого немає на склі, і DPI
  перемикається на нього, коли кадр готовий, — розривів немає. Другий буфер DMA2D доганяє копією;
  прокручуваний список зсувається DMA2D всередині фреймбуфера. Таймери оновлення дисплея й
  опитування тача — 15 мс (кадр DPI ≈ 16,5 мс на 34 МГц); на кадрах зсуву задача LVGL піднімається
  до `CONFIG_UI_SHIFT_PRIORITY` (типово 2). `avoid_tearing` порту — 0, не використовується.
- Підсвітка: LEDC 20 кГц, 10 біт, 0 % до першої команди модуля; рівень — `ui.backlight`,
  `ui.standby_bl` ([UI.md](UI.uk.md), «Чергування»).

## Перевірка на стенді

Рядки нижче — INFO. Штатна збірка на WARN (`CONFIG_LOG_DEFAULT_LEVEL_WARN`), тож стартові рядки
видно лише на збірці з `CONFIG_LOG_DEFAULT_LEVEL_INFO` у локальному `sdkconfig`. На працюючій панелі
тег відкривається без перепрошивки, але лише для подальших подій:

```
curl -u <логін>:<пароль> -X POST -H 'Content-Type: application/json' http://<панель>/api/loglevel -d '{"tag":"UiLvgl","level":"info"}'
```

1. Старт:
   - `DsiSt7701: [lcd_main] 480x800 RGB24, 2 lanes @ 500 Mbps, DPI 34 MHz, reset GPIO 5, backlight GPIO 23`
   - `Gt911: [touch_main] GT911 @0x5D on 'i2c_0' (400 kHz), 800x480, int=-1 rst=-1, swap=0 mx=0 my=0`
   - `RotaryEnc: [encoder_main] PCNT on GPIO 32/28, 4 edges/detent`
   - `UiFlush: PPA rotates straight into the frame buffer (480x800, RGB888 in, RGB888 out, two buffers, lists shifted)`
   - `UiLvgl: Initialized: panel 480x800, UI 800x480 (rotation 270), touch yes, theme '…'`
   - `UiLvgl: heap before lvgl_port_init` / `heap after LVGL screen` — LVGL, draw-буфери й
     фреймбуфери мають сидіти в PSRAM, а не з'їдати DMA.

   На рівні WARN видно відхилення: `UiFlush: … frames are drawn on the glass` (без другого
   фреймбуфера), `UiFlush: no PPA client`, `UiLvgl: touch init failed — continuing without touch`,
   `UiLvgl: panel init failed — screen disabled`.
2. Ручка: поворот — `audio.volume` у `GET /api/state` міняється на 2 за клацання і з'являється
   повзунок гучності; натиск — пауза / відтворення (на станції — вимкнення звуку); утримання — меню
   ([UI.md](UI.uk.md), «Введення»).
3. Тач: кільце 30 px іде за пальцем; `ui.touch_x` / `ui.touch_y` і рядок `UiLvgl: touch x,y`;
   чотири кути — розділ «Орієнтація».
4. Знімок екрана: `curl -u <логін>:<пароль> -o shot.bmp http://<панель>/api/screenshot`.

## Обмеження

- GT911 INT і RST у `board.json` не розведені: тач опитується таймером LVGL.
- FAULT і CLIP_OTW підсилювача TPA3255 не заведені: у `board.json` немає для них входів, прошивка їх
  не читає. Драйвер `drivers/pcf8574_input` у дереві є, у `bindings.json` не прив'язаний.
