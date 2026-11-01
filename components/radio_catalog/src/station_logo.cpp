/**
 * @file station_logo.cpp
 * @brief The presets' logos: names, files, the plan, the shelf and when the files may be
 *        written. See station_logo.h.
 */

#include "station_logo.h"

#include <cstdio>
#include <cstring>

namespace station_logo {
namespace {

constexpr char MAGIC[4] = {'S', 'L', 'G', '1'};
constexpr int  UUID_CHARS = 36;

bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

}  // namespace

bool valid_uuid(const char* s) {
    if (!s) return false;
    for (int i = 0; i < UUID_CHARS; i++) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !hex(s[i])) return false;   // the terminator fails both
    }
    return s[UUID_CHARS] == '\0';
}

bool file_name(const char* uuid, char* out, size_t out_sz) {
    if (!out || !valid_uuid(uuid) || UUID_CHARS + strlen(EXT) >= out_sz) return false;
    snprintf(out, out_sz, "%s%s", uuid, EXT);
    return true;
}

bool uuid_of(const char* name, char* uuid, size_t uuid_sz) {
    if (!name || !uuid || uuid_sz < (size_t)UUID_LEN) return false;
    if (strlen(name) != UUID_CHARS + strlen(EXT) || strcmp(name + UUID_CHARS, EXT) != 0) return false;
    char u[UUID_LEN];
    memcpy(u, name, UUID_CHARS);
    u[UUID_CHARS] = '\0';
    if (!valid_uuid(u)) return false;
    memcpy(uuid, u, UUID_LEN);
    return true;
}

size_t encode(const uint16_t* px, uint8_t* out, size_t out_sz) {
    if (!px || !out || out_sz < FILE_BYTES) return 0;
    memcpy(out, MAGIC, sizeof(MAGIC));
    out[4] = (uint8_t)SIDE; out[5] = (uint8_t)(SIDE >> 8);
    out[6] = (uint8_t)SIDE; out[7] = (uint8_t)(SIDE >> 8);
    uint8_t* p = out + HEADER;
    for (int i = 0; i < PIXELS; i++) {
        *p++ = (uint8_t)px[i];
        *p++ = (uint8_t)(px[i] >> 8);
    }
    return FILE_BYTES;
}

bool decode(const uint8_t* file, size_t len, uint16_t* px) {
    if (!file || !px || len != FILE_BYTES || memcmp(file, MAGIC, sizeof(MAGIC)) != 0) return false;
    const int w = file[4] | (file[5] << 8), h = file[6] | (file[7] << 8);
    if (w != SIDE || h != SIDE) return false;
    const uint8_t* p = file + HEADER;
    for (int i = 0; i < PIXELS; i++, p += 2) px[i] = (uint16_t)(p[0] | (p[1] << 8));
    return true;
}

// ── the plan ──

bool Plan::want(const char* const* uuids, int n) {
    Entry next[WANT];
    int m = 0;
    for (int i = 0; i < n && m < WANT; i++) {
        const char* u = uuids ? uuids[i] : nullptr;
        if (!valid_uuid(u)) continue;
        bool seen = false;
        for (int k = 0; k < m && !seen; k++) seen = !strcmp(next[k].uuid, u);
        if (seen) continue;
        const int was = find(u);
        if (was >= 0) {
            next[m] = e_[was];
        } else {
            next[m] = Entry{};
            memcpy(next[m].uuid, u, UUID_LEN);
        }
        m++;
    }
    // The same set in another order is the same set: no file is to go and none to come.
    bool changed = m != n_;
    for (int i = 0; i < m && !changed; i++) changed = find(next[i].uuid) < 0;
    for (int i = 0; i < m; i++) e_[i] = next[i];
    n_ = m;
    return changed;
}

int Plan::find(const char* uuid) const {
    if (!uuid) return -1;
    for (int i = 0; i < n_; i++) if (!strcmp(e_[i].uuid, uuid)) return i;
    return -1;
}

void Plan::wait(int i, int64_t now_us, int64_t delay_us) {
    if (i < 0 || i >= n_) return;
    e_[i].state = State::WAIT;
    e_[i].retry_us = now_us + delay_us;
}

int Plan::due(int64_t now_us, int* out, int max) const {
    int k = 0;
    for (int i = 0; i < n_ && k < max; i++) {
        const Entry& e = e_[i];
        if (e.state == State::NEED || (e.state == State::WAIT && now_us >= e.retry_us)) out[k++] = i;
    }
    return k;
}

void Plan::kept(int i) {
    if (i < 0 || i >= n_) return;
    e_[i].state = State::HAVE;
    e_[i].unsaved = true;
}

int Plan::to_save() const {
    for (int i = 0; i < n_; i++) if (e_[i].unsaved) return i;
    return -1;
}

void Plan::saved(int i) {
    if (i >= 0 && i < n_) e_[i].unsaved = false;
}

bool Quiet::tick(uint32_t dt_ms, bool silent) {
    if (!silent) ms_ = 0;
    else ms_ = dt_ms >= QUIET_MS - ms_ ? QUIET_MS : ms_ + dt_ms;   // held at QUIET_MS: no wrap
    return ms_ >= QUIET_MS;
}

// ── the shelf ──

const Thumb* Shelf::find(const char* uuid) const {
    if (!uuid || !uuid[0]) return nullptr;
    for (int i = 0; i < count && i < WANT; i++) if (!strcmp(item[i].uuid, uuid)) return &item[i];
    return nullptr;
}

bool Shelf::put(const char* uuid, const uint16_t* px) {
    if (!valid_uuid(uuid) || !px) return false;
    int at = -1;
    for (int i = 0; i < count && at < 0; i++) if (!strcmp(item[i].uuid, uuid)) at = i;
    if (at < 0) {
        if (count >= WANT) return false;
        at = count++;
        memcpy(item[at].uuid, uuid, UUID_LEN);
    }
    memcpy(item[at].px, px, sizeof(item[at].px));
    return true;
}

void Shelf::keep(const Plan& plan) {
    int k = 0;
    for (int i = 0; i < count; i++) {
        if (plan.find(item[i].uuid) < 0) continue;
        if (k != i) item[k] = item[i];
        k++;
    }
    // Not `= Thumb{}`: that is a 3 KB temporary, and this runs on the main task's 8 KB stack.
    for (int i = k; i < count; i++) {
        memset(item[i].uuid, 0, sizeof(item[i].uuid));
        memset(item[i].px, 0, sizeof(item[i].px));
    }
    count = k;
}

}  // namespace station_logo
