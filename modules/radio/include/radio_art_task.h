/**
 * @file radio_art_task.h
 * @brief The radio's picture: the song's cover by the station's ICY title, else the
 *        station's logo; shown as CoverSource::Radio.
 *
 * The judgement is components/radio_catalog (radio_art.h); this is the fetching. The task
 * and its buffers are made on first use, so a panel that never plays the radio pays nothing.
 */

#pragma once

namespace radio_art_task {

/// Called by the radio module every half second while it owns the output. A new title asks
/// for its cover; an empty one, or covers turned off, falls back to the station's logo.
void update(const char* title, bool enabled);

/// A different station started. Its logo comes from `favicon` when the catalog gave one,
/// else from Radio Browser by `uuid`; neither, and there is no logo. `tls_free`: the
/// station streams over plain HTTP, so an HTTPS-only logo may be fetched.
void station(const char* uuid, const char* favicon, bool tls_free);

}  // namespace radio_art_task
