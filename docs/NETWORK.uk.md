[English](NETWORK.md) · **Українська**

# Мережа

Wi-Fi через ESP32-C6, HTTP-сервер з REST API і WebSocket, ім'я пристрою і mDNS, інтернет-радіо,
DLNA-рендерер і браузер медіасерверів, приймач AirPlay, MQTT.

Суміжне: значення `audio.source` і контракт SharedState — [CONCEPT.uk.md](CONCEPT.uk.md), §7; обмеження
плати й DMA-пам'ять — [CONCEPT.uk.md](CONCEPT.uk.md), §8, §8а і §10; кільце PCM, частоти, конвертер частоти —
[AUDIO.uk.md](AUDIO.uk.md); сторінки панелі й вебу — [UI.uk.md](UI.uk.md); оновлення прошивки C6 —
[C6_UPDATE.uk.md](C6_UPDATE.uk.md).

| Частина | Код |
|---|---|
| Wi-Fi, HTTP, WebSocket, mDNS | `components/modesp_net` |
| ESP-Hosted, копії кадрів у PSRAM | `espressif/esp_hosted`, `espressif/esp_wifi_remote`, `components/hosted_mem` |
| DNS для SNTP | `components/lwip_dns_compat` |
| Радіо: потік, список, каталог, обкладинки | `modules/radio`, `components/stations`, `components/radio_catalog`, `components/audio_pipe` |
| DLNA-рендерер | `modules/dlna`, `components/esp_dlna`, `espressif/esp_media_protocols` |
| Браузер медіасерверів | `components/dlna_client` |
| AirPlay | `modules/airplay`, `components/airplay2` |
| MQTT | `components/modesp_mqtt` |
| Ім'я пристрою | `modules/bt/src/device_name.cpp` |
| Файли користувача з вебу | `main/userdata_http.cpp` |

Мережу вмикає `CONFIG_MODESP_NET_ENABLE` (типово `y`); без неї `modesp_net` збирається без
вихідних файлів, а main.cpp не підключає Wi-Fi, HTTP і WebSocket.

## Wi-Fi через ESP32-C6

### Лінк

У ESP32-P4 немає радіо. Wi-Fi — на ESP32-C6, з'єднаному по SDIO; на P4 `esp_wifi_remote` поверх
`esp_hosted` дає звичайний API `esp_wifi_*`.

| Параметр | Значення (`boards/p4_player/sdkconfig.board`) |
|---|---|
| Шина | SDIO slot 1, 4 біти, 40 МГц, RX streaming mode |
| Піни P4 | CLK 18, CMD 19, D0–D3 14–17 |
| Скидання C6 | GPIO54 → CHIP_PU, активний LOW (`CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW`) |
| Задачі ESP-Hosted | пріоритет 21 (`CONFIG_ESP_HOSTED_HOST_DEFLT_TASK_PRIORITY`): вище lwIP (18) і подій (20), нижче аудіозадачі (22) |
| Версії | `esp_hosted` 3.0.7, `esp_wifi_remote` 1.6.4, обидві `==` |

Прошивка C6 збирається з того самого релізу ESP-Hosted, що й хост-бібліотека; образ лежить у
розділі `slave_fw` (0x630000, 2 МБ), на C6 його передає `tools/c6_updater`. Розбіжність версій лог
показує як `eh_init_evt: major version mismatch — OTA coprocessor from host` (ERROR) або
`minor version mismatch` (WARN). Процедура і відновлення через UART C6 —
[C6_UPDATE.uk.md](C6_UPDATE.uk.md). Версія записана в кількох місцях, які змінюються разом:

- `components/modesp_net/idf_component.yml`, `tools/c6_updater/main/idf_component.yml`,
  `tools/sdio_soak/main/idf_component.yml` — обидва пакети;
- підпис `C6 3.0.7` у рядку «Сервіс» налаштувань панелі — константа в
  `modules/ui_lvgl/src/screen_set.cpp`, а не версія, прочитана з C6;
- типове значення `--hosted` у `tools/c6_image.py`;
- `components/hosted_mem/CMakeLists.txt` звіряє прототип `eh_host_port_dma_alloc()` у вихідниках
  ESP-Hosted і зупиняє збірку, коли той змінився.

### Пам'ять

Внутрішня DMA-здатна SRAM — найдефіцитніший ресурс: її ділять приймальний пул ESP-Hosted,
дескриптори I2S і все, що просить DMA ([CONCEPT.uk.md](CONCEPT.uk.md), §8).

- `components/hosted_mem` (`CONFIG_HOSTED_MEM_FRAMES_IN_PSRAM`, типово `y`) обгортає
  `eh_host_port_dma_alloc()` (`-Wl,--wrap`): копії кадрів, що перетинають SDIO, ESP-Hosted бере з
  PSRAM. Без неї кадр, для якого не знайшлося внутрішньої DMA-пам'яті, губиться без повідомлення
  (на прийомі) або як `ESP_ERR_NO_MEM` для lwIP (на передачі). Лічильники —
  `hosted_mem_get_stats()`.
- `CONFIG_EH_HOST_PORT_DMA_PREFER_SPIRAM`: транспортні буфери ESP-Hosted — у PSRAM.
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`: буфери lwIP і
  звичайні виділення понад 1 КБ (кодеки, HTTP) — у PSRAM.
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`: арена mbedTLS — у PSRAM. Вхідний буфер TLS 16 КБ
  (`MBEDTLS_SSL_IN_CONTENT_LEN`): сервери станцій шлють записи по 16 КБ і ігнорують
  `max_fragment_length`, а запис, що не вміщується в буфер, не розшифровується. Вихідний — 4 КБ.
- Стеки задач радіо, DLNA, браузера медіасерверів і обкладинок — у PSRAM. Внутрішній стек мають
  задачі, що пишуть у flash (`ap_start`: ключі HAP у NVS), і задача SSDP: задача зі стеком у PSRAM
  не може виконуватися, поки кеш flash вимкнений.

### lwIP

| Параметр | Значення | Причина |
|---|---|---|
| `LWIP_TCP_WND_DEFAULT` | 11 520 (8 × MSS) | з'єднання несе щонайбільше одне вікно за RTT, і менше вікно не тягне 320 кбіт/с від сервера за ~200 мс; вікно від 10 × MSS уже забирає DMA-пам'ять, потрібну приймальному пулу |
| `LWIP_TCP_RECVMBOX_SIZE` | 10 | сегменти одного вікна і два понад |
| `LWIP_UDP_RECVMBOX_SIZE`, `LWIP_SO_RCVBUF` | 64, `y` | потоки AirPlay: realtime ALAC — 125 пакетів/с |
| `LWIP_MAX_SOCKETS` | 36 | HTTP (10 сесій + 3 власні сокети httpd), SSDP, MQTT, радіо з підготовкою наступного треку, обкладинки, каталог, браузер DLNA, NOTIFY, OTA, сесія AirPlay (до 10 сокетів) |
| `LWIP_MAX_ACTIVE_TCP` | 32 | жорстка межа ESP-lwIP: `socket()` для TCP-клієнта відмовляє, коли вичерпано |
| `LWIP_DNS_MAX_HOST_IP` | 4 | радіо переходить на іншу адресу хоста станції |
| `LWIP_TCPIP_TASK_AFFINITY_CPU0` | `y` | ядро 1 — аудіозадачі |

`components/lwip_dns_compat`: з кількома адресами на ім'я lwIP при влученні в кеш копіює їх усі
в буфер того, хто питає, а SNTP-клієнт lwIP дає місце для однієї. Обгортка
`dns_gethostbyname()` (`-Wl,--wrap`) резолвить у масив і віддає першу адресу.

### WiFiService

`components/modesp_net/src/wifi_service.cpp`. Є `ssid` у NVS-просторі `wifi` — режим STA, немає —
точка доступу.

**STA.**

- Поріг безпеки — WPA2-PSK, коли є пароль; без пароля — відкрита мережа (з порогом WPA2 відкриту
  мережу не приєднати). Панель і веб приєднують `open`, `WPA2`, `WPA/WPA2`, `WPA3`, `WPA2/WPA3`.
- Країна UA: канали 1–13, до 20 дБм, після `esp_wifi_start()`.
- `esp_netif_set_default_netif()` на STA (`CONFIG_MODESP_NET_SET_DEFAULT_NETIF`, типово для P4):
  без цього незв'язані UDP-сокети (SNTP) не виходять через інтерфейс ESP-Hosted.
- Розрив: три повторні спроби через 2, 4 і 8 с; ще 16 с без адреси — точка доступу.
- Сумарно 10 хв без з'єднання — `esp_restart()`; година стабільного з'єднання скидає лічильник.

