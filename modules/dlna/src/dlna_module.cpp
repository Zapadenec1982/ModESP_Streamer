/**
 * @file dlna_module.cpp
 * @brief DLNA/UPnP MediaRenderer — the control plane of a player somebody else drives.
 *
 * What this is for: the panel should appear in Music Assistant, in BubbleUPnP, in foobar
 * and in Windows' "Cast to device" without a remote of its own. That is what a
 * MediaRenderer is — a device that advertises itself, accepts a URL, and answers questions
 * about what it is doing.
 *
 * It is a CONTROL plane and nothing else. No sample passes through this file: a pushed URL
 * is handed to the network stream engine in modules/radio through `radio.play_url`, exactly
 * as the screen hands it a station, and what comes back is read from `radio.state`. Modules
 * do not include each other (docs/CONCEPT.md, "SharedState contract: sources"); SharedState
 * keys are the contract between them.
 *
 * Shape:
 *   • the protocol      — espressif/esp_media_protocols (SSDP, SOAP, GENA), prebuilt
 *   • the three services — components/esp_dlna, vendored from ADF (see its README)
 *   • this file          — one callback mapping UPnP requests onto SharedState, and back
 *
 * Three things about the threading, because each of them is a way to reboot the board:
 *
 * 1. The renderer callback runs in the HTTPD task, inside a SOAP request. SharedState is
 *    mutex-guarded, so reading and writing keys there is safe; anything slower than that is
 *    not, because a control point is holding the connection open waiting for the answer.
 * 2. Actions that start or stop audio are therefore only RECORDED there and carried out by
 *    the main loop, which is where every other module's commands are carried out too.
 * 3. GENA notifications are HTTP requests we make to the subscriber. They must never be
 *    sent from the main loop — that loop feeds the task watchdog, and a subscriber that has
 *    gone away without unsubscribing blocks until the connect times out. This is the same
 *    fault as the RSSI RPC that resets the board under stream load (wifi_service.cpp), so
 *    notifications go out on this module's own task and nowhere else.
 */

#include "dlna_module.h"
#include "dlna_transport.h"
#include "dlna_browser.h"
#include "browser_json.h"   // GET /api/dlna/browser's body (dlna_browser_http.inc)
#include "upnp_xml.h"
#include <new>

#include "modesp/net/http_routes.h"
#include "modesp/services/device_id.h"
#include "modesp/services/nvs_helper.h"
#include "device_name.h"
#include "modesp/liveness.h"
#include "state_enums.h"   // audio.source values, generated from the manifests
#include "pcm_ring.h"

#include "esp_dlna.h"
#include "esp_dlna_compat.h"
#include "esp_ssdp.h"
#include "media_lib_adapter.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char* TAG = "Dlna";


/// Where the device description lives. It is also advertised over SSDP, so the two must
/// agree; `esp_dlna` passes this through to the UPnP device as `root_path`.
constexpr const char* ROOT_PATH = "/rootDesc.xml";

/// Where the "I was in the middle of starting" mark lives.
///
/// Not a nicety. Twice now this renderer has taken the whole panel into a reboot loop, and a
/// loop is the one failure a person cannot work around: the device never stays up long
/// enough to be told to stop doing it. The mark is written before the risky call and cleared
/// after, so a boot that finds it set knows the last attempt did not return — and skips it.
/// The player then comes up with everything except the renderer, which is a device somebody
/// can use while the fault is looked at.
///
/// Raw NVS rather than the framework's persist service, whose flush is debounced by design:
/// a mark that survives only if the crash is polite enough to wait thirty seconds is not a
/// mark at all.
constexpr const char* NVS_NS   = "dlna";
constexpr const char* NVS_TRY  = "starting";

/// The port the framework's HTTP server listens on — HTTPD_DEFAULT_CONFIG's own default,
/// which http_service.cpp does not change. Advertised in the SSDP location, so a change
/// there has to be a change here.
constexpr int HTTP_PORT = 80;

/// audio.source value that means "the network stream" (docs/CONCEPT.md, "`audio.source` values").
/// A pushed track is played by the same engine as a radio station, so it takes the same
/// source. From generated/state_enums.h — see the note on the same constant in radio_module.cpp.
constexpr int32_t AUDIO_SRC_RADIO = modesp::gen::AUDIO_SOURCE_RADIO;

/// How long a pushed track may take to show any sign of life before it is given up on.
///
/// It covers a DNS lookup, a connection and the engine's own reporting interval, on a link
/// that loses packets under load (docs/NETWORK.md, "Wi-Fi through the ESP32-C6"). Generous on
/// purpose: the cost of waiting too long is a progress bar that says "loading" for a few
/// seconds, and the cost of giving up too early is a track that plays while the control point
/// shows it stopped.
constexpr uint32_t START_GIVE_UP_MS = 15000;

/// As long as SharedState can carry, which is also what modules/radio accepts: a media
/// server's URI carries item ids and session tokens and is nothing like a station URL.
static_assert(dlna_core::URI_CAP == MODESP_MAX_LONG_STRING_LENGTH + 1,
              "DLNA and radio URL capacities must match SharedState");

/// AVTransport's own words. A control point compares these strings, so they are spelled
/// the way the specification spells them and not the way this project would.
constexpr const char* TS_STOPPED       = "STOPPED";
constexpr const char* TS_PLAYING       = "PLAYING";
constexpr const char* TS_PAUSED        = "PAUSED_PLAYBACK";
constexpr const char* TS_TRANSITIONING = "TRANSITIONING";
constexpr const char* TS_NO_MEDIA      = "NO_MEDIA_PRESENT";

