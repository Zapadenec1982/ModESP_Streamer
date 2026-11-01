#pragma once
/**
 * @file bt_web.h
 * @brief The satellite's Bluetooth settings as the web reads and changes them: GET and POST
 *        /api/bt.
 *
 * Pure — no HTTP, no lock — so what the web is sent and what it may send are settled by the
 * host tests (tests/host/test_bt_web.cpp); bt_http.inc only moves bytes. The web asks for the
 * same things the panel's Bluetooth page does, through the same queue (bt_settings.h).
 *
 * GET:  {"linked","settings","known","radio","discoverable","auto_reconnect",
 *        "codecs_supported","codecs_enabled","name","total",
 *        "bonds":[{"addr":"AA:BB:CC:DD:EE:FF","name","connected","last"}],
 *        "pairing":{"seq","active","kind","name","code","result","by","accepted"},
 *        "confirmer"}
 * POST, any of: {"radio":b,"discoverable":b,"auto_reconnect":b,"codecs":n}  — the switches
 *               {"name":"…"}                       — 1..32 bytes of UTF-8
 *               {"forget":"AA:BB:CC:DD:EE:FF"} or {"forget":"all"}
 *               {"disconnect":true}  {"refresh":true}
 *               {"pair":"accept"|"reject","seq":N}  — the answer to pairing request N
 *
 * The pairing code is in GET only, behind the login: the WebSocket that tells every tab a
 * phone is asking is open to the whole network (decision 4), and the code is what proves the
 * phone in the room is the one pairing.
 */

#include "bt_settings.h"

#include <cstddef>
#include <cstdint>

namespace bt_web {

/// The address as the panel prints it (screen_bt.cpp): "AA:BB:CC:DD:EE:FF".
void addr_text(const uint8_t a[6], char* out, size_t n);
/// The other way; false for anything but six hex pairs joined by ':'.
bool parse_addr(const char* s, size_t n, uint8_t out[6]);

/// GET's body. Returns its length, or 0 when it does not fit.
size_t json(const bt_settings::View& v, const bt_settings::Pairing& p, bool confirmer,
            char* buf, size_t buf_sz);

struct Edit {
    // the switches: -1 = not given, keep as it is
    int8_t  radio = -1, discoverable = -1, auto_reconnect = -1;
    int16_t codecs = -1;
    bool    rename = false;
    char    name[bt_settings::NAME_CAP] = {};
    bool    forget = false, forget_all = false;
    uint8_t forget_addr[6] = {};
    bool    disconnect = false, refresh = false;
    bool    pair = false, accept = false;
    uint32_t seq = 0;

    bool any_switch() const { return radio >= 0 || discoverable >= 0 || auto_reconnect >= 0 || codecs >= 0; }
    /// Everything but a disconnect needs a satellite that speaks protocol 1.8.
    bool needs_settings() const { return any_switch() || rename || forget || refresh || pair; }
};

/// POST's body. False for anything malformed: a value of the wrong type, a name that is empty,
/// longer than 32 bytes, not UTF-8 (device_name::valid) or has a control character, an address
/// that is not one, a pairing answer without its request number, or nothing at all asked.
///
/// Its token array is static: one caller, the HTTP server's task.
bool parse(const char* json, size_t len, Edit& out);

}  // namespace bt_web
