/**
 * @file device_name.cpp
 * @brief The panel's copy of its one name, in NVS (device_name.h).
 */

#include "device_name.h"
#include "modesp/services/nvs_helper.h"
#include <cstdio>
#include <cstring>

namespace device_name {
namespace {
constexpr const char* NVS_NS  = "devname";
constexpr const char* NVS_KEY = "name";

/// The name in NVS, when it is one the satellite would take. keep() writes no other, but an
/// older firmware did: such a name reads as none, so AirPlay, DLNA and USB never announce it and
/// the satellite's own replaces it at the next BT_CONFIG.
bool stored(char* out, size_t n) {
    return modesp::nvs_helper::read_str(NVS_NS, NVS_KEY, out, n) && valid(out, strlen(out));
}
}  // namespace

void get(char* out, size_t n) {
    if (!out || !n) return;
    out[0] = '\0';
    if (!stored(out, n)) snprintf(out, n, "%s", FALLBACK);
}

bool kept() {
    char buf[CAP] = "";
    return stored(buf, sizeof buf);
}

void keep(const char* name) {
    if (!name || !valid(name, strlen(name))) return;
    char cur[CAP] = "";
    if (modesp::nvs_helper::read_str(NVS_NS, NVS_KEY, cur, sizeof cur) && strcmp(cur, name) == 0) return;
    modesp::nvs_helper::write_str(NVS_NS, NVS_KEY, name);
}

}  // namespace device_name
