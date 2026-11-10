/**
 * @file ws_service.cpp
 * @brief WebSocket real-time state broadcast implementation
 */

#include "modesp/net/ws_service.h"
#include "modesp/shared_state.h"
#include "modesp/types.h"
#include "modesp/net/state_json.h"

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "sys/socket.h"
#include "sys/time.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

static const char* TAG = "WS";

// IDF v5.5.5 and v6.0.1 stopped calling a WebSocket URI handler with HTTP_GET for the handshake
// (espressif/esp-idf#18539; migration guide 6.0, "WebSocket Handler No Longer Called During
// Handshake"). v5.5.4 and v6.0 still call it. Without the post-handshake callback no client is
// ever registered here, and none is ever sent the state, a delta or a PING.
#if ((ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 5) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)) || \
     ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 1)) && !defined(CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT)
#error "Set CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y: this IDF does not call the /ws handler for the handshake"
#endif

namespace modesp {

// ── Client management (BUG-021: all access under clients_mutex_) ──

/// Milliseconds, wrapping at 32 bits; ws_slots.h subtracts these unsigned.
static uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool WsService::remove_client(int fd) {
    if (!clients_mutex_) return false;
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (slots_[i].fd == fd) {
            slots_[i] = net::WsSlot{};
            xSemaphoreGive(clients_mutex_);
            ESP_LOGI(TAG, "Client disconnected (fd=%d, slot=%d)", fd, (int)i);
            return true;
        }
    }
    xSemaphoreGive(clients_mutex_);
    return false;
}

bool WsService::has_client(int fd) const {
    if (!clients_mutex_) return false;
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    bool found = false;
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (slots_[i].fd == fd) { found = true; break; }
    }
    xSemaphoreGive(clients_mutex_);
    return found;
}

void WsService::heard(int fd) {
    if (!clients_mutex_) return;
    const uint32_t now = now_ms();
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (slots_[i].fd == fd) { slots_[i].heard_ms = now; break; }
    }
    xSemaphoreGive(clients_mutex_);
}

// A client that missed the snapshot or a delta holds state the panel no longer matches, and
// only a reconnect (with its fresh snapshot) puts that right. Freeing the slot instead, as this
// did before, left the session open: the browser stayed connected and was never sent anything.
void WsService::mark_lost(int fd) {
    if (!clients_mutex_) return;
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (slots_[i].fd == fd) { slots_[i].lost = true; break; }
    }
    xSemaphoreGive(clients_mutex_);
}

void WsService::mark_all_lost() {
    if (!clients_mutex_) return;
    xSemaphoreTake(clients_mutex_, portMAX_DELAY);
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (slots_[i].fd >= 0) slots_[i].lost = true;
    }
    xSemaphoreGive(clients_mutex_);
}

bool WsService::is_ws_client(int fd) const {
    if (!server_ || fd < 0) return false;
    return httpd_ws_get_fd_info(server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET;
}

// Called by httpd when an admitted client's session is freed (TCP RST, timeout, clean CLOSE,
// our own close). Before this the context carried no fd, and the sweep it ran then could not
// see the closing session: httpd still lists it as WebSocket until after this callback.
void WsService::on_session_close(void* ctx) {
    auto* sess = static_cast<SessionCtx*>(ctx);
    if (!sess) return;
    sess->self->remove_client(sess->fd);
    free(sess);
}

// Bytes waiting in the socket are the client talking; httpd just has not read them yet. The
// heartbeat is queued work, which httpd runs ahead of the sessions in each pass of its loop,
// and after a long handler (an OTA upload) every PONG of that time is still waiting.
static bool has_unread(int fd) {
    uint8_t byte;
    return recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) > 0;
}