**Точка доступу.** SSID `ModESP-XXXX` (два останні байти базової MAC) або `wifi/ap_ssid`; пароль
`wifi/ap_pass` з 8–63 символів → WPA2 (довший збережений обрізається до перших 63, з ERROR
у лозі), інакше відкрита; канал `wifi/ap_chan` (1–13, типово 1); до
2 клієнтів; адреса 192.168.4.1. Зі збереженою мережею AP пробує STA (APSTA, 15 с на спробу): перша
спроба через 30 с, далі інтервал подвоюється до 5 хв; спроба пропускається, коли вільної
внутрішньої RAM < 50 КБ. Вдала спроба переводить у чистий STA.

**Приєднання** з панелі й вебу — `WiFiService::try_connect()`: стара пара лишається в пам'яті,
нова пишеться в NVS лише після отримання адреси. Відмова мережі (`AUTH_FAIL`, `AUTH_EXPIRE`,
тайм-аут рукостискання, немає AP із сумісною безпекою чи в порозі) завершує спробу одразу; 25 с без
адреси або три невдалі повтори — повертається стара пара, а коли її не було — точка доступу.

**Сканування.** Активне, 120–500 мс на канал, без прихованих мереж, до 20 результатів (записи в
PSRAM); потрібно ≥ 50 000 Б вільної внутрішньої RAM; у режимі AP на час сканування — APSTA.
Сканування блокує того, хто його викликав: `GET /api/wifi/scan` — задачу HTTP-сервера. Одне
сканування за раз і жодного під час підключення чи спроби AP → STA: такий запит не отримує списку
(`{"ok":false}` у вебі, невдале сканування на панелі), а підключення чи спроба, що прийшли під час
сканування, чекають його кінця.

**RSSI не читається.** `esp_wifi_sta_get_ap_info()` тут — синхронний RPC до C6 з тайм-аутом 5 с,
а головний цикл годує 5-секундний watchdog; до того ж ESP-Hosted 3.0.7 повертає в ньому rssi 0.
`wifi.rssi` не пишеться; рівень сигналу мереж дає сканування.

**Modem sleep:** `WIFI_PS_NONE`, поки радіо грає станцію чи URL (від налаштування до стопу або
зупинки задачі радіо), `WIFI_PS_MIN_MODEM` — поза цим (`modules/radio`, `set_modem_sleep()`): зі
сплячим модемом лінк C6 губить пакети під потоком.

| Ключ | Значення |
|---|---|
| `wifi.connected` | є адреса в STA |
| `wifi.mode` | `sta` / `ap` |
| `wifi.ssid` | мережа STA або SSID точки доступу |
| `wifi.ip` | адреса; у режимі AP — 192.168.4.1 |
| `wifi.reason` | код причини останнього розриву (`wifi_err_reason_t`) |
| `wifi.try`, `wifi.try_ssid` | спроба приєднання: `connecting` / `ok` / `fail`, порожньо — немає |
| `system.mdns_hostname` | ім'я mDNS |

NVS-простір `wifi`: `ssid`, `pass`, `ap_ssid`, `ap_pass`, `ap_chan`.

### Час

SNTP, `pool.ntp.org`, режим poll. NVS `time/ntp_enabled` (типово true), `time/timezone` (POSIX TZ,
типово `EET-2EEST,M3.5.0/3,M10.5.0/4`). Ручний час і пояс — `POST /api/time`. `system.time` і
`system.date` до синхронізації показують `--:--:--` і `--.--.----`.

## Ім'я пристрою і mDNS

**Ім'я пристрою** одне для Bluetooth, AirPlay, DLNA, USB-аудіо й заголовка шухляди панелі
(`modules/bt/include/device_name.h`):

- NVS `devname/name`, 1–32 байти UTF-8 — правило супутника (`BT_CONFIG_SET`,
  [LINK_PROTOCOL.uk.md](LINK_PROTOCOL.uk.md)); поки імені немає — `P4 Player` (`device_name::FALLBACK`,
  те саме, що `system.device_name` у `project.json`). Збережене ім'я, що порушує правило, рахується
  як відсутнє.
- Задає поле «Ім'я» на сторінці Bluetooth панелі чи вебу (`POST /api/bt`, `{"name":…}`, на ім'я
  не в UTF-8 — `400`): ім'я йде на супутник і зберігається в NVS. Потрібен супутник, що віддає
  налаштування (`DEV_CAP_BT_SETTINGS`); без нього `POST /api/bt` відповідає `409`.
- Перше ім'я, яке повідомив супутник, зберігається, коли свого ще немає; супутникові з іншим
  ім'ям панель передає своє, один раз за з'єднання: супутник, що й після цього повідомляє своє,
  лишається з ним.
- Bluetooth бере нове ім'я одразу; AirPlay, DLNA і USB-аудіо читають його під час старту — після
  перезапуску.
- DLNA оголошує його як friendlyName (`dlna.name`), AirPlay — як ім'я в списку (`airplay.name`) і
  RAOP-сервіс `<MAC>@<ім'я>`.

**mDNS** (`espressif/mdns`): hostname `modesp-xxxx` (два останні байти MAC, hex малими літерами) —
не ім'я пристрою; instance `ModESP Controller`; сервіс `_http._tcp`, порт 80, TXT `path=/`.
Стартує після отримання адреси в STA і в режимі AP; при переході AP → STA перезапускається, і
AirPlay оголошує свої сервіси заново. Ініціалізацію й звільнення mDNS і реєстрацію сервісів
серіалізує `MdnsGuard` (`components/modesp_net/src/mdns_guard.cpp`): задача подій Wi-Fi
перебудовує mDNS паралельно з головним циклом.

## HTTP-сервер

`HttpService` (`components/modesp_net/src/http_service.cpp`) над `esp_http_server`.

| Параметр | Значення |
|---|---|
| Порт | 80 |
| Сесії | `CONFIG_MODESP_HTTPD_MAX_OPEN_SOCKETS` = 10 (`sdkconfig.board`), WebSocket і HTTP разом; плюс 3 власні сокети httpd мають вміститися в `LWIP_MAX_SOCKETS` — інакше збірка зупиняється (`static_assert`) |
| Обробники | `max_uri_handlers` = 88 |
| Задача | одна, стек 8 КБ; обробники виконуються по черзі |
| Тайм-аути | прийом 30 с, передача 10 с |
| Усі сесії зайняті | `lru_purge_enable`: нова сесія закриває ту, яку найдовше не чути |

10 сесій — це три WebSocket-клієнти, до шести з'єднань браузера на хост і одне для контрол-поінта
чи curl: WebSocket, що лише слухає, чути раз на 5 с (PONG), тож коли сесій не вистачає,
завантаження сторінки закриває живий WebSocket.

Одна задача на весь сервер: повільний обробник (сканування Wi-Fi, OTA, великий POST) тримає і
HTTP, і розсилку WebSocket. Буфери обробників понад кілька кілобайтів — у PSRAM або статичні, не
на 8-КБ стеку.

### Статичні файли

Wildcard `/*` віддає `/data/www/` з розділу `data` — веб-збірка лежить у `data.bin`, не в образі
програми. `<шлях>.gz` має перевагу (`Content-Encoding: gzip`, `Cache-Control: no-store`), інакше
файл як є (`no-cache`); невідомий шлях — `index.html` (маршрути SPA). URI довший за 51 символ —
404, `..` у шляху — 400. `/radio.html` переадресовує на `/#/stations/top`.

### Порядок маршрутів

httpd повертає перший шаблон, що збігся, і тим самим matcher-ом (`httpd_uri_match_wildcard`)
перевіряє дублікати під час реєстрації: поки `/*` зареєстрований, кожна нова GET-реєстрація
відхиляється як `already registered`. Тому порядок такий:

1. `HttpService::on_init()` — маршрути `/api/*` фреймворку;
2. main.cpp — `/ws`, `/api/mqtt`, потім `modesp::http_routes::run_all()`: маршрути модулів, які
   ті записали через `http_routes::add()` в `on_init()` (до 8 реєстраторів,
   `modesp/net/http_routes.h`);
3. останнім — `/*`.

Підсистема, що реєструється пізніше (рендерер DLNA після появи мережі), знімає wildcard
(`http_routes::wildcard(false)`) і ставить назад. Тим часом задача сервера стоїть між двома
запитами: httpd змінює таблицю обробників без блокування проти власних пошуків, тож запит, що
прийшов у цей час, чекає, доки маршрути стануть цілими, а не отримує 404 чи напівготовий слот.
Не вмістився `/*` у таблицю обробників — жодна сторінка не відкривається, API відповідає; лог
`HTTP: Static file handler NOT registered`.