/// What this module's own task has been asked to do. Mostly announcements; the first one
/// is the start-up itself.
enum class Notify : uint8_t { START, TRANSPORT_STATE, AV_TRANSPORT, RENDERING_CONTROL };

/// The task's stack, in bytes.
///
/// Six kilobytes for something that spends its life asleep, because of what it does when it
/// wakes: esp_dlna_*_register_service() builds the whole service table — every action, every
/// attribute — as ARRAYS AND COMPOUND LITERALS ON THE STACK, and AVTransport alone is about
/// a kilobyte of them. Doing that on the main task, whose remaining headroom nobody
/// measures, is a stack overflow waiting for a quiet evening: a smashed return address
/// lands the CPU at an address that resolves to nothing, with a stack that will not unwind
/// — which is exactly what this module's panic at boot looked like.
constexpr uint32_t TASK_STACK = 6 * 1024;

struct DlnaState {
    DlnaModule* self = nullptr;           ///< the only way the C callback reaches SharedState
    esp_dlna_handle_t dlna = nullptr;
    httpd_handle_t    server = nullptr;   ///< the framework's server, handed over at boot
    std::atomic<bool> routes_up{false};               ///< the UPnP URIs are on the HTTP server
    bool ssdp_up   = false;               ///< we are advertising on the network
    /// Tried once and failed. Not tried again: a renderer that retries a failing start
    /// every quarter second is a log full of the same line and a heap fragmented by it.
    std::atomic<bool> failed{false};
    /// The start has been asked for and the task has not finished it yet.
    std::atomic<bool> starting{false};
    std::atomic<bool> clear_start_marker{false};

    char uuid[40]     = "";
    char udn[32]      = "";
    char friendly[40] = "";
    /// `friendly` as XML text. The library prints the root description's friendlyName as it
    /// is given, and "Living & Dining" left that document ill-formed: strict control points
    /// drop such a renderer. Six bytes is the most a character becomes.
    char friendly_xml[sizeof(friendly) * 6] = "";

    dlna_core::Transport* model = nullptr; // bounded storage in PSRAM
    dlna_core::Command* command = nullptr; // main-loop scratch, never on its stack
    SemaphoreHandle_t guard = nullptr;
    char active_uri[dlna_core::URI_CAP] = "";
    int32_t completion_seen = 0;
    bool pause_after_start = false; // main-loop owned
    std::atomic<bool> command_error{false};

    /// The transport state as this module believes it. Only ever one of the TS_ constants
    /// above, so a pointer swap is atomic enough to be read from the HTTPD task.
    std::atomic<const char*> trans_state{TS_NO_MEDIA};
    /// True while the thing modules/radio is playing is the thing we pushed. Without it,
    /// tuning a station from the panel would be reported to the control point as its own
    /// track playing.
    std::atomic<bool> ours{false};
    /// The stream engine has been seen to leave idle since we pushed.
    ///
    /// Needed because "idle" means two opposite things a second apart. The engine publishes
    /// its state twice a second, so for up to half a second after a push it still reads
    /// idle — the command is in its queue and the socket is not open yet. Reading that as
    /// "the track finished" is what made a pushed track report STOPPED while it was audibly
    /// playing, and it made every Seek and Pause afterwards be refused as somebody else's
    /// audio.
    bool started = false;
    /// Milliseconds since the push, while waiting for that first sign of life.
    uint32_t waited_ms = 0;

    /// Position and length, mirrored from the stream engine once a tick.
    ///
    /// Mirrored rather than read where they are asked for: a control point calls
    /// GetPositionInfo about once a second per subscriber, that call lands in the HTTPD
    /// task, and taking the SharedState mutex inside a SOAP response to fetch a number that
    /// changed at most once since the last tick is work for nothing.
    std::atomic<int32_t> pos_s{0};
    std::atomic<int32_t> dur_s{0};
    std::atomic<bool> seekable{false};


    QueueHandle_t notify_q = nullptr;
    TaskHandle_t  notify_task = nullptr;
    volatile bool run = false;
    /// Bumped once per turn of notify_task_fn, which is at least every 200 ms: its receive
    /// times out rather than blocking for ever, so the counter moves even with nothing to
    /// announce.
    modesp::liveness::Counter beat{0};
};

DlnaState d;

/// Post a GENA notification to be sent from our own task. Never blocks: a full queue means
/// a subscriber is slow, and dropping an announcement is better than delaying audio.
void notify(Notify what) {
    if (!d.notify_q) return;
    xQueueSend(d.notify_q, &what, 0);
}

/// Declared here because the task runs it; defined below, where the rest of the start-up is.
void start_renderer(void* ctx);