// Executed in the httpd thread: the sessions it closes are httpd's own, and nothing races it.
void WsService::heartbeat_cb(void* arg) {
    auto* self = static_cast<WsService*>(arg);
    if (!self->server_ || !self->clients_mutex_) return;
    const uint32_t now = now_ms();

    struct Gone { int fd; uint32_t silent_ms; bool lost; };
    Gone gone[MAX_WS_CLIENTS];
    int ping[MAX_WS_CLIENTS];
    size_t n_gone = 0, n_ping = 0;

    xSemaphoreTake(self->clients_mutex_, portMAX_DELAY);
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        net::WsSlot& s = self->slots_[i];
        if (s.fd < 0) continue;
        if (!self->is_ws_client(s.fd)) {
            ESP_LOGI(TAG, "Removing stale client fd=%d (slot=%d)", s.fd, (int)i);
            s = net::WsSlot{};
            continue;
        }
        if (has_unread(s.fd)) s.heard_ms = now;
        if (net::ws_should_close(s, now, STALE_AFTER_MS)) {
            gone[n_gone++] = {s.fd, net::ws_silent_ms(s, now), s.lost};
            s = net::WsSlot{};
        } else {
            ping[n_ping++] = s.fd;
        }
    }
    xSemaphoreGive(self->clients_mutex_);

    // Closed, not just forgotten: the browser sees its socket end and reconnects.
    for (size_t i = 0; i < n_gone; i++) {
        if (gone[i].lost) {
            ESP_LOGW(TAG, "Closing fd=%d: it missed a message", gone[i].fd);
        } else {
            ESP_LOGW(TAG, "Closing fd=%d: silent for %lu ms", gone[i].fd,
                     (unsigned long)gone[i].silent_ms);
        }
        httpd_sess_trigger_close(self->server_, gone[i].fd);
    }

    httpd_ws_frame_t ping_frame = {};
    ping_frame.type = HTTPD_WS_TYPE_PING;

    char hb[24];
    const int hb_len = snprintf(hb, sizeof(hb), "{\"_hb\":%lu}",
                                (unsigned long)++self->heartbeat_seq_);
    httpd_ws_frame_t hb_frame = {};
    hb_frame.type    = HTTPD_WS_TYPE_TEXT;
    hb_frame.payload = reinterpret_cast<uint8_t*>(hb);
    hb_frame.len     = static_cast<size_t>(hb_len);

    for (size_t i = 0; i < n_ping; i++) {
        if ((httpd_ws_send_frame_async(self->server_, ping[i], &ping_frame) != ESP_OK ||
             httpd_ws_send_frame_async(self->server_, ping[i], &hb_frame) != ESP_OK) &&
            self->remove_client(ping[i])) {
            ESP_LOGW(TAG, "Heartbeat failed fd=%d — closing", ping[i]);
            httpd_sess_trigger_close(self->server_, ping[i]);
        }
    }
}

// The send of every admitted client's session (httpd_sess_set_send_override): the whole buffer
// within SEND_TIMEOUT_MS, or failure. httpd's own send makes one send() call and takes a short
// count for success.
int WsService::send_whole(httpd_handle_t hd, int sockfd, const char* buf, size_t len, int flags) {
    (void)hd;
    if (!buf) return HTTPD_SOCK_ERR_INVALID;
    const int64_t deadline_us = esp_timer_get_time() + int64_t(SEND_TIMEOUT_MS) * 1000;
    size_t sent = 0;
    while (sent < len) {
        const int64_t left_us = deadline_us - esp_timer_get_time();
        // Under a millisecond lwIP rounds the timeout to 0, which means none at all.
        if (left_us < 1000) return HTTPD_SOCK_ERR_TIMEOUT;
        struct timeval tv = {};
        tv.tv_sec  = static_cast<time_t>(left_us / 1000000);
        tv.tv_usec = static_cast<suseconds_t>(left_us % 1000000);
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        const int n = send(sockfd, buf + sent, len - sent, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? HTTPD_SOCK_ERR_TIMEOUT
                                                             : HTTPD_SOCK_ERR_FAIL;
        }
        sent += static_cast<size_t>(n);
    }
    return static_cast<int>(sent);
}

// ── WS handler (static) ────────────────────────────────────────