### Автентифікація

HTTP Basic, realm `ModESP`. NVS-простір `auth`: `user`, `pass`, `enabled`; типово `admin` /
`modesp`, увімкнено. Після 5 невдалих спроб поспіль 30 с кожен запит із перевіркою отримує `429`
(лічильник один на пристрій). Маршрути модулів перевіряють той самий логін через
`http_routes::authorized()`; без налаштованої перевірки вони відповідають `403`.

| Без логіну | З логіном |
|---|---|
| статичні файли; `/ws`; `GET /api/ui`, `GET /api/auth`, `GET /api/wifi/ap`, `GET /api/ota`, `GET /api/time`; маршрути UPnP | решта `/api/*` |

- `/ws` віддає весь стан без логіну. UPnP-маршрути без логіну, бо контрол-поінти не надсилають
  Basic auth.
- Збірки для розробки: `CONFIG_UI_SCREENSHOT_OPEN` (типово `n`) знімає логін з `/api/screenshot`,
  `/api/ui/frames`, `/api/i18n/overflow`; `CONFIG_UI_SCROLL_BENCH` (типово `n`) додає
  `/api/ui/scrollbench` без логіну. Обидва пишуть про себе WARN на кожному старті.
- CORS: заголовка `Access-Control-Allow-Origin` немає — сторінка з іншого origin відповіді не
  прочитає; `OPTIONS` на маршрути фреймворку відповідає порожньо.
- Веб тримає облікові дані в `sessionStorage` вкладки і питає їх на `401`.

`POST /api/auth`: `{"username","enabled","current_pass","new_pass","reset"}`. `new_pass` (4–63
байти) і `reset` (повернення до `admin` / `modesp`, увімкнено) вимагають `current_pass`, інакше
`403 {"error":"wrong_password"}`; `username` — 1–31 байт без `:`. Усе перевіряється до запису.

### REST API

Для всіх маршрутів:
- `POST` мусить нести `Content-Type: application/json` (`application/octet-stream` для
  завантаження OTA); з будь-яким іншим або без нього — `415 {"error":"content_type"}`. Сторінка
  чужого сайту може змусити браузер без запиту надіслати лише типи вмісту форми, а ці два
  змушують його спершу спитати preflight-запитом, якого сервер ніколи не схвалює. Контрольних URL
  UPnP це не стосується: вони не під `/api/`. З curl: `-H 'Content-Type: application/json'`.
- Тіло, що не вміщується в буфер маршруту, відхиляється з `413` до читання; тіло читається цілим,
  скількома б TCP-сегментами воно не прийшло.
- `GET` шляху `/api/`, який ніхто не обслуговує, отримує `404 {"error":"not_found"}`
  (`application/json`); власні шляхи вебу й далі отримують `index.html`. Інший метод на такому
  шляху отримує `405` від httpd: wildcard статичних файлів бере кожен шлях `GET`, тож httpd
  вважає шлях відомим.

Маршрути фреймворку (`http_service.cpp`):

| Маршрут | Що |
|---|---|
| `GET /api/state` | увесь SharedState одним JSON-об'єктом (буфер 80 КБ у PSRAM; не вмістився — 500) |
| `POST /api/settings` | `{ключ: значення,…}`: лише відомі writable-ключі з `generated/state_meta.h`, кожен зі значенням свого типу з маніфесту (рядок, число, `true`/`false`) — решта відхиляється, а запит отримує `400`, коли з нього не взято нічого; число поза діапазоном маніфесту обрізається з WARN у лозі; command-ключ (`"command": true`) іде в чергу модуля-власника, не в сховище. Тіло ≤ 1023 Б, до 15 пар |
| `GET /api/ui` | `ui.json` |
| `GET /api/board`, `GET`/`POST /api/bindings`, `GET`/`POST /api/devices`, `GET /api/modules` | конфігурація плати й модулів ModESP |
| `POST /api/wifi` | `{"ssid","password"}`, SSID 1–32 байти, пароль ≤ 64 (інакше `400`) — лише зберегти (діє після перезапуску); з `"join":true` — `try_connect()` (`409 {"error":"busy"}` під час іншої спроби); `{"forget":true}` — забути мережу, точка доступу. Тіло ≤ 255 Б |
| `GET /api/wifi/scan` | `{"ok","networks":[{"ssid","rssi","auth"}]}` |
| `GET`/`POST /api/wifi/ap` | `{"ssid","channel","password_set"}` / `{"ssid","password","channel"}`: SSID 1–32 байти, пароль порожній або 8–63; діє після перезапуску |
| `GET`/`POST /api/time` | `{"time","date","ntp_enabled","timezone","synced","unix"}` / `{"mode":"ntp"\|"manual","time":<unix>,"timezone"}` |
| `GET`/`POST /api/auth` | `{"enabled","username"}` / див. «Автентифікація» |
| `GET`/`POST /api/loglevel` | рівні логів: `{"tag","level"}`, `level` — `none`, `error`, `warn` або `info`, не вище стелі збірки (INFO); не зберігається |
| `GET /api/log`, `GET /api/log/summary` | історія журналу ModESP; у `project.json` немає модуля-джерела, тож відповідь — 500 |
| `GET /api/backup`, `POST /api/restore` | persist-ключі (числа й bool), `{"modesp_backup":1,"keys":{…}}`, ≤ 2 КБ; відновлення пропускає ключ, що не є persist-ключем або чиє значення не має типу з маніфесту, відповідає `{"ok":true,"restored":N,"skipped":M}` і перезапускає панель |
| `POST /api/restart` | запис налаштувань, зупинка модулів у головному циклі (≤ 2 с), перезапуск |
| `POST /api/factory-reset` | стирає NVS (Wi-Fi, логін, налаштування, ключі AirPlay) і перезапускає; розділ `userdata` не чіпає |
| `GET`/`POST /api/ota` | див. «OTA» |
| `GET /api/onewire/scan`, `GET /api/drivers/scan`, `GET /api/ble/scan` | сканери шин ModESP |

Маршрути модулів (`http_routes`):

| Маршрут | Що |
|---|---|
| `GET`/`POST /api/stations` | список станцій — «Список станцій» |
| `GET /api/dlna/browser` | знімок браузера медіасерверів — «Браузер медіасерверів» |
| `GET`/`POST /api/userdata` | файли користувача (список станцій, EQ і «Власний», `devices.json`, `last_track`) одним JSON; POST кладе їх поруч як `<ім'я>.restore`, `UserStore` ставить на місце на наступному старті (веб одразу перезапускає панель). Тіло ≤ 192 КБ; файли перевіряються всі до запису |
| `GET`/`POST /api/eq` | еквалайзер — [AUDIO.uk.md](AUDIO.uk.md), «PEQ і `eq.json`» |
| `GET`/`POST /api/bt` | Bluetooth супутника, ім'я пристрою — [AUDIO.uk.md](AUDIO.uk.md), «Керування з панелі й телефона» |
| `GET /api/screenshot`, `/api/cover`, `/api/library`, `/api/i18n/overflow`, `/api/ui/frames` | маршрути панелі — [UI.uk.md](UI.uk.md), «HTTP-маршрути модуля» |
| `GET`/`POST /api/mqtt` | MQTT — «MQTT» |
| UPnP | `/rootDesc.xml`, `/<Service>.xml` і маршрути під `/<Service>/` — «DLNA» |

### OTA

`POST /api/ota`, тіло — сирий `.bin` програми.

- Розмір від 256 Б до розміру наступного OTA-слота (4 МБ); перший байт `0xE9`; `project_name` з
  `esp_app_desc_t` такий самий, як у робочого образу, інакше
  `400 {"error":"board_mismatch","running","incoming"}`.
- Три тайм-аути прийому поспіль (~90 с) — `408`: клієнт, що зник без FIN, інакше тримав би єдину
  задачу httpd.
- Одне оновлення за раз, спільно з командою MQTT `_ota`: поки пишеться одне, інше відхиляється
  (тут — `409`), а записаний образ тримає слот до перезапуску.
- Пише в наступний слот (`ota_0` / `ota_1`), робить його завантажувальним, зупиняє модулі,
  перезапускає.
