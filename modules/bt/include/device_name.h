/**
 * @file device_name.h
 * @brief The one name the panel goes by: what phones list for Bluetooth, AirPlay and DLNA, and
 *        the drawer's title (the customer, 20.10.2026).
 *
 * «Ім'я» on the Bluetooth pages, the glass's and the web's, sets it: the satellite keeps the
 * Bluetooth half, and the panel keeps this copy in NVS for the services that start before the
 * satellite answers, or without one. The bt module keeps it as a rename goes to the satellite,
 * takes the satellite's name the first time there is none (the name somebody gave it there), and
 * gives this one back, once a link, to a satellite that comes up with another. AirPlay and DLNA
 * read it as they start, so a new name reaches them at the next start; Bluetooth has it at once.
 */

#pragma once

#include <cstddef>

namespace device_name {

/// 32 bytes, the satellite's limit (LINK_PROTOCOL.md, BT_CONFIG_SET), and the NUL.
constexpr size_t CAP = 33;
/// Until a name is kept: system.device_name in project.json, which never reaches SharedState.
constexpr const char* FALLBACK = "P4 Player";

/// Whether `s[0, len)` is a name the satellite takes: 1..32 bytes of well-formed UTF-8 — no
/// NUL, no stray or missing continuation byte, no overlong form, no surrogate, nothing past
/// U+10FFFF (its bt_name_ok; LINK_PROTOCOL.md, BT_CONFIG_SET: a name that is not UTF-8 is not
/// applied). The panel holds its own copy to the same rule: a name the satellite refuses can
/// never match the one it reports back.
inline bool valid(const char* s, size_t len) {
    if (!s || len == 0 || len >= CAP) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    for (size_t i = 0; i < len;) {
        const unsigned c = p[i];
        if (c == 0) return false;
        if (c < 0x80) { i++; continue; }
        // The lead byte gives the length; the range of the byte after it is what rules out the
        // overlong forms, the surrogates and everything past U+10FFFF (RFC 3629, section 4).
        size_t   n  = 0;
        unsigned lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF)      n = 2;
        else if (c >= 0xE0 && c <= 0xEF) { n = 3; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 4; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
        else return false;
        if (len - i < n || p[i + 1] < lo || p[i + 1] > hi) return false;
        for (size_t k = 2; k < n; k++)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

/// The name kept, or FALLBACK while there is none. A stored name valid() refuses counts as none.
void get(char* out, size_t n);
/// Whether a name valid() takes has been kept.
bool kept();
/// Keep `name` if valid() takes it; NVS is written only when it changes.
void keep(const char* name);

}  // namespace device_name