esp_err_t WsService::on_handshake(httpd_req_t* req) {
    auto* self = static_cast<WsService*>(req->user_ctx);
    const int fd = httpd_req_to_sockfd(req);
    if (!self->clients_mutex_) return ESP_OK;
    const uint32_t now = now_ms();

    xSemaphoreTake(self->clients_mutex_, portMAX_DELAY);
    const net::WsAdmission adm =
        net::ws_admit(self->slots_, MAX_WS_CLIENTS, fd, now, STALE_AFTER_MS);
    net::WsSlot evicted;
    if (adm.action == net::WsAdmit::Evict) evicted = self->slots_[adm.slot];
    if (adm.slot >= 0) self->slots_[adm.slot] = net::WsSlot{fd, now, false};
    xSemaphoreGive(self->clients_mutex_);

    if (adm.action == net::WsAdmit::Reject) {
        // Every slot holds a client heard within the threshold. Say so: left connected, this
        // one would be sent nothing at all and could not tell busy from broken.
        ESP_LOGW(TAG, "%d live clients, refusing fd=%d (close %u)",
                 (int)MAX_WS_CLIENTS, fd, (unsigned)CLOSE_BUSY);
        uint8_t body[] = {CLOSE_BUSY >> 8, CLOSE_BUSY & 0xff, 'b', 'u', 's', 'y'};
        httpd_ws_frame_t frame = {};
        frame.type    = HTTPD_WS_TYPE_CLOSE;
        frame.payload = body;
        frame.len     = sizeof(body);
        httpd_ws_send_frame(req, &frame);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_OK;
    }
    if (adm.action == net::WsAdmit::Evict) {
        if (evicted.lost) {
            ESP_LOGW(TAG, "Slot %d: fd=%d missed a message, given to fd=%d",
                     adm.slot, evicted.fd, fd);
        } else {
            ESP_LOGW(TAG, "Slot %d: fd=%d silent for %lu ms, given to fd=%d", adm.slot,
                     evicted.fd, (unsigned long)net::ws_silent_ms(evicted, now), fd);
        }
        httpd_sess_trigger_close(req->handle, evicted.fd);
    }
    ESP_LOGI(TAG, "Client connected (fd=%d, slot=%d)", fd, adm.slot);
    httpd_sess_set_send_override(req->handle, fd, send_whole);

    // Without the context (no memory) the heartbeat still notices the session going: httpd
    // stops reporting the fd as WebSocket.
    auto* sess = static_cast<SessionCtx*>(malloc(sizeof(SessionCtx)));
    if (sess) {
        sess->self = self;
        sess->fd   = fd;
        httpd_sess_set_ctx(req->handle, fd, sess, on_session_close);
    }

    // Відправити повний state новому клієнту одразу при підключенні.
    // Delta broadcasts не містять ключів, що не змінились з моменту
    // останнього broadcast — новий клієнт пропустив би весь стан.
    self->send_full_state_to(fd);

    return ESP_OK;
}

esp_err_t WsService::ws_handler(httpd_req_t* req) {
    auto* self = static_cast<WsService*>(req->user_ctx);

    // The handshake, on an IDF that still routes it here (before v5.5.5 / v6.0.1).
    if (req->method == HTTP_GET) return on_handshake(req);

    // Any frame at all - a PONG, a PING, data or a CLOSE - is the client alive.
    self->heard(httpd_req_to_sockfd(req));

    // Receive frame — leave type as 0 (HTTPD_WS_TYPE_CONTINUE) so httpd
    // fills in the actual opcode from the wire.
    httpd_ws_frame_t ws_pkt = {};
    // ws_pkt.type is already 0 == HTTPD_WS_TYPE_CONTINUE after zero-init

    // First call with len=0 to get frame info (type + length)
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGW(TAG, "ws_recv_frame failed fd=%d: %s", fd, esp_err_to_name(ret));
        // Remove client immediately to stop further sends to this fd
        self->remove_client(fd);
        return ret;
    }

    // ── Handle control frames (we set handle_ws_control_frames=true) ──
    // We must consume the full frame for every type, otherwise leftover
    // bytes in the socket buffer corrupt the next frame parse.

    // A frame longer than the buffer that would take it cannot be consumed, and its leftover
    // bytes would be read as the next frame: the session is closed instead. Browsers send none
    // of these; RFC 6455 caps a control frame's payload at 125 bytes.
    constexpr size_t FRAME_BUF = 128;
    if (ws_pkt.len >= FRAME_BUF) {
        ESP_LOGW(TAG, "Frame type=%d of %d bytes from fd=%d — closing", (int)ws_pkt.type,
                 (int)ws_pkt.len, httpd_req_to_sockfd(req));
        self->remove_client(httpd_req_to_sockfd(req));
        return ESP_FAIL;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        // PONG is the response to our server-initiated PING.
        // Consume any payload (usually empty) and discard.
        if (ws_pkt.len > 0) {
            uint8_t discard[FRAME_BUF];
            ws_pkt.payload = discard;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret != ESP_OK) return ret;
        }
        ESP_LOGD(TAG, "PONG from fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        // Read PING payload (if any) and reply with PONG
        uint8_t ping_buf[FRAME_BUF] = {};
        if (ws_pkt.len > 0) {
            ws_pkt.payload = ping_buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            if (ret != ESP_OK) return ret;
        }
        ws_pkt.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &ws_pkt);
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "CLOSE from fd=%d", fd);
        // Consume the close payload (status code + reason)
        uint8_t close_buf[FRAME_BUF] = {};
        if (ws_pkt.len > 0) {
            ws_pkt.payload = close_buf;
            httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        }
        // Reply with CLOSE - to a client we serve. One refused was sent our CLOSE already, and
        // this is its answer.
        if (self->remove_client(fd)) {
            ws_pkt.len = 0;
            ws_pkt.payload = nullptr;
            ws_pkt.type = HTTPD_WS_TYPE_CLOSE;
            httpd_ws_send_frame(req, &ws_pkt);
        }
        return ESP_OK;
    }

    // ── Handle data frames (TEXT / BINARY / CONTINUE) ──

    if (ws_pkt.len > 0 && ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        char payload[FRAME_BUF];
        ws_pkt.payload = reinterpret_cast<uint8_t*>(payload);
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) return ret;
        payload[ws_pkt.len] = '\0';

        ESP_LOGD(TAG, "WS recv: %s", payload);
        // Future: parse commands from client
    } else if (ws_pkt.len > 0) {
        // Non-TEXT data frame — consume and discard to keep stream aligned
        uint8_t discard[FRAME_BUF];
        ws_pkt.payload = discard;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) return ret;
        ESP_LOGD(TAG, "Discarded non-TEXT frame type=%d len=%d", (int)ws_pkt.type, (int)ws_pkt.len);
    }

    return ESP_OK;
}