- Новий образ має за 60 с отримати Wi-Fi і HTTP-сервер, `audio.state` ≠ `error` і працюючий
  `ui_lvgl`; тоді `esp_ota_mark_app_valid_cancel_rollback()`, інакше перезапуск, і завантажувач
  повертає попередній образ (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
- OTA оновлює лише програму: веб-збірка, `board.json` і `bindings.json` — у розділі `data`.
- `GET /api/ota` (без логіну): `{"partition","version","idf","date","time","board"}`.

### WebSocket `/ws`

`WsService` (`components/modesp_net/src/ws_service.cpp`, правила слотів — `ws_slots.h`, тест
`tests/host/test_ws_slots.cpp`).

- Повідомлення сервера — текстовий JSON-об'єкт `{ключ: значення,…}` у форматі `/api/state`. Після
  рукостискання клієнт отримує весь стан (потрібно ≥ 32 000 Б вільної внутрішньої RAM), далі —
  змінені ключі, щойно вони змінились, не частіше ніж одне повідомлення на 200 мс.
- Ключі, що починаються з `_`, зарезервовані для службових повідомлень і ніколи не є ключами стану.
  Кожні 5 с сервер шле кожному клієнту PING і текстовий кадр `{"_hb":n}`: сторінка PING не бачить,
  і без повідомлення не відрізнить тиху панель від з'єднання, якого вже немає. Веб закриває сокет,
  що вже приносив heartbeat, а потім мовчить 15 с, і підключається знову.
- Ключі, які модулі пишуть без позначки зміни (діагностика: CPU, купа, лімітер, лічильники
  супутника), у дельти не потрапляють; сторінка вебу з діагностикою, поки видима, опитує
  `/api/state` раз на 2 с (`webui/src/lib/live.js`).
- До 3 клієнтів. Клієнт, якого не чути 15 с (будь-який кадр, PONG теж), або той, що пропустив
  повідомлення, закривається. Кадр клієнту йде цілим за 3 с, інакше сесія закривається: задача
  httpd обслуговує весь HTTP і всіх клієнтів, і повільний телефон не повинен її тримати.
  Четвертий клієнт займає слот того, хто вже підлягає
  закриттю, або отримує CLOSE 1013 `busy`.
- Текстові кадри клієнта (до 127 Б) читаються й відкидаються; команди йдуть через
  `POST /api/settings`. Довший кадр будь-якого типу закриває сесію.
- Надсилання йде через `httpd_queue_work` у задачі httpd: `httpd_ws_send_frame_async()` з іншої
  задачі змагається з її `recv()`.
- Клієнта реєструє `ws_post_handshake_cb`: IDF 5.5.5 не викликає обробник `/ws` для рукостискання,
  тому `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y` (`sdkconfig.defaults`) обов'язковий — без
  нього `#error`.

## Інтернет-радіо

`modules/radio`. Модуль працює, лише поки `audio.source` = 5: буфери, банк фільтрів і задачі
створюються, коли радіо стає джерелом, і звільняються, коли перестає. Каталог працює за будь-якого
джерела, і рядок каталогу (з панелі чи вебу) сам ставить `audio.source` = 5; DLNA ставить його
перед `radio.play_url`. Решта входів обробляється лише при джерелі 5: `radio.cmd`, що прийшли при
іншому джерелі, відкидаються, а `radio.station` і `radio.play_url` чекають, доки джерелом стане 5.
Той самий рушій грає станції й URL ззовні; `radio.mode` розрізняє `station` і `push`
([CONCEPT.uk.md](CONCEPT.uk.md), §7).

### Потік

- Власний HTTP-клієнт над `esp_transport` (TCP або TLS з пакетом сертифікатів): ICY-метадані
  вплетені в аудіо кожні `icy-metaint` байтів, і стек Espressif їх не виймає.
- Запит: `GET`, `Icy-MetaData: 1`, `User-Agent: modesp-player/1.0`; голова відповіді ≤ 4080 Б;
  до 4 переадресацій (`Location`); `Range` — для перемотки й відновлення треку.
- Читаються `Content-Type`, `Content-Length`, `Content-Range`, `icy-metaint` (≤ 64 КБ), `icy-br`,
  `icy-name`. `StreamTitle='…'` → `radio.now_playing`.
- Плейлист (Content-Type `mpegurl` / `scpls` / `x-pls` або URL із закінченням `.m3u`, `.m3u8`, `.pls`, чий
  Content-Type не `audio/…`): береться перший http-URL. HLS не підтримується.
- Декодер — за Content-Type: `mpeg`/`mp3` → MP3, `aac`/`aacp` → AAC, `flac` → FLAC (перевіряється
  раніше за `ogg`, бо буває `audio/ogg; codecs=flac`), `ogg`/`opus` → Ogg, `wav`/`wave` → WAV,
  `mp4`/`m4a` → M4A; без придатного типу — за розширенням URL (`.aac`, `.wav`, `.flac`,
  `.m4a`/`.mp4`); інакше MP3.
- Декодери в збірці (`sdkconfig.board`): MP3, AAC з HE-AAC (SBR, PS: `aac_plus_enable`), FLAC, WAV,
  M4A. Ogg (Vorbis, Opus) вимкнений — такі потоки не грають.
- Буфер стиснутого потоку — 128 КБ у PSRAM (~8 с при 128 кбіт/с), відтворення починається з 32 КБ;
  `radio.buffer` — його заповнення, %. PCM-кільце — 0,5 с на 48 кГц ([AUDIO.uk.md](AUDIO.uk.md), «Кільце
  PCM і його власники»).
- Тайм-аути: з'єднання (DNS, TCP, рукостискання TLS) — 8 с, одне читання — 1 с; 8 тайм-аутів
  поспіль — потік утрачено.
- Частота: потік просить у кільця власну частоту і, якщо вихід став на іншій, конвертує
  ([AUDIO.uk.md](AUDIO.uk.md), «Частота виходу і сесії»). Початок і кінець потоку — згасання 480 кадрів.
- Задачі `radio` (декодер, 12 КБ) і `radio_net` (читач сокета, 6 КБ): стеки в PSRAM, ядро 0,
  пріоритет головного циклу з розподілом часу між ними.
- `esp_audio_codec` 2.5.0 пакує моно-AAC-кадр на місці у вихідному буфері й сягає до 12 КБ від
  його початку, тому буфер PCM декодера ≥ 12 КБ (`static_assert`).

**Станція** перепідключається без обмеження спроб: паузи 0,5, 1, 2, 4, 8, далі 15 с. Без мережі
(`wifi.connected` = false) чекає, не рахуючи спроб; станція, вибрана до появи мережі, стартує, щойно
мережа з'явиться.

**Повільний сервер** (`modules/radio/include/stream_watch.h`, тест `tests/host/test_stream_watch.cpp`):
два 5-секундні звіти поспіль, коли мережа дає < 95 % потреби потоку, а буфер < 10 % — перепідключення
до іншої адреси того самого хоста (з'єднання за IP, ім'я — у `Host` і SNI), не частіше ніж раз на
35 с. Лише станції.

**TLS.** Станція може грати по HTTPS; каталог і обкладинки треків ходять лише по HTTP, логотипи по
HTTPS — лише поки сама станція грає по HTTP: її адреса і, після переадресації чи плейлиста, її
з'єднання: TLS-сесія одна, і поки грає станція, її тримає станція. Логотип по HTTPS завантажується
лише один за раз (`radio_http::get_url`).

### Список станцій

`components/stations` (тест `tests/host/test_stations.cpp`): до 64 станцій (назва ≤ 63 Б, URL
≤ 223 Б, `uuid` Radio Browser), 6 пресетів. Файл — `/userdata/stations.json` через `UserStore`, на
панелі без розділу `userdata` — `/data/stations.json`; без файлу або з порожнім — вбудований набір.

```json
{"stations":[{"name":"…","url":"…","uuid":"…"}],"presets":[0,3,-1,-1,-1,-1]}
```

Нечитабельні записи відкидаються, решта лишається; файл ≤ 24 КБ. Назва — один рядок: керівний
символ у ній (табуляція, розрив рядка) стає пробілом, а пробіли на її краях відкидаються, хоч би
звідки вона прийшла — з вебу, з каталогу, з файлу.

- `GET /api/stations` → `{"rev":N,"list":{"stations":[…],"presets":[…]}}`.
- `POST /api/stations` ← `{"rev":N,"stations":[…],"presets":[…]}`, `rev` — першим полем. `202` —
  прийнято, застосовує головний цикл; `409 {"error":"changed","rev":M}` — список змінився з того
  покоління; `400 {"error":"rev_first"|"no_stations"}` (порожній список не приймається: на
  наступному старті він читався б як відсутній); `413`; `503 {"error":"busy"|"no_memory"}`.
- Станція, що грає, грає далі й після видалення зі списку.
- `radio.list_rev` — покоління списку; змінюється з кожною правкою з панелі, каталогу чи вебу.
- Пресет, записаний з екрана, зберігає у файл `radio.save` = 1.

### Каталог Radio Browser

**Панель** (`modules/radio/src/catalog.cpp`, задача `radio_cat`; запити, розбір і фільтри —
`components/radio_catalog`, тест `tests/host/test_radio_catalog.cpp`).

- Лише HTTP, порт 80, User-Agent `P4Player/<версія програми>`.
- Сервери — з `/json/servers`, раз на добу; запасне ім'я `all.api.radio-browser.info`.
- Запит `radio.cat_query`: `top:UA`, `country:DE`, `tag:jazz`, `tag:jazz@UA`, `name:kiss`,
  `name:kiss@UA`; рейтинг — у межах країни чи жанру, бо глобальні голоси Radio Browser накручені.
  Порядок `radio.cat_sort`: 0 кліки за добу, 1 голоси, 2 бітрейт, 3 назва. Поріг
  `radio.cat_quality`: 0 усі, 1 від 128, 2 від 192, 3 320 кбіт/с, 4 лише FLAC.
- Сторінка — 12 рядків із 24, запитаних у сервера: пристрій відкидає кодеки, крім MP3, AAC, AAC+ і
  FLAC, HLS, URL довші за 223 Б і повтори одного потоку. До 64 сторінок уперед; кеш 6 сторінок на
  10 хв; не частіше двох запитів на секунду; тіло ≤ 64 КБ.
- Країни — `/json/countrycodes`, раз на добу; жанри — `/data/www/radio_genres.json` (назви uk, en,
  de, pl і тег запиту).
- `radio.cat_play` = N — грати рядок як станцію (перепідключається, має назву), `radio.cat_fav` = N
  — додати до списку, `radio.fav_add` — додати до списку станцію з каталогу, що грає. Станція з
  каталогу поза списком: `radio.station` = −1, `radio.uuid` — її uuid.
- Стан — `radio.cat_state` (`idle|loading|ready|error`), `radio.cat_page`, `radio.cat_count`,
  `radio.cat_more`, `radio.cat_list`, `radio.cat_error`.

**Веб** (Станції → каталог, `webui/src/lib/radiobrowser.js`): браузер сам ходить у Radio Browser по
HTTPS (API дає CORS `*`) з тими самими фільтрами. Панелі передаються лише `radio.web_name`,
`radio.web_url`, `radio.web_uuid`, потім `radio.web_act`: 0 грати, 1 до списку, 2–7 у пресет 1–6.
URL має бути `http(s)://` і ≤ 223 Б. Плеєр тими самими ключами зберігає на кнопку те, що грає, і
питає `/json/stations/byuuid?uuids=…` про логотипи своїх станцій ([UI.uk.md](UI.uk.md), «Сторінки
і що вони читають/пишуть»).

**Кліки.** Коли станція з `uuid` (зі списку чи з каталогу) почала грати, модуль раз на налаштування
робить `GET /json/url/<uuid>` — так Radio Browser рахує популярне. Вимикає `radio.report_clicks`.

### Обкладинки й логотипи

`modules/radio/src/art.cpp`, задача `radio_art` (16 КБ, PSRAM); розбір назви, запит і вибір
результату — `components/radio_catalog/include/radio_art.h`, тест `tests/host/test_radio_art.cpp`.

- **Обкладинка треку.** ICY-назва `Виконавець - Назва` (також із `–` чи `—`) → пошук Deezer
  `http://api.deezer.com/search?q=…&limit=10`; береться перший результат з тією самою назвою і тим
  самим виконавцем (прямо, коли одне починається з іншого, або через транслітерацію кирилиці);
  `album.cover_big` (500 px) по HTTP, ≤ 256 КБ. `radio.covers` = false — назва в Deezer не
  надсилається.
- **Логотип станції.** `favicon` рядка каталогу або з `/json/stations/byuuid?uuids=<uuid>`; спершу по
  HTTP (і для `https://`-адреси), по HTTPS — лише коли сама станція грає по HTTP; ≤ 128 КБ, JPEG або
  PNG.
- Плитка `CoverSource::Radio`: обкладинка треку, без неї — логотип, без нього — нічого, і панель
  малює заміну станції ([UI.uk.md](UI.uk.md), «Сторінки панелі»). Трек, надісланий через DLNA,
  показує обкладинку контрол-поінта (`CoverSource::Network`), без неї — плитку Radio.

**Логотипи пресетів** (`modules/radio/src/logos.cpp`; правила — `components/radio_catalog/include/
station_logo.h`, тест `tests/host/test_station_logo.cpp`; вписування в диск —
`components/cover/include/cover_thumb.h`, тест `tests/host/test_cover_thumb.cpp`). Панель зберігає
логотип кожної станції на пресеті, що має `uuid` Radio Browser, тож її кнопка показує його, поки
TLS-сесію тримає HTTPS-станція, і після перезапуску ([UI.uk.md](UI.uk.md), «Сторінки панелі»).

- **Зберігання**: `logos/<uuid>.565` у теці `UserStore` (`/userdata`; на панелі без цього розділу —
  `/data`) — 40×40, розмір диска пресету, малюється без масштабування: 8-байтний заголовок (`SLG1`,
  сторона двічі) і RGB565, 3208 Б на файл. Читається на старті й коли змінюються пресети.
  Логотип, що прийшов або змінився, одразу з'являється на пресеті, а файли пишуться (через
  `safe_file`) і видаляються лише тоді, коли нічого не грає: після 4 с тиші на виході
  (`PcmRing::output_quiet` — вихідний гейт закритий і черга TX дограна, хай яке джерело), по
  одному файлу за прохід раз на 500 мс, бо запис LittleFS зупиняє кеш обох ядер і під музику в
  колонках клацає. Тоді ж іде файл станції, що більше не на пресеті, і кандидат, якого лишило
  перерване збереження. Логотип, не записаний до зникнення живлення, після перезапуску
  завантажується знову. Файл називає лише правильний uuid.
