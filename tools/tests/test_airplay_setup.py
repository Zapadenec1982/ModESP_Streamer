"""Run the production SETUP handler against stubbed receiver, clocks, plist and sockets.

audio_receiver_set_format() frees and recreates the one decoder the stream task decodes
with, unlocked. An AirPlay 1 SETUP (a Transport header, no streams) calls it, so once a
SETUP has made the stream active, until TEARDOWN, another is refused before anything is
changed, as ANNOUNCE is (test_airplay_sdp.py).
An ekey is decrypted into a 32-byte key; libsodium writes all of its plaintext, so one that
would not fit is not handed to it. An AirPlay 2 stream's sr outside
AIRPLAY_RATE_MIN_HZ..AIRPLAY_RATE_MAX_HZ is refused before the receiver's format is set.
Direct run: python tools/tests/test_airplay_setup.py
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_airplay_mode import PORT, UP, compiler_command, function
    from .test_airplay_sdp import production_types
except ImportError:
    from test_airplay_mode import PORT, UP, compiler_command, function
    from test_airplay_sdp import production_types


def production_conn():
    conn = (UP / "rtsp/rtsp_conn.h").read_text(encoding="utf-8")
    start = conn.index("struct rtsp_conn {")
    return "typedef struct rtsp_conn rtsp_conn_t;\n" + conn[start:conn.index("\n};", start) + 3]


def production_stream_type():
    receiver = (UP / "audio/audio_receiver.h").read_text(encoding="utf-8")
    start = receiver.index("typedef enum {\n  AUDIO_STREAM_NONE")
    return receiver[start:receiver.index("} audio_stream_type_t;", start) + len("} audio_stream_type_t;")]


PREFIX = r'''
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plist.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef int esp_err_t;
#define ESP_OK 0
typedef struct { uint8_t shared_secret[32]; bool session_established; } hap_session_t;
typedef enum { RTSP_EVENT_CLIENT_CONNECTED, RTSP_EVENT_PLAYING } rtsp_event_t;
typedef struct { int unused; } rtsp_event_data_t;
#define AP2_AUDIO_BUFFER_SIZE (1 * 1024 * 1024)
#define AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES 11025
'''

STUBS = r'''
static int host_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        int d = tolower((unsigned char)a[i]) - tolower((unsigned char)b[i]);
        if (d || !a[i]) return d;
    }
    return 0;
}
#define strncasecmp host_strncasecmp
static void unreached(const char *what) {
    fprintf(stderr, "unexpected call: %s\n", what);
    abort();
}
static int formats, encryptions, ntp_starts, port_allocs, ptp_stops, stream_types;
static int transports, event_sockets, replies, status, decrypts, derives;
static size_t ekey_sent;   /* the ekey the sender's plist carries; 0 = none */
static bool streams;       /* an AirPlay 2 stream SETUP: one realtime stream... */
static int64_t stream_sr;  /* ...at this sr */
static audio_encrypt_t last_encrypt;
static audio_format_t last_format;
static void stream_setup_only(const char *what) { if (!streams) unreached(what); }
static void reset_counts(void) {
    formats = encryptions = ntp_starts = port_allocs = ptp_stops = stream_types = 0;
    transports = event_sockets = replies = status = decrypts = derives = 0;
}
bool bplist_get_streams_count(const uint8_t *p, size_t n, size_t *count) {
    (void)p; (void)n;
    if (!streams) return false;   /* an initial SETUP: no streams */
    *count = 1; return true;
}
bool bplist_get_stream_info(const uint8_t *p, size_t n, size_t i, int64_t *type,
                            size_t *ekey, size_t *eiv, size_t *shk) {
    (void)p; (void)n; stream_setup_only("bplist_get_stream_info");
    assert(i == 0); *type = AUDIO_STREAM_REALTIME; *ekey = *eiv = *shk = 0; return true;
}
bool bplist_get_stream_kv_info(const uint8_t *p, size_t n, size_t i, bplist_kv_info_t *out,
                               size_t cap, size_t *count) {
    (void)p; (void)n; stream_setup_only("bplist_get_stream_kv_info");
    static const char *const keys[] = {"ct", "sr", "spf"};
    const int64_t values[] = {2, stream_sr, 352};
    assert(i == 0 && cap >= 3);
    for (size_t k = 0; k < 3; ++k) {
        memset(&out[k], 0, sizeof(out[k]));
        strcpy(out[k].key, keys[k]);
        out[k].value_type = BPLIST_VALUE_INT; out[k].int_value = values[k];
    }
    *count = 3; return true;
}
bool bplist_find_stream_crypto(const uint8_t *p, size_t n, int64_t type, uint8_t *ekey,
                               size_t ekey_cap, size_t *ekey_len, uint8_t *eiv,
                               size_t eiv_cap, size_t *eiv_len, uint8_t *shk,
                               size_t shk_cap, size_t *shk_len) {
    (void)p; (void)n; (void)type; (void)eiv; (void)eiv_cap; (void)shk; (void)shk_cap;
    if (!ekey_sent) stream_setup_only("bplist_find_stream_crypto");
    /* As the parser does: cut to the caller's buffer (a 72-byte ekey arrives as 64). */
    *ekey_len = ekey_sent < ekey_cap ? ekey_sent : ekey_cap;
    memset(ekey, 0x77, *ekey_len);
    *eiv_len = *shk_len = 0;
    return *ekey_len > 0;
}
bool bplist_find_data_deep(const uint8_t *p, size_t n, const char *key, uint8_t *out,
                           size_t cap, size_t *len) {
    (void)p; (void)n; (void)key; (void)out; (void)cap; (void)len;
    stream_setup_only("bplist_find_data_deep"); return false;
}
size_t bplist_build_initial_setup(uint8_t *out, size_t cap, uint16_t port) {
    (void)port; assert(cap >= 8); memcpy(out, "bplist00", 8); return 8;
}
size_t bplist_build_stream_setup(uint8_t *out, size_t cap, int64_t type, uint16_t data,
                                 uint16_t control, uint32_t size) {
    (void)data; (void)control; (void)size; stream_setup_only("bplist_build_stream_setup");
    assert(type == AUDIO_STREAM_REALTIME && cap >= 8); memcpy(out, "bplist00", 8); return 8;
}
#define crypto_aead_chacha20poly1305_ietf_ABYTES 16U
static int crypto_aead_chacha20poly1305_ietf_decrypt(
        unsigned char *m, unsigned long long *mlen, unsigned char *nsec,
        const unsigned char *c, unsigned long long clen, const unsigned char *ad,
        unsigned long long adlen, const unsigned char *npub, const unsigned char *k) {
    (void)nsec; (void)c; (void)ad; (void)adlen; (void)npub; (void)k;
    /* libsodium 1.0.21 writes clen - 16 bytes into m, and zeroes as many when the tag
       fails: m is the handler's 32-byte key, so more than that is a stack overrun. */
    assert(clen > 16 && clen - 16 <= 32);
    memset(m, 0x5a, (size_t)(clen - 16)); *mlen = clen - 16; ++decrypts; return 0;
}
static esp_err_t hap_derive_audio_key(hap_session_t *s, uint8_t *key, size_t len) {
    assert(s->session_established); memset(key, 0x33, len); ++derives; return ESP_OK;
}
static void audio_receiver_set_stream_type(audio_stream_type_t type) {
    assert(type == AUDIO_STREAM_REALTIME); ++stream_types;
}
static bool rtsp_codec_configure(int64_t type, audio_format_t *fmt, int64_t sr, int64_t spf) {
    assert(type == 2 && spf == 352 && (streams || sr == 44100));
    strcpy(fmt->codec, "ALAC"); fmt->sample_rate = (int)sr; fmt->channels = 2;
    return true;
}
static void audio_receiver_set_format(const audio_format_t *format) {
    assert(!strcmp(format->codec, "ALAC")); last_format = *format; ++formats;
}
static void audio_receiver_set_encryption(const audio_encrypt_t *encrypt) {
    last_encrypt = *encrypt; ++encryptions;
}
static int rtsp_create_event_socket(uint16_t *port) { ++event_sockets; *port = 7010; return 9; }
static esp_err_t rtsp_start_event_port_task(int s) { assert(s == 9); return ESP_OK; }
static int host_close(int fd) { (void)fd; unreached("close"); return 0; }
#define close host_close
static int rtsp_send_response(int socket, rtsp_conn_t *conn, int code, const char *text,
                              int cseq, const char *headers, const char *body, size_t len) {
    (void)socket; (void)conn; (void)text; (void)headers; (void)body; (void)len;
    assert(cseq == 7); ++replies; status = code; return 0;
}
static int rtsp_send_ok(int socket, rtsp_conn_t *conn, int cseq) {
    return rtsp_send_response(socket, conn, 200, "OK", cseq, NULL, NULL, 0);
}
static void rtsp_parse_transport(const char *request, uint16_t *control, uint16_t *timing) {
    assert(strstr(request, "Transport:")); ++transports; *control = 6001; *timing = 6002;
}
static bool start_ntp_timing_or_fail(int socket, rtsp_conn_t *conn,
                                     const rtsp_request_t *req) {
    (void)socket; (void)conn; (void)req; ++ntp_starts; return true;
}
static void ensure_stream_ports(rtsp_conn_t *conn, bool buffered) {
    assert(!buffered); ++port_allocs;
    conn->data_port = 6100; conn->control_port = 6101; conn->timing_port = 6102;
}
static void ptp_clock_stop(void) { ++ptp_stops; }
static bool audio_stream_uses_buffer(audio_stream_type_t type) {
    stream_setup_only("audio_stream_uses_buffer"); return type == AUDIO_STREAM_BUFFERED;
}
static esp_err_t audio_receiver_start_buffered(uint16_t port) {
    (void)port; unreached("audio_receiver_start_buffered"); return ESP_OK;
}
static uint16_t audio_receiver_get_buffered_port(void) {
    unreached("audio_receiver_get_buffered_port"); return 0;
}
static bool start_audio_receiver_or_fail(int socket, rtsp_conn_t *conn,
                                         const rtsp_request_t *req, int64_t type) {
    (void)socket; (void)conn; (void)req; stream_setup_only("start_audio_receiver_or_fail");
    assert(type == AUDIO_STREAM_REALTIME); return true;
}
static void audio_receiver_set_playout_latency_samples(uint32_t n) {
    stream_setup_only("audio_receiver_set_playout_latency_samples");
    assert(n == AIRPLAY_RT_LATENCY_DEFAULT_SAMPLES);
}
static void audio_receiver_stop(void) { unreached("audio_receiver_stop"); }
static void audio_receiver_set_client_control(uint32_t ip, uint16_t port) {
    (void)ip; (void)port; unreached("audio_receiver_set_client_control");
}
static void audio_receiver_set_playing(bool playing) {
    stream_setup_only("audio_receiver_set_playing"); assert(playing);
}
static void rtsp_events_emit(rtsp_event_t event, const rtsp_event_data_t *data) {
    (void)data; stream_setup_only("rtsp_events_emit"); assert(event == RTSP_EVENT_PLAYING);
}
'''

MAIN = r'''
static const char V1_SETUP[] =
    "SETUP rtsp://192.168.1.30/3413821438 RTSP/1.0\r\n"
    "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;"
    "control_port=6001;timing_port=6002\r\n"
    "CSeq: 7\r\nDACP-ID: 14413BE4996FEA4D\r\nUser-Agent: iTunes/12.12\r\n\r\n";
static const char AP2_SETUP[] =
    "SETUP rtsp://192.168.1.30/3413821438 RTSP/1.0\r\n"
    "CSeq: 7\r\nUser-Agent: AirPlay/690.7.1\r\n\r\n";
static const char AP2_PLIST[] = "bplist00 (the stubs read nothing else)";
static rtsp_conn_t conn;
static hap_session_t hap = {.session_established = true};
static void setup(const char *raw, const char *body) {
    rtsp_request_t req; memset(&req, 0, sizeof(req));
    strcpy(req.method, "SETUP"); strcpy(req.protocol, "RTSP/1.0"); req.cseq = 7;
    req.body = (const uint8_t *)body; req.body_len = body ? strlen(body) : 0;
    if (body == AP2_PLIST) strcpy(req.content_type, "application/x-apple-binary-plist");
    reset_counts();
    handle_setup(3, &conn, &req, (const uint8_t *)raw, strlen(raw));
    assert(replies == 1);
}
static void first_v1_setup(void) {
    setup(V1_SETUP, NULL);
    assert(status == 200 && formats == 1 && ntp_starts == 1 && ptp_stops == 1);
    assert(port_allocs == 1 && transports == 1 && stream_types == 1);
    assert(conn.stream_active && conn.protocol_version == 1 && conn.stream_type == 96);
    assert(conn.client_control_port == 6001 && conn.client_timing_port == 6002);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    memset(&conn, 0, sizeof(conn));
    conn.data_socket = conn.control_socket = conn.event_socket = -1;
    conn.client_ip = 0x1401a8c0u;
    switch (atoi(argv[1])) {
    case 0: /* The RAOP SETUP before RECORD configures ALAC and the NTP clock. */
        first_v1_setup();
        assert(event_sockets == 0);
        break;
    case 1: { /* Another, while RECORD's stream task decodes, changes nothing. */
        first_v1_setup();
        rtsp_conn_t before;
        memcpy(&before, &conn, sizeof(conn));
        setup(V1_SETUP, NULL);
        assert(status == 455 && formats == 0 && ntp_starts == 0 && ptp_stops == 0);
        assert(port_allocs == 0 && transports == 0 && stream_types == 0);
        assert(!memcmp(&before, &conn, sizeof(conn)));
        /* With a body, before its keys reach the receiver (unreached() otherwise). */
        setup(V1_SETUP, "not a plist");
        assert(status == 455 && encryptions == 0 && formats == 0);
        assert(!memcmp(&before, &conn, sizeof(conn)));
        break;
    }
    case 2: /* TEARDOWN clears stream_active; the next SETUP is served again. */
        first_v1_setup();
        conn.stream_active = false;
        first_v1_setup();
        break;
    case 3: /* The gate is the Transport SETUP's: AirPlay 2's initial SETUP still passes. */
        conn.stream_active = true;
        conn.protocol_version = 2;
        setup(AP2_SETUP, NULL);
        assert(status == 200 && formats == 0 && encryptions == 0 && event_sockets == 1);
        break;
    case 4: /* A 32-byte key and its tag decrypt into the key; that key is used. */
        conn.hap_session = &hap;
        ekey_sent = 48;
        setup(AP2_SETUP, AP2_PLIST);
        assert(status == 200 && decrypts == 1 && derives == 0 && encryptions == 1);
        assert(last_encrypt.key_len == 32 && last_encrypt.key[31] == 0x5a);
        break;
    case 5: /* Longer, it is never decrypted (64 would write 48 into the 32-byte key): the
               session's derived key is used, as for an ekey that does not decrypt. */
        conn.hap_session = &hap;
        for (size_t sent = 49; sent <= 72; sent += 23) {   /* 72 arrives cut to 64 */
            ekey_sent = sent;
            setup(AP2_SETUP, AP2_PLIST);
            assert(status == 200 && decrypts == 0 && derives == 1 && encryptions == 1);
            assert(last_encrypt.key_len == 32 && last_encrypt.key[0] == 0x33);
        }
        break;
    case 6: { /* A stream at a rate the timing engine and the output's pace cannot divide
                 by is refused, before the receiver's format is set. */
        const int64_t refused[] = {400000000, 192001, 7999, 0, -44100};
        streams = true;
        for (size_t r = 0; r < sizeof(refused) / sizeof(refused[0]); ++r) {
            stream_sr = refused[r];
            setup(AP2_SETUP, AP2_PLIST);
            assert(status == 400 && formats == 0 && port_allocs == 0 && event_sockets == 0);
        }
        break;
    }
    case 7: /* 48 kHz is configured and the stream started. */
        streams = true;
        stream_sr = 48000;
        setup(AP2_SETUP, AP2_PLIST);
        assert(status == 200 && formats == 1 && last_format.sample_rate == 48000);
        assert(port_allocs == 1 && conn.stream_active && conn.protocol_version == 2);
        break;
    default: return 99;
    }
    return 0;
}
'''


def build(directory):
    handlers = (PORT / "rtsp_handlers.c").read_text(encoding="utf-8")
    message = (PORT / "rtsp_message.c").read_text(encoding="utf-8")
    source = directory / "airplay_setup.c"
    source.write_text("\n\n".join([
        PREFIX, production_types(), production_stream_type(), production_conn(), STUBS,
        function(message, "const uint8_t *rtsp_find_header_end("),
        function(handlers, "static const char *parse_raw_header("),
        function(handlers, "static void handle_setup("),
        MAIN]), encoding="utf-8")
    exe = directory / ("airplay_setup.exe" if os.name == "nt" else "airplay_setup")
    command, env = compiler_command(source, exe)
    built = subprocess.run(command, env=env, cwd=directory,
                           capture_output=True, text=True, timeout=120)
    if built.returncode:
        raise AssertionError("SETUP harness compilation failed:\n" + built.stdout + built.stderr)
    return exe, env


class AirplaySetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="airplay-setup-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable, cls.env = build(Path(cls.temp.name))

    def run_case(self, number):
        result = subprocess.run([str(self.executable), str(number)], env=self.env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_v1_setup_configures_the_stream(self): self.run_case(0)
    def test_v1_setup_during_a_stream_is_refused(self): self.run_case(1)
    def test_v1_setup_after_teardown_is_served(self): self.run_case(2)
    def test_airplay2_initial_setup_is_not_gated(self): self.run_case(3)
    def test_ekey_that_fits_the_key_is_decrypted(self): self.run_case(4)
    def test_ekey_longer_than_a_key_is_never_decrypted(self): self.run_case(5)
    def test_stream_rate_outside_the_range_is_refused(self): self.run_case(6)
    def test_stream_at_48_khz_is_configured(self): self.run_case(7)


if __name__ == "__main__":
    unittest.main(verbosity=2)
