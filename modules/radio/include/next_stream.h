#pragma once

#include "esp_audio_simple_dec.h"
#include "esp_http_client.h"
#include <cstddef>
#include <cstdint>

namespace radio_core::next_stream {
// One prepared stream. The worker owns it until take(); afterwards the radio
// decoder/reader own it, under their existing I/O lifetime mutex.
constexpr size_t PREFIX_BYTES = 128 * 1024 - 1;
constexpr size_t PCM_BYTES = 32 * 1024;
struct Prepared {
    uint32_t owner = 0;
    char uri[257]{};
    esp_http_client_handle_t http = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    /// The decoder the sniff chose, which the URL need not name: a DLNA item often has no
    /// extension at all.
    esp_audio_simple_dec_type_t type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    esp_audio_simple_dec_info_t info{};
    uint8_t *prefix = nullptr, *pcm = nullptr;
    size_t size = 0, consumed = 0, pcm_size = 0;
    uint64_t total = 0;
    bool complete = false;
};

bool init();
// Idempotent; an empty URI invalidates any preparation. Never blocks on I/O.
void request(uint32_t owner, const char *uri);
// Transfer only when the next track has the same source rate. Different rates
// use the ordinary drained transition so queued PCM never changes rate midway.
Prepared *take(uint32_t owner, uint32_t source_rate);
int read(Prepared *stream, uint8_t *out, size_t size);
void destroy(Prepared *stream);
} // namespace radio_core::next_stream