- **Завантаження**: задача `radio_logo` (12 КБ, PSRAM, пріоритет простою), одна за раз, створюється
  на одну партію й зникає після неї; на проході головної задачі раз на 500 мс, коли мережа є вже
  20 с, не раніше ніж через 30 с після попередньої партії, і лише поки TLS-сесія вільна — радіо не
  джерело, зупинене або грає станцію по звичайному HTTP і вже пройшло буферизацію (не надісланий
  трек). Один `/json/stations/byuuid?uuids=a,b,…` для всіх станцій без логотипа, потім кожен
  favicon так само, як логотип станції вище: спершу HTTP, ≤ 128 КБ, JPEG або PNG за першими
  байтами. Перед кожним favicon задача знову дивиться, чи вільна TLS-сесія; коли ні, решта чекають
  наступної партії.
- **Декодування**: `cover_thumb()` (`components/cover`) — апаратний JPEG для baseline JPEG,
  програмний декодер на пріоритеті простою задачі для PNG і progressive JPEG, з обмеженнями
  обкладинки. Картинка вписується вся, без обрізання, і усереднюється вниз; прозорі пікселі й поля
  лягають на тло: колір краю картинки, коли прозорості немає, інакше світлий нейтральний під
  темним малюнком і темний — під світлим.
- Логотип станції, що грає, завантажений для плитки, стає логотипом її пресету без другого
  завантаження.
- **Невдачі**: станцію, чий логотип не отримати — його немає в записі, адреса не відповідає, файл не
  JPEG і не PNG (ICO, SVG, WebP) або не декодується, — питають знову через 6 год; коли Radio
  Browser не відповів на запит — через 10 хв. Очікування не переживають перезапуску. Станція без
  `uuid` (введена вручну) не має звідки взяти логотип і лишається з заміною.

### URL ззовні: позиція, перемотка, наступний трек

`radio.play_url` (з вебу або від DLNA) грає URL один раз: `radio.mode` = `push`, `radio.name`
порожня, `radio.url` — сам URL, назву тримає той, хто надіслав (`dlna.title`). URL — до 256 Б
проти 223 у станції: медіасервер кладе в URL id елемента й токен сесії, і обрізаний URL падає як
404.

- Нескінченного перепідключення немає: невдале перше з'єднання зупиняє трек; обрив посеред треку —
  продовження з тієї самої позиції через `Range`, до 20 спроб на трек (3 з'єднання на епізод з
  паузою 0,5 с).
- **Тривалість** `radio.dur_s` = `Content-Length` × 8 / бітрейт; 0 — невідома (станція). Декодер
  MP3 рапортує бітрейт у кбіт/с, тож значення < 10 000 читається як кбіт/с
  (`components/audio_pipe`, тест `tests/host/test_stream_pos.cpp`).
- **Позиція** `radio.pos_s` — кадри на виході декодера, поділені на частоту джерела: прийняті байти
  гнали б її вперед на весь буфер.