// ── Broadcast ───────────────────────────────────────────────────

void WsService::broadcast_state() {
    if (!server_ || !state_) return;

    // Delta broadcast: серіалізуємо тільки змінені ключі
    // for_each_changed_and_clear() атомарно ітерує + очищує changed list
    //
    // The main task's buffer: broadcast_state() has exactly one caller, on_update() below.
    char* buf = net::state_json_buffer_main();
    if (!buf) return;  // the changes stay marked; the next tick tries again
    net::StateJson ctx = net::state_json_begin(buf, net::STATE_JSON_SIZE);

    bool had_changes = state_->for_each_changed_and_clear(net::state_json_entry, &ctx);

    if (!had_changes) return;  // Нічого не змінилось — skip

    if (!net::state_json_end(ctx)) {
        // The change marks are cleared already: every client now misses these keys, and only a
        // reconnect with its fresh snapshot puts that right.
        ESP_LOGE(TAG, "Delta state exceeds serialization buffer");
        mark_all_lost();
        return;
    }

    ESP_LOGD(TAG, "Delta broadcast: %d bytes", (int)ctx.pos);
    send_to_all(buf, ctx.pos);
}

void WsService::send_full_state_to(int fd) {
    if (!server_ || !state_) return;

    // Out-of-memory guard, on internal RAM: the kind the send through lwIP and the Wi-Fi path
    // allocates from, and the kind that runs out. With CONFIG_SPIRAM_USE_MALLOC=y the whole heap
    // also counts every free byte of PSRAM, tens of MB, and a check on it could never refuse.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 32000) {
        ESP_LOGW(TAG, "Internal heap < 32KB, skip initial state to fd=%d", fd);
        mark_lost(fd);
        return;
    }

    // The httpd task's buffer, shared with GET /api/state: this runs in the httpd task too
    // (on_handshake), one request at a time.
    char* buf = net::state_json_buffer_httpd();
    if (!buf) {
        ESP_LOGW(TAG, "No memory for serialize buf to fd=%d", fd);
        mark_lost(fd);
        return;
    }
    net::StateJson ctx = net::state_json_begin(buf, net::STATE_JSON_SIZE);
    state_->for_each(net::state_json_entry, &ctx);
    if (!net::state_json_end(ctx)) {
        ESP_LOGE(TAG, "Full state exceeds serialization buffer");
        mark_lost(fd);
        return;
    }

    // Queued, not sent from here: a delta queued before it must reach the client first. The
    // copy goes to PSRAM where it can - lwIP copies it out - and keeps ~5 KB off internal RAM.
    auto* send_ctx = static_cast<AsyncSendCtx*>(heap_caps_malloc_prefer(
        sizeof(AsyncSendCtx) + ctx.pos, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!send_ctx) {
        ESP_LOGW(TAG, "No memory for initial state to fd=%d", fd);
        mark_lost(fd);
        return;
    }
    send_ctx->server = server_;
    send_ctx->self   = this;
    send_ctx->fd     = fd;
    send_ctx->type   = HTTPD_WS_TYPE_TEXT;
    send_ctx->len    = ctx.pos;
    memcpy(send_ctx->data, buf, ctx.pos);

    if (httpd_queue_work(server_, async_send_cb, send_ctx) != ESP_OK) {
        ESP_LOGW(TAG, "Queue full for initial state fd=%d", fd);
        free(send_ctx);
        mark_lost(fd);
    } else {
        ESP_LOGI(TAG, "Full state sent to new client fd=%d (%d bytes)", fd, (int)ctx.pos);
    }
}

