/**
 * @file buffered_audio.c
 * @brief What a buffered (AirPlay 2) audio packet says it is, before it is decoded.
 *
 * Bytes 8-11 of each packet on the buffered stream are its SSRC, and an AirPlay 2 sender
 * uses them to name the packet's encoding. shairport-sync knows these (player.h, ssrc_t):
 * 0 none, 0x15000000 ALAC 48 kHz stereo, 0x16000000 AAC 44.1 kHz stereo, 0x17000000 AAC
 * 48 kHz stereo, 0x27000000 / 0x28000000 AAC 48 kHz 5.1 / 7.1 - and decodes a packet only
 * when it knows the SSRC and it is not 0 (ap2_buffered_audio_processor.c). Upstream's
 * buffered task decrypts and decodes every packet with the decoder SETUP made, whatever the
 * packet names.
 *
 * Seen on the panel: the AAC decoder refused one frame (esp_aac_dec error 20, then a
 * decoder reset) about 0.7 s before each TEARDOWN at the end of a YouTube item - every ad -
 * and never across Apple Music's gapless track changes, which have no TEARDOWN.
 *
 * Two functions are wrapped at link time (CMakeLists.txt), both called across objects:
 *
 *  - audio_crypto_decrypt_buffered() (audio_stream_buffered.c -> audio_crypto.c), the one
 *    call that sees the whole packet. A packet with SSRC 0 is refused there, before it is
 *    decrypted, as shairport-sync skips it; upstream counts it as a packet that did not
 *    decrypt and drops it before the decoder sees it. A packet whose SSRC is not in the
 *    table above, or that switches from one listed format to another, is decoded as before
 *    and said at WARN once per run: the table is shairport-sync's, not Apple's, and refusing
 *    what it does not list could silence a stream that plays today.
 *
 *  - audio_decoder_decode() (audio_stream.c -> audio_decoder.c). A frame it refuses is said
 *    at WARN with its length, the SSRC and sequence number of the last buffered packet, and
 *    its first bytes, so the next refusal names itself.
 *
 * Both run on the buffered task alone (audio_decoder_decode also on the realtime one, whose
 * packets carry no such SSRC), so the little state here is not shared.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "audio_crypto.h"
#include "audio_decoder.h"
#include "audio_stream.h"

static const char *TAG = "ap_buffered";

#define SSRC_NONE 0u

typedef struct {
  uint32_t ssrc;
  const char *name;
} ssrc_name_t;

static const ssrc_name_t SSRC_NAMES[] = {
    {0x15000000u, "ALAC 48 kHz stereo"},  {0x16000000u, "AAC 44.1 kHz stereo"},
    {0x17000000u, "AAC 48 kHz stereo"},   {0x27000000u, "AAC 48 kHz 5.1"},
    {0x28000000u, "AAC 48 kHz 7.1"},
};

static const char *ssrc_name(uint32_t ssrc) {
  for (size_t i = 0; i < sizeof(SSRC_NAMES) / sizeof(SSRC_NAMES[0]); i++) {
    if (SSRC_NAMES[i].ssrc == ssrc) {
      return SSRC_NAMES[i].name;
    }
  }
  return NULL;
}

// The last buffered packet seen (no SSRC is all ones), and the last one in a listed format.
static uint32_t s_last_ssrc = 0xFFFFFFFFu;
static uint32_t s_last_known;
static uint32_t s_last_seq;

int __real_audio_crypto_decrypt_buffered(const audio_encrypt_t *encrypt,
                                         const uint8_t *packet, size_t packet_len,
                                         uint8_t *output, size_t output_capacity);

int __wrap_audio_crypto_decrypt_buffered(const audio_encrypt_t *encrypt,
                                         const uint8_t *packet, size_t packet_len,
                                         uint8_t *output, size_t output_capacity) {
  if (!packet || packet_len < 12) {
    return __real_audio_crypto_decrypt_buffered(encrypt, packet, packet_len, output,
                                                output_capacity);
  }
  const uint32_t seq = ((uint32_t)packet[1] << 16) | ((uint32_t)packet[2] << 8) | packet[3];
  const uint32_t ssrc = ((uint32_t)packet[8] << 24) | ((uint32_t)packet[9] << 16) |
                        ((uint32_t)packet[10] << 8) | packet[11];
  const bool new_run = ssrc != s_last_ssrc;
  s_last_ssrc = ssrc;
  s_last_seq = seq;

  // The only caller passes &stream->encrypt (audio_stream_buffered.c), so the stream - and
  // the format SETUP agreed - is the one around it.
  const audio_stream_t *stream =
      encrypt ? (const audio_stream_t *)((const char *)encrypt -
                                         offsetof(audio_stream_t, encrypt))
              : NULL;
  const char *codec = stream ? stream->format.codec : "?";
  const int rate = stream ? stream->format.sample_rate : 0;

  if (ssrc == SSRC_NONE) {
    if (new_run) {
      ESP_LOGW(TAG, "packet %u: SSRC 0, no audio - not decoded", (unsigned)seq);
    }
    return -1;
  }

  const char *name = ssrc_name(ssrc);
  if (!name) {
    if (new_run) {
      ESP_LOGW(TAG, "packet %u: SSRC 0x%08x is not one known - decoded as SETUP's %s %d Hz",
               (unsigned)seq, (unsigned)ssrc, codec, rate);
    }
  } else {
    if (s_last_known && ssrc != s_last_known) {
      ESP_LOGW(TAG, "packet %u: %s after %s - decoded as SETUP's %s %d Hz", (unsigned)seq,
               name, ssrc_name(s_last_known), codec, rate);
    }
    s_last_known = ssrc;
  }
  return __real_audio_crypto_decrypt_buffered(encrypt, packet, packet_len, output,
                                              output_capacity);
}

int __real_audio_decoder_decode(audio_decoder_t *decoder, const uint8_t *input,
                                size_t input_len, int16_t *output,
                                size_t output_capacity_samples, audio_decode_info_t *info);

int __wrap_audio_decoder_decode(audio_decoder_t *decoder, const uint8_t *input,
                                size_t input_len, int16_t *output,
                                size_t output_capacity_samples, audio_decode_info_t *info) {
  const int samples = __real_audio_decoder_decode(decoder, input, input_len, output,
                                                  output_capacity_samples, info);
  if (samples < 0) {
    char head[3 * 8 + 1] = "";
    const size_t n = input && input_len < 8 ? input_len : (input ? 8 : 0);
    for (size_t i = 0; i < n; i++) {
      snprintf(head + 3 * i, sizeof(head) - 3 * i, "%02x ", input[i]);
    }
    ESP_LOGW(TAG, "frame of %u bytes refused (last packet %u, SSRC 0x%08x): %s",
             (unsigned)input_len, (unsigned)s_last_seq, (unsigned)s_last_ssrc, head);
  }
  return samples;
}