- **Перемотка** `radio.seek_s` = секунда (−1 — нічого): та сама частка байтів, що й секунд, новий
  `GET` з `Range: bytes=N-`, довжина — з `Content-Range` відповіді 206.
  Вміст кільця згасає (`request_flush`); декодер не перевідкривається — синхронізується на
  наступному заголовку кадру. На VBR перемотка наближена.
- `radio.seekable` = надісланий трек, не WAV, відома тривалість. У WAV частка байтів може влучити в
  заголовки RIFF/JUNK або між кадрами PCM, тому WAV не перемотується і після обриву не
  продовжується.
- **Наступний трек** — `radio.next_url` (DLNA пише туди слот наступного треку). За ≤ 12 с до кінця
  надісланого треку задача `radio_next` (12 КБ, PSRAM) відкриває його (`esp_http_client`, потрібні
  відповідь 200 і `Content-Length`), за сигнатурою файлу бере декодер MP3, FLAC чи WAV (інший
  формат не готується) і декодує перший PCM-блок (префікс ≤ 128 КБ). На кінці треку з тією самою
  частотою джерела підготовлений потік стає поточним без спорожнення кільця (лог
  `prepared handoff`); інша частота, невідома довжина чи неготовий префікс — звичайний перехід
  після спорожнення. Затримку й доповнення енкодера MP3 не обрізано: стик MP3 не sample-accurate.
- **Кінець треку:** `radio.completed` + 1, `radio.completed_uri` — трек, що скінчився,
  `radio.promoted_uri` — підхоплений наступний (порожньо, коли трек дограв до кінця без нього, лог
  `track drained to end`).

### Ключі `radio.*`

| Ключ | Значення |
|---|---|
| `radio.state` | `idle` / `connecting` / `buffering` / `playing` / `reconnecting`; `idle`, поки грає інше джерело |
| `radio.cmd` | команда: 1 грати, 2 стоп, 3 наступна, 4 попередня |
| `radio.station` | індекс у списку (0–63), −1 — станція з каталогу; persist |
| `radio.name`, `radio.url`, `radio.uuid`, `radio.now_playing`, `radio.format`, `radio.buffer` | що грає |
| `radio.count`, `radio.list_rev`, `radio.save` | список станцій |
| `radio.mode` | `station` / `push` |
| `radio.play_url`, `radio.next_url` | URL ззовні; наступний трек |
| `radio.pos_s`, `radio.dur_s`, `radio.seek_s`, `radio.seekable` | позиція й перемотка |
| `radio.completed`, `radio.completed_uri`, `radio.promoted_uri` | кінець треку |
| `radio.cat_*`, `radio.fav_add`, `radio.web_*` | каталог |
| `radio.covers`, `radio.report_clicks` | persist, типово true |

## DLNA

Рендерер (DMR) і контрол-поінт (браузер медіасерверів); медіасервера немає. Через рендерер панель
видно в Music Assistant (DLNA-провайдер), BubbleUPnP, mConnect, foobar2000 і «Передати на пристрій»
Windows.

| Шар | Звідки |
|---|---|
| SSDP, опис пристрою, SOAP, GENA | `espressif/esp_media_protocols` 1.0.1 — прибитий передзібраний архів; ISA `xespv2p1`, без інструкцій `xespv2p2`, яких P4 v1.3 не має |
| AVTransport, RenderingControl, ConnectionManager | `components/esp_dlna`, вендорено з ESP-ADF ([README](../components/esp_dlna/README.uk.md)) |
| SOAP AVTransport | `avt_soap`, `modules/dlna/src/dlna_soap.inc` |
| Зв'язок із SharedState | `modules/dlna/src/dlna_module.cpp` |
| Модель транспорту | `components/dlna_client/include/dlna_transport.h`, спільна з браузером |

Модуль не відтворює звук: трек віддається рушію радіо через `radio.play_url` з `audio.source` = 5,
стан читається з `radio.*`. Модулі не включають один одного — контракт лише ключі.

### Запуск рендерера

- Стартує один раз, коли `dlna.enabled` і `wifi.connected`, на задачі `dlna` (6 КБ PSRAM,
  пріоритет 3, ядро 0): `esp_dlna_*_register_service()` будують таблиці сервісів масивами на
  стеку.
- Перед стартом у NVS ставиться `dlna/starting`, після — знімається. Мітка, знайдена на старті,
  означає, що попередній старт не повернувся: рендерер вимкнений на цей запуск, мітка стерта,
  наступний запуск пробує знову.
- Реєстрація маршрутів: wildcard `/*` знімається; на час виклику вмикається
  `components/esp_dlna/httpd_uri_compat.c`. Бібліотека зібрана без `CONFIG_HTTPD_WS_SUPPORT` і
  передає 16-байтний `httpd_uri_t` проти 24 байтів у цій збірці — httpd читав би
  `supported_subprotocol` за межею об'єкта. `-Wl,--wrap=httpd_register_uri_handler` діє лише поки
  реєструється бібліотека: перебудовує структуру з чотирьох заповнених полів і для
  `POST /AVTransport/*` ставить обробник `avt_soap`. Потім wildcard повертається.
- Маршрути: `/rootDesc.xml`; для кожного сервісу (`AVTransport`, `RenderingControl`,
  `ConnectionManager`) — `/<Service>.xml` і маршрути керування й подій під `/<Service>/`, разом сім
  обробників httpd (реєструє `esp_media_protocols`); SOAP AVTransport приходить на
  `/AVTransport/Control`. Модуль додає `/api/dlna/browser` (у вікні `http_routes` на старті). Веб ці
  шляхи не займає.
- SSDP — поки `dlna.enabled` і є мережа: 239.255.255.250:1900, `LOCATION:
  http://<ip>/rootDesc.xml`, оголошення `upnp:rootdevice`, `MediaRenderer:1`,
  `ConnectionManager:1`, `RenderingControl:1`, `AVTransport:1`. Стек задачі SSDP — у внутрішній
  RAM, всупереч типовому значенню компонента (`stack_in_ext = false`): задача зі стеком у PSRAM не
  може виконуватися, поки кеш flash вимкнений, а NVS і LittleFS пишуть інші задачі. Ядро 0.
  Вимкнення шле `ssdp:byebye`; маршрути рендерера лишаються зареєстрованими.
- Ідентичність: UUID `50344453-0000-1000-8000-<MAC>` (сталий між перезапусками, контрол-поінти
  пам'ятають пристрій за ним), UDN `P4_DMR_<device_id>`, friendlyName — ім'я пристрою, manufacturer
  `ModESP`, model `P4 Player`. Іконки немає.

### Потоки

1. Колбек рендерера (`renderer_request`) і `avt_soap` виконуються в задачі httpd усередині
   SOAP-запиту: лише читання й запис SharedState під м'ютексом моделі (очікування 50 мс у
   `avt_soap`, 1 с у колбеку).
2. Дії, що запускають чи зупиняють звук, ставляться в чергу моделі (8 команд); виконує головний
   цикл — по одній за оберт і лише коли рушій забрав попередній `radio.play_url`.
3. GENA NOTIFY — HTTP-запити до підписників; їх шле лише задача `dlna` (черга 8): підписник, що
   зник без відписки, блокує до тайм-ауту з'єднання, а головний цикл годує watchdog.

### AVTransport

| Дія | Поведінка |
|---|---|
| `SetAVTransportURI` | URI ≤ 256 Б, DIDL-Lite ≤ 2048 Б (інакше `402`); не `http(s)://` — `716`; порожній URI — Stop. Очищує наступний і попередній трек. Коли транспорт грає, новий трек грає одразу; інакше лише завантажується (`STOPPED` або `PAUSED_PLAYBACK`) |
| `SetNextAVTransportURI` | слот наступного треку; ті самі межі й коди, що в `SetAVTransportURI` |
| `Play` | `Speed` лише `1` (`717`); без треку — `701`. На паузі з тим самим URI — продовження; інакше старт: `audio.source` = 5, `radio.play_url`, стан `TRANSITIONING` |
| `Pause` | пауза споживача кільця (`pcm_ring().set_paused`): сокет лишається, TCP притримує сервер, продовження миттєве. Не свій трек або `STOPPED` — `701` |
| `Stop` | свій трек — `radio.cmd` = 2; стан `STOPPED` |
| `Next` | наступний трек; немає — `711` |
| `Previous` | попередній трек; немає — перемотка на 0 (діє лише при `radio.seekable`) |
| `Seek` | `REL_TIME` / `ABS_TIME` (інакше `710`); ціль < тривалості (`711`); не свій трек — `701`. Виконується лише при `radio.seekable`; інакше відповідь OK, а головний цикл команду відкидає |
| `GetTransportInfo`, `GetPositionInfo`, `GetMediaInfo`, `GetDeviceCapabilities`, `GetTransportSettings`, `GetCurrentTransportActions` | читання; час у форматі `H:MM:SS` |
| `SetPlayMode` | лише `NORMAL` (`712`) |

