/**
 * @file http_routes.h
 * @brief A window for late URI handlers, for code the framework may not name.
 *
 * The static file handler is the wildcard `/\*` with `httpd_uri_match_wildcard`, so it
 * matches every GET there is. `httpd_find_uri_handler` returns the FIRST match, which makes
 * registration order the whole contract: anything registered after the wildcard is dead
 * code that never sees a request. main.cpp therefore registers it last, on purpose.
 *
 * That works as long as everything with routes is something main.cpp knows about — WebSocket,
 * MQTT. A product module is not: modesp_net must not depend on a concrete module, the same
 * rule that produced modesp::log_source. So a module self-registers a callback here in
 * on_init(), and HttpService calls it inside the window, just before the wildcard goes down.
 *
 * Header-only for the same reason log_source.h is: with CONFIG_MODESP_NET_ENABLE=n the
 * component exports headers and no sources, and a module that registers routes must still
 * compile in an offline build — it simply never gets called.
 */

#pragma once

#include "esp_http_server.h"
#include <cstddef>

namespace modesp {
namespace http_routes {

/// Called with the running server, in the window before the `/\*` wildcard is registered.
using Registrar = void (*)(httpd_handle_t server, void* ctx);

/// Take the wildcard off the server, and put it back.
///
/// Registering routes after boot needs more than the right order — it needs the wildcard
/// GONE while it happens, and the reason is in httpd's own duplicate check. The server runs
/// with `httpd_uri_match_wildcard`, and `httpd_register_uri_handler` looks for an existing
/// handler using that same match function before it accepts a new one. `/\*` matches every
/// GET path there is, so with it registered, EVERY later GET registration is refused with
/// "already registered" — not shadowed, refused.
///
/// Measured on the panel: four of those warnings in a row, then a load access fault inside
/// the library that had just had its routes turned down. So a subsystem coming up late
/// takes the wildcard down first, registers, and puts it back.
using WildcardCtl = void (*)(void* self, bool on);

inline WildcardCtl& wildcard_fn() { static WildcardCtl f = nullptr; return f; }
inline void*&       wildcard_ctx() { static void* c = nullptr; return c; }

/// HttpService calls this once; nothing else should.
inline void set_wildcard_ctl(WildcardCtl fn, void* ctx) {
    wildcard_fn() = fn;
    wildcard_ctx() = ctx;
}

/// false takes the catch-all off the server, true puts it back on the end. Does nothing
/// when nothing set it — then there is no catch-all in the way either.
///
/// Between the two the server answers nothing: its task is held still, between two requests,
/// because httpd changes its handler table with no lock against its own lookups (see
/// HttpService::unregister_static_handler()). Keep the window to the registrations, and call it
/// from a task of your own — from a route handler, false would wait for itself.
inline void wildcard(bool on) { if (wildcard_fn()) wildcard_fn()(wildcard_ctx(), on); }

/// The server's login check, for routes that live outside HttpService. main.cpp sets it with
/// the wildcard control; a route calls authorized() first and returns when it is false — the
/// check has sent the 401 (or 429) itself by then.
///
/// Unset, it refuses: a route nobody can log in to is a smaller mistake than one anybody can
/// read.
using AuthCheck = bool (*)(httpd_req_t* req);

inline AuthCheck& auth_fn() { static AuthCheck f = nullptr; return f; }

/// main.cpp calls this once; nothing else should.
inline void set_auth_check(AuthCheck fn) { auth_fn() = fn; }

inline bool authorized(httpd_req_t* req) {
    if (auth_fn()) return auth_fn()(req);
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "No login check configured");
    return false;
}

/// The request's whole body, req->content_len bytes, into `buf`, NUL-terminated — room for both
/// is the caller's to check. The server's own policy for a body: a receive timeout is retried
/// while bytes keep coming, and three in a row (recv_wait_timeout each) end the request, since a
/// client that vanished without a FIN would otherwise hold the server's only task. A failure is
/// answered 408 for the timeouts or 400 for a broken connection, unless `reply` is false — for a
/// route with an error format of its own, such as a SOAP fault. False on failure either way.
inline bool read_body(httpd_req_t* req, char* buf, bool reply = true) {
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
            if (reply) {
                httpd_resp_send_err(req, r == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT
                                                                     : HTTPD_400_BAD_REQUEST,
                                    "Receive error");
            }
            return false;
        }
        timeouts = 0;
        got += (size_t)r;
    }
    buf[got] = '\0';
    return true;
}

/// A JSON answer: `status` as httpd takes it ("200 OK"), the body a NUL-terminated string. Never
/// cached — what a route answers is the state of the moment.
inline esp_err_t send_json(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/// Room for eight. Four would be a smell threshold — a fifth subsystem wanting routes would
/// mean the seam needs revisiting — but in the player DLNA, the station list, the
/// screenshot, the EQ, Bluetooth and the user's files each own their routes, which is the
/// seam doing its job, not a fault in it. Each slot is two pointers; add() still says no
/// when the table is full, and the caller logs it.
inline constexpr size_t MAX_REGISTRARS = 8;

struct Entry { Registrar fn; void* ctx; };

inline Entry* table() { static Entry t[MAX_REGISTRARS] = {}; return t; }
inline size_t& count() { static size_t n = 0; return n; }

/// Ask to be called before the wildcard. Returns false when the table is full — the caller
/// is expected to log that, because the silent half of this failure is "my routes 404".
inline bool add(Registrar fn, void* ctx = nullptr) {
    if (!fn || count() >= MAX_REGISTRARS) return false;
    table()[count()++] = Entry{fn, ctx};
    return true;
}

/// HttpService calls this; nothing else should.
inline void run_all(httpd_handle_t server) {
    if (!server) return;
    for (size_t i = 0; i < count(); ++i) table()[i].fn(server, table()[i].ctx);
}

} // namespace http_routes
} // namespace modesp
