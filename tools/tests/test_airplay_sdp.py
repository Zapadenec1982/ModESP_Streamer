"""Run the production ANNOUNCE/SDP handler against stubbed receiver, RSA and sodium.

The decode buffer and the PCM ring slots hold AUDIO_MAX_CHANNELS 16-bit channels of at
most MAX_SAMPLES_PER_FRAME frames (audio_buffer.h). An SDP describing more must be
refused before the receiver is reconfigured, and so must a rate outside
AIRPLAY_RATE_MIN_HZ..AIRPLAY_RATE_MAX_HZ (port/airplay2_port.h).
Direct run: python tools/tests/test_airplay_sdp.py
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

try:
    from .test_airplay_mode import PORT, UP, compiler_command, function
except ImportError:
    from test_airplay_mode import PORT, UP, compiler_command, function


def production_types():
    receiver = (UP / "audio/audio_receiver.h").read_text(encoding="utf-8")
    formats = receiver[receiver.index("typedef struct {\n  char codec[32];"):
                       receiver.index("// Audio buffer statistics")]
    message = (UP / "rtsp/rtsp_message.h").read_text(encoding="utf-8")
    request = message[message.index("typedef struct {\n  char method[32];"):
                      message.index("} rtsp_request_t;") + len("} rtsp_request_t;")]
    limits = (UP / "audio/audio_buffer.h").read_text(encoding="utf-8")
    rates = (PORT / "airplay2_port.h").read_text(encoding="utf-8")
    defines = "\n".join(re.search(rf"^#define {name}\s+\d+$", text, re.MULTILINE).group(0)
                        for text, name in ((limits, "AUDIO_MAX_CHANNELS"),
                                           (limits, "MAX_SAMPLES_PER_FRAME"),
                                           (rates, "AIRPLAY_RATE_MIN_HZ"),
                                           (rates, "AIRPLAY_RATE_MAX_HZ")))
    return defines + "\n" + formats + "\n" + request


PREFIX = r'''
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef struct {
  bool stream_active;
  char codec[32];
  int sample_rate, channels, bits_per_sample;
  uint8_t protocol_version;
} rtsp_conn_t;
'''

STUBS = r'''
static size_t host_strlcpy(char *out, const char *in, size_t n) {
    size_t len = strlen(in);
    if (n) { size_t c = len < n - 1 ? len : n - 1; memcpy(out, in, c); out[c] = 0; }
    return len;
}
#define strlcpy host_strlcpy
static char *host_strcasestr(const char *a, const char *b) {
    size_t n = strlen(b);
    for (; *a; ++a) {
        size_t i = 0;
        while (i < n && tolower((unsigned char)a[i]) == tolower((unsigned char)b[i])) ++i;
        if (i == n) return (char *)a;
    }
    return NULL;
}
#define strcasestr host_strcasestr
#define sodium_base64_VARIANT_ORIGINAL 1
#define sodium_base64_VARIANT_ORIGINAL_NO_PADDING 3
static int formats, encryptions, status, replies;
static audio_format_t last_format;
static audio_encrypt_t last_encrypt;
static int rsa_decrypt_aes_key(const char *b64, uint8_t *key, size_t size, size_t *len) {
    (void)b64; assert(size >= 16); memset(key, 0x5a, 16); *len = 16; return 0;
}
static int sodium_base642bin(unsigned char *bin, size_t max, const char *b64, size_t b64_len,
                             const char *ignore, size_t *len, const char **end, int variant) {
    (void)b64; (void)b64_len; (void)ignore; (void)end; (void)variant;
    assert(max >= 16); memset(bin, 0x11, 16); *len = 16; return 0;
}
static void audio_receiver_set_format(const audio_format_t *format) {
    ++formats; last_format = *format;
}
static void audio_receiver_set_encryption(const audio_encrypt_t *encrypt) {
    ++encryptions; last_encrypt = *encrypt;
}
static int rtsp_send_response(int socket, rtsp_conn_t *conn, int code, const char *text,
                              int cseq, const char *headers, const char *body, size_t len) {
    (void)socket; (void)conn; (void)text; (void)headers; (void)body; (void)len;
    assert(cseq == 7); ++replies; status = code; return 0;
}
static int rtsp_send_ok(int socket, rtsp_conn_t *conn, int cseq) {
    return rtsp_send_response(socket, conn, 200, "OK", cseq, NULL, NULL, 0);
}
'''

MAIN = r'''
static const char HEAD[] =
    "v=0\r\no=iTunes 3413821438 0 IN IP4 192.168.1.20\r\ns=iTunes\r\n"
    "c=IN IP4 192.168.1.30\r\nt=0 0\r\nm=audio 0 RTP/AVP 96\r\n";
static const char KEYS[] =
    "a=rsaaeskey:VjVbxWcmYgbBbhwBNlCh3K0CMNtWoB844BuiHGUJT51zQS7SDpMnlbBIobsKbfEJ\r\n"
    "a=aesiv:zcZmAZtqh7uGcEwPXk0QeA\r\na=min-latency:11025\r\n";
static rtsp_conn_t conn;
static void announce(const char *media) {
    static char body[1024];
    snprintf(body, sizeof(body), "%s%s%s", HEAD, media, KEYS);
    rtsp_request_t req; memset(&req, 0, sizeof(req));
    strcpy(req.method, "ANNOUNCE"); strcpy(req.protocol, "RTSP/1.0"); req.cseq = 7;
    req.body = (const uint8_t *)body; req.body_len = strlen(body);
    formats = encryptions = status = replies = 0;
    handle_announce(3, &conn, &req, req.body, req.body_len);
    assert(replies == 1);
}
static void refused(const char *media) {
    announce(media);
    /* Refused whole: no format, no key, and the session is left as it was. */
    assert(status == 400 && formats == 0 && encryptions == 0);
    assert(conn.protocol_version == 0 && conn.channels == 0 && conn.codec[0] == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: /* The SDP iTunes and AirPlay 1 senders send is accepted as before. */
        announce("a=rtpmap:96 AppleLossless\r\n"
                 "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n");
        assert(status == 200 && formats == 1 && encryptions == 1);
        assert(!strcmp(last_format.codec, "AppleLossless"));
        assert(last_format.channels == 2 && last_format.num_channels == 2);
        assert(last_format.max_samples_per_frame == 352 && last_format.sample_size == 16);
        assert(last_format.bits_per_sample == 16 && last_format.sample_rate == 44100);
        assert(last_encrypt.key_len == 16 && conn.protocol_version == 1);
        assert(conn.channels == 2 && !strcmp(conn.codec, "AppleLossless"));
        break;
    case 1: /* Mono and the largest frame the decode buffer holds still pass. */
        announce("a=rtpmap:96 AppleLossless/44100/1\r\n"
                 "a=fmtp:96 4096 0 16 40 10 14 1 255 0 0 44100\r\n");
        assert(status == 200 && formats == 1);
        assert(last_format.channels == 1 && last_format.max_samples_per_frame == 4096);
        break;
    case 2: refused("a=rtpmap:96 AppleLossless/44100/4\r\n"); break;
    case 3: refused("a=rtpmap:96 L16/44100/0\r\n"); break;
    case 4: /* fmtp channels; 258 would also wrap to 2 in the uint8_t cookie field. */
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 352 0 16 40 10 14 4 255 0 0 44100\r\n");
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 352 0 16 40 10 14 258 255 0 0 44100\r\n");
        break;
    case 5:
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 4097 0 16 40 10 14 2 255 0 0 44100\r\n");
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 0 0 16 40 10 14 2 255 0 0 44100\r\n");
        break;
    case 6:
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 352 0 24 40 10 14 2 255 0 0 96000\r\n");
        break;
    case 7: /* An AAC fmtp is not the ALAC parameter list and is left alone. */
        announce("a=rtpmap:96 mpeg4-generic/44100/2\r\n"
                 "a=fmtp:96 mode=AAC-hbr; sizelength=13\r\n");
        assert(status == 200 && formats == 1 && last_format.channels == 2);
        assert(last_format.max_samples_per_frame == 1024);
        break;
    case 8: /* After SETUP the stream task decodes with the decoder an SDP replaces. */
        conn.stream_active = true;
        announce("a=rtpmap:96 AppleLossless\r\n"
                 "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n");
        assert(status == 455 && formats == 0 && encryptions == 0);
        assert(conn.protocol_version == 0 && conn.codec[0] == 0);
        break;
    case 9: /* The timing engine and the output's pace divide by the rate: 8..192 kHz. */
        refused("a=rtpmap:96 L16/400000000/2\r\n");
        refused("a=rtpmap:96 L16/0/2\r\n");
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 4000000000\r\n");
        refused("a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 7999\r\n");
        announce("a=rtpmap:96 AppleLossless\r\n"
                 "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 48000\r\n");
        assert(status == 200 && formats == 1 && last_format.sample_rate == 48000);
        break;
    default: return 99;
    }
    return 0;
}
'''


def build(directory):
    handlers = (PORT / "rtsp_handlers.c").read_text(encoding="utf-8")
    source = directory / "airplay_sdp.c"
    source.write_text("\n\n".join([
        PREFIX, production_types(), STUBS,
        function(handlers, "static bool parse_sdp("),
        function(handlers, "static void handle_announce("),
        MAIN]), encoding="utf-8")
    exe = directory / ("airplay_sdp.exe" if os.name == "nt" else "airplay_sdp")
    command, env = compiler_command(source, exe)
    built = subprocess.run(command, env=env, cwd=directory,
                           capture_output=True, text=True, timeout=120)
    if built.returncode:
        raise AssertionError("SDP harness compilation failed:\n" + built.stdout + built.stderr)
    return exe, env


class AirplaySdpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="airplay-sdp-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable, cls.env = build(Path(cls.temp.name))

    def run_case(self, number):
        result = subprocess.run([str(self.executable), str(number)], env=self.env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_itunes_sdp_still_configures_stereo_alac(self): self.run_case(0)
    def test_mono_and_largest_frame_are_accepted(self): self.run_case(1)
    def test_rtpmap_more_than_two_channels_is_refused(self): self.run_case(2)
    def test_rtpmap_zero_channels_is_refused(self): self.run_case(3)
    def test_fmtp_channel_count_is_bounded(self): self.run_case(4)
    def test_fmtp_frame_length_is_bounded(self): self.run_case(5)
    def test_fmtp_other_than_16_bit_is_refused(self): self.run_case(6)
    def test_aac_fmtp_is_not_parsed_as_alac(self): self.run_case(7)
    def test_announce_during_a_stream_is_refused(self): self.run_case(8)
    def test_rate_outside_the_supported_range_is_refused(self): self.run_case(9)


if __name__ == "__main__":
    unittest.main(verbosity=2)
