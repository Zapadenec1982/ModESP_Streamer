/*
 * httpd_uri_compat.c — the prebuilt UPnP library and this app do not agree on the size of
 * httpd_uri_t, and the disagreement is fatal.
 *
 * WHAT HAPPENS
 *
 * `httpd_uri_t` grows with Kconfig. With CONFIG_HTTPD_WS_SUPPORT it gains three more
 * members after user_ctx:
 *
 *     uri (0) method (4) handler (8) user_ctx (12) | is_websocket (16)
 *     handle_ws_control_frames (17) supported_subprotocol (20)
 *
 * espressif/esp_media_protocols 1.0.1 is shipped as a binary, built with that option OFF.
 * Its idea of the struct is sixteen bytes, and its disassembly says so exactly — the call in
 * esp_upnp_service_register_actions writes offsets 0, 4, 8 and 12 and nothing beyond:
 *
 *     sw a5,0(sp)              uri
 *     li a5,3 ; sw a5,4(sp)    method = 3, HTTP_POST
 *     sw a5,8(sp)              handler
 *     sw s2,12(sp)             user_ctx
 *     call httpd_register_uri_handler
 *
 * This app has CONFIG_HTTPD_WS_SUPPORT=y, because the web UI is pushed over a WebSocket.
 * So httpd reads `supported_subprotocol` at offset 20 — past the end of the object the
 * library passed — and hands whatever is next on that stack to strdup(). On the panel, what
 * was next was the 3 that had just been written as the method:
 *
 *     Guru Meditation Error: Core 0 panic'ed (Load access fault)
 *     MTVAL : 0x00000003
 *     httpd_register_uri_handler at httpd_uri.c:179  <- strdup(supported_subprotocol)
 *     esp_upnp_service_register_actions at upnp_service.c:852
 *
 * That is every boot with the renderer on, and it is not a fault in this firmware: any
 * application with a WebSocket and this component will hit it.
 *
 * WHAT THIS DOES
 *
 * Wraps httpd_register_uri_handler at link time and, only while the UPnP library is inside
 * esp_dlna_start(), rebuilds the request from the four members the library actually wrote
 * into a struct of the size this app uses, zeroed. Everything else in the firmware —
 * including the WebSocket registration, which genuinely needs those members — goes through
 * untouched, because the guard is off.
 *
 * The rest of the ABI was checked rather than assumed: httpd_req_t has no Kconfig-dependent
 * members except `uri[CONFIG_HTTPD_MAX_URI_LEN + 1]`, and the library reads `user_ctx` at
 * offset 532, which is exactly where 512 puts it here. Nothing else crosses this boundary
 * by value.
 *
 * Remove this file when the component ships a build that agrees with us, and check the
 * disassembly rather than the changelog before believing it does.
 */

#include "esp_dlna_compat.h"

#include <stdbool.h>
#include <string.h>

#include "esp_http_server.h"

/* Set around the library's own registration and nowhere else. A plain flag rather than
   anything cleverer: the library registers from the one task that sets it, inside one
   call, and a lock here would only make that harder to read. */
static volatile bool s_sanitise = false;
static esp_err_t (*s_avt_handler)(httpd_req_t*) = NULL;
void esp_dlna_avt_handler(esp_err_t (*handler)(httpd_req_t*)) { s_avt_handler = handler; }

void esp_dlna_httpd_compat(bool on)
{
    s_sanitise = on;
}

esp_err_t __real_httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);

esp_err_t __wrap_httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    if (!s_sanitise || uri_handler == NULL) {
        return __real_httpd_register_uri_handler(handle, uri_handler);
    }

    /* Only the first sixteen bytes are real. Reading them through this type is fine; the
       members past user_ctx are what must not be read. */
    httpd_uri_t safe;
    memset(&safe, 0, sizeof(safe));
    safe.uri      = uri_handler->uri;
    safe.method   = uri_handler->method;
    safe.handler  = uri_handler->handler;
    safe.user_ctx = uri_handler->user_ctx;
    if (s_avt_handler && safe.method == HTTP_POST &&
        !strncmp(safe.uri, "/AVTransport/", sizeof("/AVTransport/") - 1))
        safe.handler = s_avt_handler;

    return __real_httpd_register_uri_handler(handle, &safe);
}