- Спільне: `InstanceID` ≠ 0 — `718`; тіло ≥ 16 КБ — `402`; черга повна або м'ютекс зайнятий —
  `715`; відповідь понад 32 КБ — `501`. Помилка — справжній UPnP SOAP fault, HTTP 500.
- SOAP-відповідь `GetCurrentTransportActions` завжди містить `Seek`; змінна події
  `CurrentTransportActions` — лише при `radio.seekable`.
- Позиція й тривалість — `radio.pos_s` / `radio.dur_s`, які модуль дзеркалить раз на 250 мс:
  контрол-поінт питає `GetPositionInfo` щосекунди на кожного підписника.
- **Володіння.** Дія контрол-поінта діє лише на трек, який він надіслав (`audio.source` = 5,
  `radio.mode` = `push`, `radio.url` = його URI): пауза чи стоп з телефону не зупиняє станцію чи трек
  з картки. `Play` з новим URI — це і є запит забрати вихід.
- **Старт.** Після пушу модуль до 15 с чекає першої ознаки життя рушія (`connecting`, `buffering`,
  `playing`, `reconnecting`): перші ~500 мс `radio.state` ще `idle`, бо команда в черзі. Після неї
  `idle` означає кінець треку.
- **Кінець треку.** `radio.completed` з `radio.completed_uri` = свій трек → наступний трек моделі:
  `Adopt`, коли рушій його вже підхопив (`radio.promoted_uri`), або `Play`; стан `TRANSITIONING`.
  Без наступного — `STOPPED`. Лог `Dlna: track completion: <uri>, next adopted|scheduled|empty`.

### RenderingControl, ConnectionManager

- `GetVolume` / `SetVolume` ↔ `audio.volume` (0–100), mute ↔ `audio.mute`. Зміна гучності на панелі
  шле подію RenderingControl — повзунок у застосунку йде слідом.
- Sink-список `GetProtocolInfo`: MP3 (`audio/mpeg`, `audio/mp3`, `audio/x-mpeg`, `audio/mpeg3`), FLAC
  (`audio/flac`, `audio/x-flac`), WAV (`audio/wav`, `audio/x-wav`, `audio/wave`), плейлисти
  (`audio/mpegurl`, `audio/x-mpegurl`, `audio/x-scpls`). За ним Music Assistant вирішує, що
  перекодувати на сервері. Рушій декодує також AAC і M4A, але ні sink-список, ні браузер їх не
  пропонують.

### Браузер медіасерверів

`components/dlna_client`, задача `dlna_client` (12 КБ PSRAM, пріоритет 1). Сторінки — «DLNA» на
панелі (`modules/ui_lvgl/src/screen_dlna.cpp`) і «Медіасервери DLNA» у вебі ([UI.uk.md](UI.uk.md)).

- Пошук: `M-SEARCH`, `ST: urn:schemas-upnp-org:device:MediaServer:1`, `MX: 2`, відповіді 2,5 с; до 8
  серверів, без повторів `LOCATION`. З опису — `friendlyName`, `URLBase`, `controlURL`
  ContentDirectory.
- `Browse` `BrowseDirectChildren` по 12 рядків, до 16 рівнів углиб, XML ≤ 64 КБ. HTTP або HTTPS: 4 с
  на операцію, 12 с на запит, до 3 переадресацій.
- Трек відтворюваний, коли в нього є `<res>` з `protocolInfo` `http-get:*:<mime>` і MIME зі
  sink-списку (MP3, FLAC, WAV). DIDL-Lite для рендерера будується наново з відомими просторами імен.
- Команда — `dlna.browse_cmd` = `ревізія << 12 | рядок << 4 | команда`; команди: 1 пошук, 2 відкрити,
  3 назад, 4 наступна сторінка, 5 попередня, 6 у чергу, 7 скасувати. Команда з ревізією, відмінною
  від показаної, ігнорується: дотик до рядка, що встиг змінитися, нічого не робить.
- «Відкрити» на треку — трек стає поточним і грає; «у чергу» — слот наступного треку.
- Обкладинка: `albumArtURI` відносно адреси сервера, ≤ 512 КБ → `CoverSource::Network`.
- `status` знімка і `title` списку серверів — ключі каталогу перекладів `@dlna.*` (аргументи після
  `\x1f`): панель і веб показують їх своєю мовою; у папці `title` — її назва.
- `GET /api/dlna/browser` → `{"revision","busy","servers","can_back","count","next","previous",
  "title","status","rows":[{"title","artist","folder","playable"}]}`.

### Ключі `dlna.*`

| Ключ | Значення |
|---|---|
| `dlna.enabled` | SSDP-оголошення; persist, типово true |
| `dlna.state` | `NO_MEDIA_PRESENT` / `STOPPED` / `TRANSITIONING` / `PLAYING` / `PAUSED_PLAYBACK` — те саме, що отримує контрол-поінт |
| `dlna.name` | friendlyName |
| `dlna.uri`, `dlna.title`, `dlna.artist`, `dlna.album`, `dlna.art_uri` | трек і DIDL-Lite |
| `dlna.cmd` | транспорт з панелі чи вебу: 1 грати/продовжити, 2 стоп, 3 пауза, 4 наступний, 5 попередній |
| `dlna.browse_cmd` | команда браузера |

### Перевірка

```
curl -u <user>:<pass> -X POST -H 'Content-Type: application/json' http://<panel>/api/loglevel -d '{"tag":"Dlna","level":"info"}'
curl http://<panel>/rootDesc.xml     # XML опису; HTML вебу — маршрути стали після wildcard
```

- `dlna.name` і `dlna.state` — у `GET /api/state`.
- Контрол-поінт у тій самій мережі показує рендерер за кілька секунд; повзунок гучності ↔
  `audio.volume` в обидва боки.
- Пуш треку: `Radio: playing pushed URL …`, `dlna.state` `TRANSITIONING` → `PLAYING`.
- Кінець треку: `Radio: track drained to end` або `prepared handoff: …`, далі
  `Dlna: track completion: …`; `dlna.state` = `STOPPED` без наступного треку, `TRANSITIONING` — з ним.
- `tools/dlna_fixture_server.py --host <IP ПК>` (MediaServer з тестовими WAV і PNG) і
  `tools/dlna_acceptance.py --panel <URL> --fixture <URL>` — обмежені перевірки рендерера й
  браузера на панелі; хост-тести `tests/host/test_dlna_*.cpp`.

## AirPlay

