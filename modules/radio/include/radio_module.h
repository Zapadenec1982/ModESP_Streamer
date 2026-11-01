/**
 * @file radio_module.h
 * @brief Internet radio: an HTTP stream decoded into the same PCM ring the card plays through.
 */

#pragma once

#include "modesp/base_module.h"
#include <cstdint>

class RadioModule : public modesp::BaseModule {
public:
    RadioModule();
    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

private:
    /// The station catalog (src/catalog.cpp). Runs whatever the source is: browsing does not
    /// need the radio to be playing, and choosing a row is what makes it the source.
    void catalog_update(uint32_t dt_ms);

    /// Plays a station that is not in the list — a catalog row — as a station: it reconnects
    /// for ever and has a name, unlike a pushed URL. Takes the output.
    void play_catalog(const char* name, const char* url, const char* uuid, const char* favicon);
    /// Appends a station to the list and saves it. Its index, or −1 when the list is full.
    /// A station already there (by URL) is not added twice: its index comes back. `preset`
    /// 0..5 also puts it in that preset, in the same write.
    int  add_station(const char* name, const char* url, const char* uuid, int preset = -1);
    /// Moves the catalog station that is playing into the list, so it can be a preset — and
    /// with `preset` 0..5 makes it one; a station from the list only goes into the preset.
    int  keep_playing_station(int preset = -1);

    uint32_t click_gen_ = 0;   ///< the tune a play was last counted for (Radio Browser clicks)
    uint32_t logo_gen_  = 0;   ///< the tune the station's logo was last asked for
    uint32_t starve_gen_ = 0;  ///< the tune the slow-server watch was last reset for

    uint32_t report_ms_ = 0;
    uint32_t health_ms_ = 0;   ///< pacing for the stream-health line
    uint32_t logos_ms_  = 0;   ///< pacing for the presets' logos (src/logos.cpp)
    uint32_t up_wait_ms_ = 0;  ///< after a failed radio_up(), how long until the next try
    bool     idle_shown_ = false;  ///< radio.state says "idle" while another source plays
    bool     restored_  = false;   ///< the last station is tuned once, on the first pass
    int32_t  last_station_ = -1;   ///< what was written to the list file, to write on a change

    // Catalog request state, owned by the main task.
    static constexpr int CAT_PAGES = 64;     ///< pages a query can be walked forward
    char     cat_spec_[64] = "";             ///< the query as last seen in radio.cat_query
    int32_t  cat_sort_     = -1;
    int32_t  cat_quality_  = -1;
    int      cat_page_     = 0;
    uint32_t cat_offset_[CAT_PAGES] = {};    ///< server offset where each page starts
    uint32_t cat_serial_   = 0;              ///< last request posted
    uint32_t cat_seen_     = 0;              ///< last result published
    bool     cat_started_  = false;
};
