/**
 * @file radio_logos.h
 * @brief The presets' logos, kept on the panel: a preset shows its station's logo while an HTTPS
 *        station holds the TLS session, and after a reboot (docs/NETWORK.md, "Covers and logos").
 *
 * The rules are components/radio_catalog/include/station_logo.h; the Player draws what is kept
 * from radio_catalog::logo_shelf(). This is the module's side: the files, and the task that
 * fetches (src/logos.cpp).
 */

#pragma once

#include <cstdint>

namespace station_logo { struct Thumb; }

namespace radio_logos {

/// The radio module's call on the main task, every half second whatever the source;
/// `elapsed_ms` since the last. `may_fetch`: the panel's one TLS session is free and no stream
/// is starting (radio_module.cpp, logos_may_fetch). Reads and writes the files, publishes the
/// shelf, and starts the fetching task when a preset's logo is missing and may be had.
void update(uint32_t elapsed_ms, bool net_up, bool may_fetch);

/// The playing station's logo as a preset shows it, from the art task once it has fetched the
/// logo for the tile: kept, and written down if the station is a preset or becomes one — no
/// second download.
void offer(const station_logo::Thumb& logo);

}  // namespace radio_logos