// ── Thread-safe send via httpd_queue_work ────────────────────

// Executed in the httpd thread — safe to call send() on the socket.
void WsService::async_send_cb(void* arg) {
    auto* ctx = static_cast<AsyncSendCtx*>(arg);

    // Fast path: if a previous callback already removed this fd from our
    // client list, skip the send entirely — avoids hitting the socket and
    // producing httpd_txrx warnings for a known-dead connection.
    if (ctx->self && !ctx->self->has_client(ctx->fd)) {
        ESP_LOGD(TAG, "fd=%d already removed before send, skipping", ctx->fd);
        free(ctx);
        return;
    }

    httpd_ws_client_info_t info = httpd_ws_get_fd_info(ctx->server, ctx->fd);
    if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
        ESP_LOGD(TAG, "fd=%d no longer WS at send time (info=%d), skipping",
                 ctx->fd, (int)info);
        if (ctx->self) ctx->self->remove_client(ctx->fd);
        free(ctx);
        return;
    }

    httpd_ws_frame_t frame = {};
    frame.type    = ctx->type;
    frame.payload = (ctx->len > 0)
                        ? reinterpret_cast<uint8_t*>(ctx->data)
                        : nullptr;
    frame.len     = ctx->len;

    esp_err_t ret = httpd_ws_send_frame_async(ctx->server, ctx->fd, &frame);
    if (ret != ESP_OK) {
        // Only act if this fd was still tracked — avoids duplicate
        // trigger_close calls when multiple queued sends fail in sequence
        bool was_tracked = ctx->self ? ctx->self->remove_client(ctx->fd) : false;
        if (was_tracked) {
            ESP_LOGW(TAG, "Send failed fd=%d: %s — closing", ctx->fd, esp_err_to_name(ret));
            httpd_sess_trigger_close(ctx->server, ctx->fd);
        } else {
            ESP_LOGD(TAG, "Send failed fd=%d (already removed), ignoring", ctx->fd);
        }
    }

    free(ctx);
}

void WsService::send_to_all(const char* data, size_t len) {
    // BUG-021: snapshot fds під mutex, queue work поза mutex. Whether a session is still
    // WebSocket is asked in the httpd task, by async_send_cb(): httpd's session table is not
    // locked against this one.
    int fds[MAX_WS_CLIENTS];
    if (clients_mutex_) {
        xSemaphoreTake(clients_mutex_, portMAX_DELAY);
        for (size_t i = 0; i < MAX_WS_CLIENTS; i++) fds[i] = slots_[i].fd;
        xSemaphoreGive(clients_mutex_);
    } else {
        return;
    }

    // A delta not delivered leaves the client out of step: mark_lost() has it reconnect.
    for (size_t i = 0; i < MAX_WS_CLIENTS; i++) {
        if (fds[i] == -1) continue;

        // Out-of-memory guard, on internal RAM as in send_full_state_to(): a delta of ~200 B
        // and its AsyncSendCtx, small enough for malloc() to put there
        // (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 8000) {
            ESP_LOGW(TAG, "Internal heap < 8KB, skip WS send fd=%d", fds[i]);
            mark_lost(fds[i]);
            continue;
        }

        auto* ctx = static_cast<AsyncSendCtx*>(
            malloc(sizeof(AsyncSendCtx) + len));
        if (!ctx) {
            ESP_LOGW(TAG, "No memory for async send to fd=%d", fds[i]);
            mark_lost(fds[i]);
            continue;
        }
        ctx->server = server_;
        ctx->self   = this;
        ctx->fd     = fds[i];
        ctx->type   = HTTPD_WS_TYPE_TEXT;
        ctx->len    = len;
        memcpy(ctx->data, data, len);

        if (httpd_queue_work(server_, async_send_cb, ctx) != ESP_OK) {
            ESP_LOGW(TAG, "Queue full for fd=%d", fds[i]);
            free(ctx);
            mark_lost(fds[i]);
        }
    }
}