`modules/airplay` над `components/airplay2` — ядром
[rbouteiller/airplay-esp32](https://github.com/rbouteiller/airplay-esp32) з некомерційною ліцензією;
походження, ліцензія й локальні зміни — [UPSTREAM.uk.md](../components/airplay2/UPSTREAM.uk.md). Збірка
без модуля не пропонує `audio.source` = 6.

Приймач: realtime ALAC по UDP і буферизований AAC по TCP, HAP-спарювання (ключі в NVS-просторі
`airplay`), годинники PTP і NTP, DACP-клієнт. Звук іде в PCM-кільце як `OWNER_AIRPLAY`
([AUDIO.uk.md](AUDIO.uk.md)).

### Профілі

| `airplay.mode` | Профіль | mDNS | Порт RTSP | `Server` | model |
|---|---|---|---|---|---|
| 0 (типово) | AirPlay 2 | `_airplay._tcp` і `_raop._tcp` | 7000 | `AirTunes/377.40.00` | `AudioAccessory5,1` |
| 1 | AirPlay 1 · сумісність із Windows | лише `_raop._tcp` | 5000 | `AirTunes/105.1` | `Shairport` |

- Спільна політика — `components/airplay2/port/mode_policy.h`: оголошення, порт, версія, features,
  відповідь `/info` (бінарний plist на RTSP, XML на HTTP).
- Профіль 1 — лише класичний RAOP: без функцій AirPlay 2 і без мультируму AirPlay 2.
- Оголошується один профіль за раз.

### Увімкнення й перемикання

- Приймач стартує один раз, коли `airplay.enabled` і Wi-Fi підключений у режимі STA, на задачі
  `ap_start` з 8 КБ внутрішнього стека: HAP на першому старті пише ключі у flash. Мітка
  `airplay_mod/starting` у NVS діє як у DLNA: старт, що не повернувся, вимикає приймач на
  наступний запуск.
- Ім'я береться під час старту модуля, ідентичність — базова MAC; нове ім'я — після перезапуску.
- `airplay.mode` зберігається й застосовується без перезапуску P4: записи mDNS знімаються, задача
  `ap_mode` перезапускає лише слухача RTSP з новим профілем, поточна сесія AirPlay рветься, решта
  служб працює; потім оголошення повертається. Без Wi-Fi перемикання чекає зі статусом
  `@airplay.st_wifi`.
- `airplay.enabled` = false: записи mDNS знімаються, усі клієнти відключаються, слухач RTSP
  зупиняється. true — слухач стартує з обраним профілем.
- Веб: сторінка «Налаштування», група «Служби» — рядок AirPlay з перемикачем, під ним, поки AirPlay
  увімкнений, рядок «Режим AirPlay» зі списком; у підписі — помилка або стан, поки триває
  перемикання. Активний профіль показує сторінка «Плеєр», поки грає AirPlay. Панель:
  «Налаштування → Служби», дотик до рядка AirPlay (не до перемикача) відкриває список із двох
  режимів.
- Відправник читає features, коли приймач обирають. Після прошивки, що змінює features, треба
  обрати на телефоні інший вихід і знову панель.

### Джерело, гучність, пульт

- Відправник, що почав грати, перемикає `audio.source` на 6 — лише на початку відтворення і лише
  при `airplay.enabled`. Вибір іншого джерела на панелі: відправника AirPlay 1 просять стати на
  паузу через DACP; відправник AirPlay 2 (MRP не реалізовано) шле далі, приймач тримає сесію і
  годинник і відкидає звук, тож повернення до джерела 6 продовжує синхронно.
- Гучність: повзунок телефону −30…0 дБ ↔ `audio.volume` 0–100 за положенням (−144, mute → 0).
  Гучність кімнати оголошується відправникам як `initialVolume` у `/info`. Рівень телефону рухає
  кімнату лише поки AirPlay — джерело і не в перші 3 с після підключення чи початку відтворення
  (`components/audio_dsp/include/volume_follow.h`).
- Пульт: відправник, що дав `DACP-ID` і `Active-Remote` (AirPlay 1: TuneBlade, Music Assistant,
  iTunes, iPhone чи Mac у режимі 1), — `airplay.remote` = true; `airplay.cmd` (1 грати, 2 пауза,
  3 стоп, 4 наступний, 5 попередній, 6 перемкнути) іде йому через DACP. Відправник AirPlay 2 пульта
  не дає.

### Обкладинки

Запитуються, коли працює задача обкладинок `ap_cover` (6 КБ PSRAM): RAOP-запис `md=0,1,2` для
відправника AirPlay 1 і біт features 15 для AirPlay 2 — відправник AirPlay 2 питає обкладинку за
features, а не за `md`. Біти 16 (прогрес) і 17 (текст) профіль AirPlay 2 ставить завжди.
Обкладинка ≤ 256 КБ (межа буфера RTSP), JPEG або PNG → `CoverSource::AirPlay`; `image/none`
очищує; нова назва без обкладинки в межах 5 с прибирає стару.

### Ключі `airplay.*`

| Ключ | Значення |
|---|---|
| `airplay.enabled` | persist, типово true |
| `airplay.mode` | обраний профіль; persist |
| `airplay.active_mode`, `airplay.active_mode_label` | активний профіль: `AirPlay 2` / `AirPlay 1 · Windows` / `@airplay.st_stopped` |
| `airplay.switching` | перемикання триває |
| `airplay.mode_status` | `@airplay.st_starting`, `st_switching`, `st_applied`, `st_not_applied`, `st_stopped`, `st_wifi` — ключі каталогу перекладів |
| `airplay.mode_error` | назва `esp_err_t`; порожньо — без помилки |
| `airplay.state` | `off` / `idle` / `connected` / `playing` / `paused` / `switching` |
| `airplay.name` | ім'я в списку AirPlay |
| `airplay.title`, `airplay.artist`, `airplay.album`, `airplay.pos_s`, `airplay.dur_s` | метадані відправника |
| `airplay.remote`, `airplay.cmd` | пульт DACP |

Kconfig `components/airplay2/Kconfig`: `AIRPLAY_TIMING_THRESHOLD_MS` 25 (буферизований потік),
`AIRPLAY_RT_TIMING_THRESHOLD_MS` 50 (realtime) — наскільки кадр може випереджати чи запізнюватися,
перш ніж його притримають чи відкинуть.

### Перевірки

```
python tools/tests/test_airplay_mode.py
python tools/tests/test_airplay_mode_module.py
python tools/tests/test_airplay_reconfigure.py
python tools/airplay_send.py <ip панелі>     # відправник AirPlay 1 (RAOP) з тестовим сигналом
```

Тести проганяють обидва профілі з імітацією транспорту й mDNS (потрібні Python і нативний
компілятор C). `airplay_send.py` не досягає половини AirPlay 2 (HAP, PTP, буферизований AAC).

## MQTT

`components/modesp_mqtt`. Компілюється разом із мережею (`CONFIG_MODESP_CLOUD_MQTT` — типовий
вибір при `MODESP_NET_ENABLE`), вимкнений під час роботи: NVS `mqtt/enabled` типово false.

- Налаштування: `GET /api/mqtt` → `{"enabled","connected","broker","port","user","prefix",
  "tenant","device_id","status"}`, `POST /api/mqtt` ← `{"enabled","broker","port","user",
  "password","prefix","tenant"}`; або веб, «Сервіс → Інші параметри». NVS-простір `mqtt`:
  `broker`, `port` (типово 1883), `user`, `pass`, `prefix`, `tenant`, `enabled`, `broker_ca` (PEM).
- TLS: `mqtts://` у `broker` або порт 8883 при адресі без схеми → TLS із сертифікатом `broker_ca`
  чи вбудованим пакетом; інакше відкритий текст, `mqtt.status` = `insecure`.
- Префікс: `player/v1/<tenant>/<device_id>`, без tenant — `player/v1/pending/<device_id>`
  (`mqtt_topic_root` у `project.json`; `device_id` — три останні байти MAC, hex).
- `<prefix>/status` — `online` / `offline` (LWT, QoS 1, retain).
- `<prefix>/state/<ключ>` — раз на 1 с змінені ключі з розділів `mqtt.publish` маніфестів:
  `audio.source`, `audio.volume`, `audio.mute`, `audio.state`, `audio.underruns`, `ui.backlight`,
  `ui.encoder_pos`, а також `_ota.status`, `_ota.progress`, `_ota.error`.
- `<prefix>/heartbeat` — раз на 30 с `{"proto":1,"fw","up","heap","rssi":0}`.
- Команди — підписка `<prefix>/cmd/+`: будь-який writable-ключ типу int, float або bool з
  `state_meta` (діапазон обрізається, command-ключ — у чергу модуля). Рядкові ключі
  (`radio.play_url`) через MQTT не застосовуються. Retained-команди ігноруються. Службові:
  `_ota` (`{"url","version","checksum":"sha256:…"}` — завантаження прошивки по HTTP чи HTTPS,
  перевірка й запис в OTA-слот), `_request_full_state`, `_set_mqtt_creds`, `_set_tenant`.
- Перепідключення: 5 с, подвоєння до 5 хв. Мережеві кроки клієнта — з тайм-аутом 3 с, а старий
  клієнт зупиняє задача `mqtt_stop`: `esp_mqtt_client_stop()` може чекати цілу спробу з'єднання, а
  головний цикл годує 5-секундний watchdog.
- Home Assistant discovery: маніфести не оголошують сутностей, нічого не публікується.
- `mqtt.status`: `disabled`, `no_broker`, `waiting_wifi`, `connecting`, `connected`, `insecure`,
  `disconnected`, `error`; також `mqtt.connected`, `mqtt.broker`.

## Діагностика

- Типовий рівень логів — WARN; із мережевих тегів на INFO лише `Radio`
  (`components/modesp_services/src/log_policy.cpp`). Решту — `WiFi`, `HTTP`, `WS`, `Dlna`,
  `NextStream`, `AirPlay`, `airplay2`, `mdns_airplay`, `MQTT`, `UserData` — відкриває
  `POST /api/loglevel` до перезапуску; рядки, надруковані під час старту, так не побачити.
- `Radio` раз на 5 с, поки грає: `stream: buffer P%, net N B/s (need M), audio X.XXx` і рядок
  навантаження помпи, декодера й ресемплера.
- Задачі `radio`, `radio_net` (30 с) і `dlna` (10 с) під наглядом `modesp::liveness`.
- Соук-тест SDIO-лінку під потоком — [SOAK_TEST.uk.md](SOAK_TEST.uk.md).
