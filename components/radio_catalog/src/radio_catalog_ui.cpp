/**
 * @file radio_catalog_ui.cpp
 * @brief The stores and the ask queue the catalog screen shares with the radio module. See
 *        radio_catalog_ui.h.
 */

#include "radio_catalog_ui.h"

#include <cstdlib>
#include <cstring>
#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace radio_catalog {
namespace {

/// One of each, made on first use. In PSRAM on the device for the reason catalog() gives:
/// internal .bss is what the SDIO co-processor's receive pool and the I2S DMA descriptors
/// compete for, and these are read by the LVGL and main tasks only, never with the cache off.
template <typename T>
T& single() {
#ifdef ESP_PLATFORM
    static T* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(T), 2,
                                            MALLOC_CAP_SPIRAM   | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!mem) abort();
        return new (mem) T();
    }();
    return *p;
#else
    static T v;
    return v;
#endif
}

}  // namespace

Published<Status>&    status()    { return single<Published<Status>>(); }
Published<Countries>& countries() { return single<Published<Countries>>(); }
Published<Genres>&    genres()    { return single<Published<Genres>>(); }
Asks&                 asks()      { return single<Asks>(); }

int order_countries(CountryCount* items, int n, const char* first) {
    // The service keys countries as typed, so "GR" and "gr" are both there; the parser has
    // upper-cased them, and here they become one country with both counts.
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (!items[i].stations || !country_name(items[i].code, 0)) continue;
        int same = -1;
        for (int k = 0; k < kept && same < 0; k++) if (!strcmp(items[k].code, items[i].code)) same = k;
        if (same >= 0) items[same].stations += items[i].stations;
        else           items[kept++] = items[i];
    }
    // Insertion sort: a couple of hundred entries, once a day.
    const auto before = [first](const CountryCount& a, const CountryCount& b) {
        const bool fa = first && !strcmp(a.code, first);
        const bool fb = first && !strcmp(b.code, first);
        if (fa != fb) return fa;
        if (a.stations != b.stations) return a.stations > b.stations;
        return strcmp(a.code, b.code) < 0;
    };
    for (int i = 1; i < kept; i++) {
        const CountryCount v = items[i];
        int j = i;
        while (j > 0 && before(v, items[j - 1])) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = v;
    }
    return kept;
}

bool Asks::post(const Ask& a) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= N) return false;
    item_[h % N] = a;
    head_.store(h + 1, std::memory_order_release);
    return true;
}

bool Asks::take(Ask& out) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    out = item_[t % N];
    tail_.store(t + 1, std::memory_order_release);
    return true;
}

}  // namespace radio_catalog