// ── WS handler registration ────────────────────────────────────

void WsService::register_ws_handler() {
    if (!server_) {
        ESP_LOGE(TAG, "Cannot register WS handler: no HTTP server");
        return;
    }
    if (ws_registered_) {
        return;  // Already registered — skip silently
    }

    httpd_uri_t ws_uri = {};
    ws_uri.uri          = "/ws";
    ws_uri.method       = HTTP_GET;
    ws_uri.handler      = ws_handler;
    ws_uri.user_ctx     = this;
    ws_uri.is_websocket = true;
    // We MUST handle control frames ourselves.  When this is false (default),
    // ESP-IDF auto-handles PING (replies PONG) and CLOSE, but does NOT
    // consume PONG frames at all — their second-byte + mask-key + payload
    // remain in the socket buffer, causing the next read to misinterpret
    // leftover bytes as a new frame header ("WS frame is not properly masked").
    ws_uri.handle_ws_control_frames = true;
#ifdef CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    ws_uri.ws_post_handshake_cb = on_handshake;
#endif

    esp_err_t err = httpd_register_uri_handler(server_, &ws_uri);
    if (err == ESP_OK) {
        ws_registered_ = true;
        ESP_LOGI(TAG, "WebSocket handler registered at /ws");
    } else {
        ESP_LOGE(TAG, "Failed to register /ws handler: %s", esp_err_to_name(err));
        return;
    }

    // Install session close callback so we get notified when any
    // client disconnects (TCP RST, timeout, clean close).
    // NOTE: httpd_sess_set_close_fn is per-session; we install it in
    // ws_handler on each new WS connection instead.
}

void WsService::set_http_server(httpd_handle_t server) {
    if (!server) return;                 // Немає серверу — нічого робити
    if (server_ == server && ws_registered_) return;  // Вже налаштований
    server_ = server;
    register_ws_handler();
}

// ── Lifecycle ───────────────────────────────────────────────────

bool WsService::on_init() {
    // BUG-021: створити mutex для slots_
    clients_mutex_ = xSemaphoreCreateMutexStatic(&clients_mutex_buf_);

    ESP_LOGI(TAG, "WS service init (waiting for HTTP server handle)");
    // WS handler буде зареєстрований коли set_http_server() буде
    // викликаний з main.cpp після старту HttpService.
    // Якщо server_ вже встановлений І handler ще не зареєстрований — реєструємо.
    if (server_ && !ws_registered_) {
        register_ws_handler();
    }
    return true;
}

void WsService::on_update(uint32_t dt_ms) {
    if (!server_ || !state_) return;

    // Delta broadcast: as soon as something changed, once BROADCAST_MIN_GAP_MS has passed since
    // the last one. The timer stops counting at the gap, so a change after a quiet spell goes out
    // on the next tick.
    if (broadcast_timer_ < BROADCAST_MIN_GAP_MS) broadcast_timer_ += dt_ms;
    if (broadcast_timer_ >= BROADCAST_MIN_GAP_MS && state_->has_changes()) {
        broadcast_timer_ = 0;
        broadcast_state();
    }

    // Heartbeat: PINGs, and the close of clients gone silent, made in the httpd thread
    heartbeat_timer_ += dt_ms;
    if (heartbeat_timer_ >= HEARTBEAT_INTERVAL_MS) {
        heartbeat_timer_ = 0;
        // Not queued: this beat is skipped, nobody is dropped for it, the next is 5 s away.
        if (httpd_queue_work(server_, heartbeat_cb, this) != ESP_OK) {
            ESP_LOGW(TAG, "Heartbeat not queued");
        }
    }
}

} // namespace modesp