void notify_task_fn(void* ctx) {
    Notify what;
    while (d.run) {
        modesp::liveness::tick(d.beat);
        if (xQueueReceive(d.notify_q, &what, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (what == Notify::START) {
            start_renderer(ctx);
            ESP_LOGI(TAG, "start used %u B of this task's %u B stack",
                     (unsigned)(TASK_STACK - uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)),
                     (unsigned)TASK_STACK);
            continue;
        }
        if (!d.dlna) continue;
        switch (what) {
        case Notify::START: break;      // handled above
        case Notify::TRANSPORT_STATE:   esp_dlna_notify_avt_by_action(d.dlna, "TransportState"); break;
        case Notify::AV_TRANSPORT:      esp_dlna_notify(d.dlna, "AVTransport"); break;
        case Notify::RENDERING_CONTROL: esp_dlna_notify(d.dlna, "RenderingControl"); break;
        }
    }
    d.notify_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void set_trans_state(const char* st) {
    if (d.trans_state.load(std::memory_order_acquire) == st) return;
    d.trans_state.store(st, std::memory_order_release);
    notify(Notify::TRANSPORT_STATE);
}

dlna_core::Mode transport_mode() {
    const char* state = d.trans_state.load();
    return state == TS_PAUSED ? dlna_core::Mode::Paused :
           (state == TS_PLAYING || state == TS_TRANSITIONING) ? dlna_core::Mode::Playing :
           dlna_core::Mode::Stopped;
}

/// Seconds as UPnP writes time: "H:MM:SS", which is what every control point parses.
int fmt_time(char* out, int max, uint32_t secs) {
    return snprintf(out, max, "%u:%02u:%02u", (unsigned)(secs / 3600),
                    (unsigned)((secs / 60) % 60), (unsigned)(secs % 60));
}

// Queries and command submission share this mutex; no network or audio I/O occurs
// under it. All commands, including their Play identity, retain FIFO order.
int renderer_request(esp_dlna_handle_t, const upnp_attr_t* attr, int attr_num,
                     char* buffer, int max_len) {
    if (attr_num != 1 || !attr || !d.self || !d.model || !d.guard) return 0;
    if (xSemaphoreTake(d.guard, pdMS_TO_TICKS(1000)) != pdTRUE) {
        if (buffer && max_len > 0) buffer[0] = 0;
        return 0;
    }
    struct Unlock { ~Unlock() { xSemaphoreGive(d.guard); } } unlock;
    auto& m = *d.model;
    const int req = attr->type & 0xFF;
    const char* input = buffer ? buffer : "";
    auto result = [](bool ok) {
        d.command_error.store(!ok, std::memory_order_release);
        if (!ok) ESP_LOGW(TAG, "transport request rejected (invalid input or queue full)");
        return ok ? 0 : -1;
    };
    using dlna_core::Action;
    switch (req) {
    case RCS_GET_VOL: return snprintf(buffer, max_len, "%d", (int)d.self->state_int("audio.volume", 30));
    case RCS_SET_VOL: {
        const int v = atoi(input);
        d.self->put_int("audio.volume", v < 0 ? 0 : v > 100 ? 100 : v);
        notify(Notify::RENDERING_CONTROL); return 0;
    }
    case RCS_GET_MUTE: return snprintf(buffer, max_len, "%d", d.self->state_flag("audio.mute", false) ? 1 : 0);
    case RCS_SET_MUTE: d.self->put_flag("audio.mute", atoi(input) != 0); notify(Notify::RENDERING_CONTROL); return 0;
    case AVT_SET_TRACK_URI:
        if (!m.current.set_uri(input)) return result(false);
        m.next = dlna_core::Track{};
        m.previous = dlna_core::Track{};
        if (!d.ours.load()) set_trans_state(m.current.uri[0] ? TS_STOPPED : TS_NO_MEDIA);
        notify(Notify::AV_TRANSPORT); return result(true);
    case AVT_SET_TRACK_METADATA: return result(m.current.set_metadata(input));
    case AVT_SET_NEXT_URI:
        if (!m.next.set_uri(input)) return result(false);
        notify(Notify::AV_TRANSPORT); return result(true);
    case AVT_SET_NEXT_METADATA:
        if (!m.next.set_metadata(input)) return result(false);
        notify(Notify::AV_TRANSPORT); return result(true);
    case AVT_GET_TRACK_URI: return dlna_core::xml_reply(m.current.uri, buffer, max_len > 0 ? max_len : 0);
    case AVT_GET_TRACK_METADATA: return dlna_core::xml_reply(m.current.metadata, buffer, max_len > 0 ? max_len : 0);
    case AVT_GET_NEXT_URI: return dlna_core::xml_reply(m.next.uri, buffer, max_len > 0 ? max_len : 0);
    case AVT_GET_NEXT_METADATA: return dlna_core::xml_reply(m.next.metadata, buffer, max_len > 0 ? max_len : 0);
    case AVT_PLAY: return result(m.push(Action::Play));
    case AVT_STOP: return result(m.push(Action::Stop));
    case AVT_PAUSE: return result(m.push(Action::Pause));
    case AVT_SEEK: {
        int32_t seconds = 0;
        return result(d.ours.load(std::memory_order_acquire) &&
                      d.seekable.load(std::memory_order_acquire) &&
                      dlna_core::time_seconds(input, seconds) && m.push(Action::Seek, seconds));
    }
    case AVT_NEXT: return result(m.advance(transport_mode()));
    case AVT_PREV: return result(m.back(transport_mode()));
    case AVT_GET_TRANS_STATE: return snprintf(buffer, max_len, "%s", d.trans_state.load());
    case AVT_GET_TRANS_STATUS: return snprintf(buffer, max_len, "%s", d.command_error.load() ? "ERROR_OCCURRED" : "OK");
    case AVT_GET_PLAY_SPEED: return snprintf(buffer, max_len, "1");
    case AVT_GET_PLAY_MODE: return snprintf(buffer, max_len, "NORMAL");
    case AVT_GET_TRACK_NO: return snprintf(buffer, max_len, "%d", m.current.uri[0] ? 1 : 0);
    case AVT_GET_ACTIONS:
        return snprintf(buffer, max_len, "%s%s%s", m.current.uri[0] ?
                        (d.ours.load(std::memory_order_acquire) &&
                         d.seekable.load(std::memory_order_acquire) ?
                         "Play,Stop,Pause,Seek,Previous" : "Play,Stop,Pause,Previous") : "",
                        m.current.uri[0] && m.next.uri[0] ? "," : "", m.next.uri[0] ? "Next" : "");
    case AVT_GET_TRACK_DURATION:
    case AVT_GET_MEDIA_DURATION: return fmt_time(buffer, max_len, (uint32_t)d.dur_s.load());
    case AVT_GET_POS_RELTIME:
    case AVT_GET_POS_ABSTIME: return fmt_time(buffer, max_len, (uint32_t)d.pos_s.load());
    case AVT_GET_POS_RELCOUNT:
    case AVT_GET_POS_ABSCOUNT: return snprintf(buffer, max_len, "%d", 2147483647);
    default: return 0;
    }
}

/// Called by HttpService in the window before the static `/\*` wildcard goes down.
///
/// It takes the handle and nothing else. Starting UPnP here — during boot, on the main
/// task, before there is a network — is what this module once did, and a fault inside it
/// left a device that would not start at all. So the boot only hands over the server; the
/// renderer comes up later, on its own, when there is a network to be discovered on, and
/// puts the wildcard back on top itself.
#include "dlna_soap.inc"
#include "dlna_browser_http.inc"

void register_routes(httpd_handle_t server, void* ctx) {
    (void)ctx;
    d.server = server;
    httpd_uri_t browser{};browser.uri="/api/dlna/browser";browser.method=HTTP_GET;browser.handler=browser_http;
    if(httpd_register_uri_handler(server,&browser)!=ESP_OK)ESP_LOGW(TAG,"browser diagnostics route unavailable");
    ESP_LOGI(TAG, "http server handed over; renderer starts when the network is up");
}

size_t free_internal() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

/// Bring the renderer up: the three services, then the wildcard back on top of them.
///
/// Every step is logged with the heap it had to work with, and the log line comes BEFORE
/// the call rather than after. The panic at boot was a jump to an address that resolves to
/// nothing, with a stack that does not unwind, and the log stopped at the line before the
/// whole sequence — which narrowed it to six calls and no further. Whatever the next boot
/// does, it will say which one.
void start_renderer(void* ctx) {
    if (!d.server || d.routes_up || d.failed) return;
    if (!d.self) { d.failed = true; return; }

    ESP_LOGI(TAG, "starting renderer, internal heap %u B", (unsigned)free_internal());

    dlna_config_t cfg = {};
    cfg.friendly_name = d.friendly_xml;
    cfg.uuid          = d.uuid;
    cfg.httpd         = d.server;
    cfg.httpd_port    = HTTP_PORT;
    cfg.renderer_req  = renderer_request;
    cfg.root_path     = ROOT_PATH;
    cfg.user_ctx      = ctx;
    cfg.device_list   = false;
    // logo stays zeroed: this device advertises no icon. esp_upnp_init substitutes a
    // default for every config string it finds NULL — checked in its disassembly, which is
    // also where the struct layout was confirmed to match this header (offset 68 = httpd,
    // total 84 bytes, and the library calloc's exactly 84).

    // The catch-all comes off first. It matches every GET, and httpd checks for duplicates
    // through the same match function, so with it on the server every route below is refused
    // with "already registered" — and the library, having had its routes turned down, walks
    // into something it never set up. That is what the panic at boot was, both times.
    modesp::http_routes::wildcard(false);

    // And the ABI shim goes on for exactly the length of this call. The library hands
    // httpd a sixteen-byte httpd_uri_t while ours is twenty-four — see
    // components/esp_dlna/httpd_uri_compat.c, which has the disassembly and the panic.
    esp_dlna_avt_handler(avt_soap);
    esp_dlna_httpd_compat(true);
    ESP_LOGI(TAG, "  esp_dlna_start…");
    d.dlna = esp_dlna_start(&cfg);
    esp_dlna_httpd_compat(false);
    modesp::http_routes::wildcard(true);
    if (!d.dlna) {
        ESP_LOGE(TAG, "esp_dlna_start failed — the renderer will not answer");
        d.clear_start_marker.store(true);
        d.failed = true;
        d.starting = false;
        return;
    }
    ESP_LOGI(TAG, "  services registered, internal heap %u B", (unsigned)free_internal());

    d.clear_start_marker.store(true);
    d.routes_up = true;
    d.starting = false;
    ESP_LOGI(TAG, "renderer on http://<ip>%s as \"%s\"", ROOT_PATH, d.friendly);
}

void ssdp_start() {
    static char location[64];
    snprintf(location, sizeof(location), "http://${ip}%s", ROOT_PATH);

    // One advertisement per thing a control point may search for. Music Assistant looks for
    // the device type, Windows for upnp:rootdevice, and some apps for AVTransport alone —
    // a renderer that answers only one of them is found by only some of them.
    static ssdp_service_t services[6];
    services[0] = {d.uuid, (char*)"upnp:rootdevice", nullptr};
    services[1] = {d.uuid, (char*)"urn:schemas-upnp-org:device:MediaRenderer:1", nullptr};
    services[2] = {d.uuid, (char*)"urn:schemas-upnp-org:service:ConnectionManager:1", nullptr};
    services[3] = {d.uuid, (char*)"urn:schemas-upnp-org:service:RenderingControl:1", nullptr};
    services[4] = {d.uuid, (char*)"urn:schemas-upnp-org:service:AVTransport:1", nullptr};
    services[5] = {nullptr, nullptr, nullptr};

    // Filled field by field rather than with SSDP_DEFAULT_CONFIG(): that macro assigns a
    // string literal to a `char *`, which C accepts and C++ does not.
    ssdp_config_t cfg = {};
    cfg.udn                = d.udn;
    cfg.location           = location;
    cfg.port               = DEFAULT_SSDP_PORT;
    cfg.notify_interval_ms = DEFAULT_SSDP_NOTIFY_INTERVAL_MS;
    cfg.task_stack         = DEFAULT_SSDP_TASK_STACK;
    cfg.task_prio          = DEFAULT_SSDP_TASK_PRIORITY;
    // Its stack stays in INTERNAL memory, against the component's own default.
    //
    // Not because of anything measured — a run of silent resets that looked like evidence
    // for this turned out to be the capture script resetting the board, not the board
    // resetting itself. The reason is the rule docs/CONCEPT.md, "Board constraints" already
    // records: a task whose stack is in PSRAM must not run while the flash cache is disabled,
    // and on this device NVS and the data partition are written from other tasks often enough
    // that a long-lived task cannot promise never to be scheduled during one. Four kilobytes of
    // internal memory is a smaller price than a fault that appears once a week.
    cfg.stack_in_ext       = false;
    cfg.task_core          = 0;

    if (esp_ssdp_start(&cfg, services) != ESP_OK) {
        ESP_LOGW(TAG, "SSDP did not start — the device will not be discovered");
        return;
    }
    d.ssdp_up = true;
    ESP_LOGI(TAG, "SSDP advertising %s", d.uuid);
}

void ssdp_stop() {
    if (!d.ssdp_up) return;
    esp_ssdp_send_byebye();     // leave the network tidy: a renderer that vanishes without
    esp_ssdp_stop();            // this one stays in every app's device list until it ages out
    d.ssdp_up = false;
    ESP_LOGI(TAG, "SSDP stopped");
}

}  // namespace

// ── the module ────────────────────────────────────────────────────────────────────────────

DlnaModule::DlnaModule() : modesp::BaseModule("dlna") {}

bool DlnaModule::on_init() {
    // Flash writes must run on main's internal stack, never the PSRAM worker.
    bool crashed_last_time = false;
    modesp::nvs_helper::read_bool(NVS_NS, NVS_TRY, crashed_last_time);
    if (crashed_last_time) {
        modesp::nvs_helper::erase_key(NVS_NS, NVS_TRY);
        d.failed = true;
        ESP_LOGE(TAG, "previous renderer start did not return; disabled for this boot");
    }
    // media_lib_sal is the shim the protocol library calls through. Its OS half (malloc,
    // mutex, event groups) registers itself from a constructor before app_main; this call
    // adds the socket, TLS, crypt and netif halves, which SSDP needs. Checked rather than
    // ignored: every media_lib_* entry point returns null or NOT_SUPPORTED when its wrapper
    // is missing, so a failure here would show up much later as something unrelated.
    const esp_err_t adapter = media_lib_add_default_adapter();
    if (adapter != ESP_OK) {
        ESP_LOGE(TAG, "media_lib adapters not registered (%s) — UPnP cannot run",
                 esp_err_to_name(adapter));
    }
    d.guard = xSemaphoreCreateMutex();
    void* model_mem = heap_caps_malloc(sizeof(dlna_core::Transport), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void* command_mem = heap_caps_malloc(sizeof(dlna_core::Command), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d.guard || !model_mem || !command_mem) {
        free(model_mem); free(command_mem);
        if (d.guard) vSemaphoreDelete(d.guard);
        d.guard = nullptr;
        return false;
    }
    d.model = new (model_mem) dlna_core::Transport{};
    d.command = new (command_mem) dlna_core::Command{};
    d.self = this;

    const uint8_t* mac = modesp::device_mac();
    // A UUID a control point can remember. It has to survive a reboot — an app that saw
    // this device yesterday looks it up by UUID, and a fresh one every boot leaves a
    // graveyard of dead entries in everyone's device list. The MAC gives us that for free.
    snprintf(d.uuid, sizeof(d.uuid), "%08x-0000-1000-8000-%02x%02x%02x%02x%02x%02x",
             0x50344453u /* "P4DS" */, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(d.udn, sizeof(d.udn), "P4_DMR_%s", modesp::device_id());
    // The friendly name people see in the list of devices: the panel's one name (device_name.h),
    // as AirPlay and Bluetooth have it, with no hex tail (it was «P4 Player D12C4C»). Control
    // points know the renderer by its UUID, which stays. A new
    // name reaches DLNA at the next start.
    device_name::get(d.friendly, sizeof(d.friendly));
    dlna_core::xml_reply(d.friendly, d.friendly_xml, sizeof(d.friendly_xml));

    if (!dlna_client::init()) ESP_LOGE(TAG, "MediaServer browser allocation failed");
    // What PersistService restored, not `true`: this key is persisted, and writing the default
    // over it here would switch the renderer back on at every boot — the panel's DLNA switch
    // would hold only until the power went off.
    state_set("dlna.enabled", read_bool("dlna.enabled", true));
    // The same word the renderer would answer a control point with right now, rather than a
    // friendlier one: this key and the SOAP response must never disagree.
    state_set("dlna.state", TS_NO_MEDIA);
    state_set("dlna.name", d.friendly);
    state_set("dlna.uri", "");
    state_set("dlna.title", "");
    state_set("dlna.artist", "");
    state_set("dlna.album", "");
    state_set("dlna.art_uri", "");

    d.notify_q = xQueueCreate(8, sizeof(Notify));
    if (d.notify_q) {
        d.run = true;
        // Priority 3: above the main loop, below the audio and stream tasks. It sends short
        // HTTP requests and then blocks, which is exactly what it is here to do somewhere
        // other than the main loop.
        if (xTaskCreatePinnedToCoreWithCaps(notify_task_fn, "dlna", TASK_STACK, this, 3,
                                    &d.notify_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGW(TAG, "notify task not created — events will not be sent");
            d.run = false;
        } else {
            // 10 s rather than the 200 ms beat, because this task also runs the one-shot
            // renderer start — the call that once did not return and is already
            // guarded by its own NVS flag. Tight enough to catch a wedge, loose enough
            // that a merely slow start is not reported as one.
            modesp::liveness::watch("dlna", &d.beat, 10000, this);
        }
    }

    // The server does not exist yet: it is started by a LOW-priority service, after every
    // product module has been initialised. This asks to be handed the handle when it does.
    if (!modesp::http_routes::add(register_routes, this)) {
        ESP_LOGE(TAG, "no room in the route table — the renderer will not be reachable");
    }
    return true;
}

void DlnaModule::on_update(uint32_t dt_ms) {
    if (!d.model || !d.guard) return;
    if (d.clear_start_marker.exchange(false))
        modesp::nvs_helper::erase_key(NVS_NS, NVS_TRY);
    // Both of this module's command keys arrive on the one queue and are told apart by the
    // key each entry carries, so a browse token and a transport press cannot overwrite each
    // other the way two writes to one state key did.
    //
    // A transport press is HELD until the model's guard can be taken. The queue has to be
    // drained whether or not the guard is free — leaving entries in it would stall the
    // browse tokens behind them — but a press that cannot be applied this turn must not be
    // thrown away: while a SOAP request holds the guard, the person is still pressing Play.
    // The state key it replaced survived the same way, by simply not being cleared.
    static int32_t ui_cmd = 0;
    modesp::BaseModule::Command dc;
    while (next_command(dc)) {
        if (!strcmp(dc.key, "dlna.browse_cmd")) {
            if (dc.value) dlna_client::request(dc.value);
        } else if (dc.value) {
            ui_cmd = dc.value;          // the last transport press wins
        }
    }
    // One command per main-loop turn: give the radio module a chance to consume
    // its SharedState mailbox before posting another operation.
    bool have_command = false;
    if (xSemaphoreTake(d.guard, 0) == pdTRUE) {
        bool next=false;
        if(dlna_client::take_track(d.command->track,next)) {
            if(next) {
                d.model->next=d.command->track;
                ESP_LOGI(TAG, "next slot set: %s", d.model->next.uri);
            }
            else {d.model->current=d.command->track;d.model->next=dlna_core::Track{};d.command_error.store(!d.model->push(dlna_core::Action::Play,0,dlna_core::Mode::Playing,true));}
            notify(Notify::AV_TRANSPORT);
        }
        // Drained above, before the guard was taken; cleared only now that it has been
        // applied, which is what makes a press survive a busy turn.
        if (ui_cmd) {
            bool ok = false;
            using dlna_core::Action;
            switch (ui_cmd) {
            case 1: ok = d.model->push(Action::Play); break;
            case 2: ok = d.model->push(Action::Stop); break;
            case 3: ok = d.model->push(Action::Pause); break;
            case 4: ok = d.model->advance(transport_mode()); break;
            case 5: ok = d.model->back(transport_mode()); break;
            }
            d.command_error.store(!ok);
            ui_cmd = 0;
        }
        char pending_url[dlna_core::URI_CAP]{};
        read_string("radio.play_url",pending_url,sizeof(pending_url));
        if(!pending_url[0])have_command = d.model->pop(*d.command);
        state_set("radio.next_url", d.ours.load() ? d.model->next.uri : "");
        xSemaphoreGive(d.guard);
    }
    if (have_command) {
        auto& cmd = *d.command;
        char mode[12] = "", uri[dlna_core::URI_CAP] = "";
        read_string("radio.mode", mode, sizeof(mode));
        read_string("radio.url", uri, sizeof(uri));
        char ended[dlna_core::URI_CAP]{}, promoted[dlna_core::URI_CAP]{};
        read_string("radio.completed_uri", ended, sizeof(ended));
        read_string("radio.promoted_uri", promoted, sizeof(promoted));
        // A command can arrive between the engine's handoff and our next poll.
        // Stop/Pause still own that stream during this short adoption window.
        const bool handoff = dlna_core::pending_handoff(
            d.active_uri, uri, ended, promoted, read_int("radio.completed", 0) != d.completion_seen);
        const bool ours = d.ours.load() && read_int("audio.source", 0) == AUDIO_SRC_RADIO &&
            (!d.started || (!strcmp(mode, "push") && (!strcmp(uri, d.active_uri) || handoff)));
        using dlna_core::Action;
        switch (cmd.action) {
        case Action::Adopt:
            // The stream engine already appended this track behind the previous
            // PCM tail. Update ownership/metadata without starting its URL again.
            if (read_int("audio.source", 0) == AUDIO_SRC_RADIO &&
                !strcmp(mode, "push") && !strcmp(uri, cmd.track.uri)) {
                snprintf(d.active_uri, sizeof(d.active_uri), "%s", cmd.track.uri);
                d.ours.store(true);
                d.started = true;
                d.pause_after_start = false;
                d.completion_seen = read_int("radio.completed", 0);
                d.pos_s.store(read_int("radio.pos_s", 0));
                d.dur_s.store(read_int("radio.dur_s", 0));
                state_set("dlna.uri", cmd.track.uri);
                state_set("dlna.title", cmd.track.title);
                state_set("dlna.artist", cmd.track.artist);
                state_set("dlna.album", cmd.track.album);
                state_set("dlna.art_uri", cmd.track.art);
                dlna_client::artwork(cmd.track.art);
                set_trans_state(audio_core::pcm_ring().paused() ? TS_PAUSED : TS_PLAYING);
                notify(Notify::AV_TRANSPORT);
            }
            break;
        case Action::Stop:
            d.pause_after_start = false;
            if (ours) {
                // The pause stays on until the radio has flushed: lifting it here let the
                // paused track play on until the stop arrived.
                post_command_to("radio", "radio.cmd", (int32_t)2);
                d.ours.store(false);
                d.pos_s.store(0);
            }
            set_trans_state(TS_STOPPED);
            break;
        case Action::Pause:
            if (ours && !d.started) d.pause_after_start = true;
            if (ours && d.started) { audio_core::pcm_ring().set_paused(true); set_trans_state(TS_PAUSED); }
            break;
        case Action::Seek:
            if (ours && d.seekable.load(std::memory_order_acquire) && cmd.seconds >= 0)
                state_set("radio.seek_s", cmd.seconds);
            break;
        case Action::Select:
            if (ours) post_command_to("radio", "radio.cmd", (int32_t)2);
            d.ours.store(false);
            d.pause_after_start = false;
            state_set("dlna.uri", cmd.track.uri);
            state_set("dlna.title", cmd.track.title);
            state_set("dlna.artist", cmd.track.artist);
            state_set("dlna.album", cmd.track.album);
            state_set("dlna.art_uri", cmd.track.art);
            dlna_client::artwork(cmd.track.art);
            d.pos_s.store(0); d.dur_s.store(0);
            set_trans_state(cmd.mode == dlna_core::Mode::Paused ? TS_PAUSED : TS_STOPPED);
            notify(Notify::AV_TRANSPORT);
            break;
        case Action::Play:
            if (!cmd.restart && ours && strcmp(d.active_uri, cmd.track.uri) == 0) {
                // The track that is already ours: resume it if it is paused, and otherwise
                // leave it alone. Judged by the ring's pause, not by the transport state — a
                // re-buffer while paused passes through TRANSITIONING — and a Play repeated for
                // a playing track must not start it over.
                d.pause_after_start = false;
                if (d.started && audio_core::pcm_ring().paused()) {
                    audio_core::pcm_ring().set_paused(false);
                    set_trans_state(TS_PLAYING);
                }
            } else if (cmd.track.uri[0]) {
                state_set("audio.source", AUDIO_SRC_RADIO);
                state_set("radio.play_url", cmd.track.uri);
                snprintf(d.active_uri, sizeof(d.active_uri), "%s", cmd.track.uri);
                d.completion_seen=read_int("radio.completed",0);
                d.ours.store(true); d.started = false; d.waited_ms = 0;
                d.pause_after_start = cmd.mode == dlna_core::Mode::Paused;
                d.pos_s.store(0); d.dur_s.store(0);
                set_trans_state(TS_TRANSITIONING);
                state_set("dlna.uri", cmd.track.uri);
                state_set("dlna.title", cmd.track.title);
                state_set("dlna.artist", cmd.track.artist);
                state_set("dlna.album", cmd.track.album);
                state_set("dlna.art_uri", cmd.track.art);
            dlna_client::artwork(cmd.track.art);
                notify(Notify::AV_TRANSPORT);
            }
            break;
        }
    }

    poll_ms_ += dt_ms;
    if (poll_ms_ < 250) return;
    poll_ms_ = 0;

    // ── advertise only while there is a network to advertise on ──
    const bool enabled = read_bool("dlna.enabled", true);
    const bool net     = read_bool("wifi.connected", false);
    if (enabled != last_enabled_) {
        last_enabled_ = enabled;
        ESP_LOGI(TAG, "renderer %s", enabled ? "enabled" : "disabled");
    }
    // Up only when there is a network to be found on, and only once — and on this module's
    // own task, not on the main loop, for the stack reason written at TASK_STACK.
    if (!d.routes_up && !d.failed && !d.starting && enabled && net) {
        if (d.notify_q && d.run) {
            modesp::nvs_helper::write_bool(NVS_NS, NVS_TRY, true);
            d.starting = true;
            Notify start = Notify::START;
            if (xQueueSend(d.notify_q, &start, 0) != pdTRUE) {
                d.starting = false;
                modesp::nvs_helper::erase_key(NVS_NS, NVS_TRY);
            }
        }
    }
    if (d.routes_up && enabled && net && !d.ssdp_up) ssdp_start();
    if (d.ssdp_up && (!enabled || !net))             ssdp_stop();

    // ── follow what the stream engine is actually doing ──
    //
    // Only while it is playing OUR track: the same engine plays radio stations, and a
    // station tuned from the panel is not this renderer's business to report.
    if (d.ours.load(std::memory_order_acquire)) {
        // The progress bar's two numbers, taken once here instead of on every SOAP request.
        d.pos_s.store(read_int("radio.pos_s", 0), std::memory_order_release);
        d.dur_s.store(read_int("radio.dur_s", 0), std::memory_order_release);
        d.seekable.store(read_bool("radio.seekable", false), std::memory_order_release);
        char rs[16] = "", mode[12] = "", uri[dlna_core::URI_CAP] = "";
        read_string("radio.state", rs, sizeof(rs));
        read_string("radio.mode", mode, sizeof(mode));
        read_string("radio.url", uri, sizeof(uri));
        const bool source_matches = read_int("audio.source", 0) == AUDIO_SRC_RADIO;
        const bool playing_ours = source_matches && strcmp(mode, "push") == 0 && strcmp(uri, d.active_uri) == 0;
        const bool alive = (strcmp(rs, "playing") == 0 || strcmp(rs, "connecting") == 0 ||
                            strcmp(rs, "buffering") == 0 || strcmp(rs, "reconnecting") == 0);
        if (alive && playing_ours) d.started = true;

        char ended[dlna_core::URI_CAP]{};
        read_string("radio.completed_uri",ended,sizeof(ended));
        const int32_t completed=read_int("radio.completed",0);
        if(source_matches && completed!=d.completion_seen && !strcmp(ended,d.active_uri)) {
            // Retry on the next tick if a SOAP request owns the model.
            if(xSemaphoreTake(d.guard,0)!=pdTRUE) return;
            char promoted[dlna_core::URI_CAP]{};
            read_string("radio.promoted_uri", promoted, sizeof(promoted));
            const bool prepared = promoted[0] && !strcmp(promoted, d.model->next.uri);
            const bool had_next = d.model->next.uri[0] != 0;
            const bool advanced=d.model->advance(dlna_core::Mode::Playing, prepared);
            xSemaphoreGive(d.guard);
            // A saturated command queue is not the end of the playlist. Retry
            // this completion after pending commands have been consumed.
            if (had_next && !advanced) return;
            d.completion_seen=completed;
            ESP_LOGI(TAG, "track completion: %s, next %s", ended,
                     advanced ? (prepared ? "adopted" : "scheduled") : "empty");
            d.ours.store(false); d.pause_after_start=false;
            set_trans_state(advanced?TS_TRANSITIONING:TS_STOPPED);
            notify(Notify::AV_TRANSPORT);
        } else if (!source_matches || (d.started && !playing_ours)) {
            dlna_client::artwork("");
            d.ours.store(false);
            d.pause_after_start = false;
            d.pos_s.store(0); d.dur_s.store(0);
            set_trans_state(TS_STOPPED);
        } else if (!d.started) {
            // Still waiting for the engine to admit it has the track. Nothing it says is
            // meaningful yet, including "idle" — which is what it said half a second ago
            // because the command was still in its queue.
            d.waited_ms += 250;
            if (d.waited_ms >= START_GIVE_UP_MS) {
                ESP_LOGW(TAG, "the stream engine never started the pushed track");
                d.ours.store(false, std::memory_order_release);
                set_trans_state(TS_STOPPED);
            }
        } else if (strcmp(rs, "playing") == 0) {
            if (d.pause_after_start) {
                audio_core::pcm_ring().set_paused(true);
                d.pause_after_start = false;
                set_trans_state(TS_PAUSED);
            } else if (!audio_core::pcm_ring().paused()) set_trans_state(TS_PLAYING);
        } else if ((strcmp(rs, "connecting") == 0 || strcmp(rs, "buffering") == 0) &&
                   !audio_core::pcm_ring().paused()) {
            // Not while paused: a seek re-buffers behind the pause, and PAUSED_PLAYBACK is still
            // what the control point should see.
            set_trans_state(TS_TRANSITIONING);
        } else if (strcmp(rs, "idle") == 0) {
            // Now it means what it says: the track was read to its end, or somebody stopped
            // it. A control point that is not told sits showing a progress bar for a track
            // that finished a minute ago.
            d.ours.store(false, std::memory_order_release);
            set_trans_state(TS_STOPPED);
        }
    }

    // ── let a control point's own volume slider follow the panel's knob ──
    const int32_t vol = read_int("audio.volume", -1);
    const bool    mut = read_bool("audio.mute", false);
    if (vol != last_volume_ || (int8_t)mut != last_mute_) {
        if (last_volume_ >= 0 && d.dlna) notify(Notify::RENDERING_CONTROL);
        last_volume_ = vol;
        last_mute_   = (int8_t)mut;
    }

    state_set("dlna.state", d.trans_state.load(std::memory_order_acquire));
}

void DlnaModule::on_stop() {
    modesp::liveness::unwatch("dlna");
    ssdp_stop();
    d.run = false;
    dlna_client::stop();
}
