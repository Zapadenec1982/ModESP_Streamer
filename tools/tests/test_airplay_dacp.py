"""Run the production RTSP dispatch's DACP bookkeeping against stubbed handlers.

The DACP session is global (dacp_client.c). An AirPlay 1 slot replaced during its grace
period (rtsp_server.c) leaves its sender's session set, and an AirPlay 2 sender names no
DACP-ID or Active-Remote, so nothing replaced it: the panel's pause and next went to the
previous device. A SETUP on a connection without both identifiers clears the session; a
connection with them keeps setting its own.
Direct run: python tools/tests/test_airplay_dacp.py
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_airplay_mode import PORT, compiler_command, function
    from .test_airplay_sdp import production_types
    from .test_airplay_setup import production_conn
except ImportError:
    from test_airplay_mode import PORT, compiler_command, function
    from test_airplay_sdp import production_types
    from test_airplay_setup import production_conn


PREFIX = r'''
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct { int unused; } hap_session_t;
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
static int host_strcasecmp(const char *a, const char *b) {
    return host_strncasecmp(a, b, (size_t)-1);
}
#define strcasecmp host_strcasecmp
static size_t host_strlcpy(char *out, const char *in, size_t n) {
    size_t len = strlen(in);
    if (n) { size_t c = len < n - 1 ? len : n - 1; memcpy(out, in, c); out[c] = 0; }
    return len;
}
#define strlcpy host_strlcpy
static void unreached(const char *what) {
    fprintf(stderr, "unexpected call: %s\n", what);
    abort();
}
typedef void (*rtsp_handler_fn)(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                                const uint8_t *raw_request, size_t raw_len);
typedef struct { const char *method; rtsp_handler_fn handler; } rtsp_method_handler_t;
static int handled;
static void handle_any(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,
                       const uint8_t *raw, size_t raw_len) {
    (void)socket; (void)conn; (void)req; (void)raw; (void)raw_len; ++handled;
}
static const rtsp_method_handler_t method_handlers[] = {
    {"OPTIONS", handle_any}, {"GET", handle_any}, {"SETUP", handle_any},
    {"RECORD", handle_any}, {NULL, NULL}};
/* dacp_client.c's session, as far as the dispatch moves it. */
static char session_id[32];
static int sets, clears;
static void dacp_set_session(const char *id, const char *remote, uint32_t ip) {
    (void)remote; (void)ip; snprintf(session_id, sizeof(session_id), "%s", id); ++sets;
}
static void dacp_clear_session(void) { session_id[0] = 0; ++clears; }
static int rtsp_send_response(int socket, rtsp_conn_t *conn, int code, const char *text,
                              int cseq, const char *headers, const char *body, size_t len) {
    (void)socket; (void)conn; (void)code; (void)text; (void)cseq; (void)headers;
    (void)body; (void)len; unreached("rtsp_send_response"); return 0;
}
static int rtsp_send_http_response(int socket, rtsp_conn_t *conn, int code, const char *text,
                                   const char *type, const char *body, size_t len) {
    (void)socket; (void)conn; (void)code; (void)text; (void)type; (void)body; (void)len;
    unreached("rtsp_send_http_response"); return 0;
}
'''

MAIN = r'''
static const char V1_REMOTE[] = "DACP-ID: 14413BE4996FEA4D\r\nActive-Remote: 2543110914\r\n";
static rtsp_conn_t conn;
static void request(const char *method, const char *headers) {
    char raw[512];
    const int n = snprintf(raw, sizeof(raw),
                           "%s rtsp://192.168.1.30/3413821438 RTSP/1.0\r\nCSeq: 3\r\n%s"
                           "User-Agent: test\r\n\r\n", method, headers);
    assert(n > 0 && (size_t)n < sizeof(raw));
    const int before = handled;
    assert(rtsp_dispatch(3, &conn, (const uint8_t *)raw, (size_t)n) == 0);
    assert(handled == before + 1);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    memset(&conn, 0, sizeof(conn));
    conn.client_ip = 0x1401a8c0u;
    switch (atoi(argv[1])) {
    case 0: /* An AirPlay 1 sender names its remote on every request; its SETUP keeps it. */
        request("OPTIONS", V1_REMOTE);
        request("SETUP", V1_REMOTE);
        request("RECORD", V1_REMOTE);
        assert(sets == 3 && clears == 0 && !strcmp(session_id, "14413BE4996FEA4D"));
        break;
    case 1: /* A replaced AirPlay 1 slot left its sender's session: an AirPlay 2 sender's
               SETUP clears it, and nothing before the SETUP does. */
        strcpy(session_id, "14413BE4996FEA4D");
        request("GET", "");
        assert(clears == 0 && session_id[0]);
        request("SETUP", "");
        assert(clears == 1 && sets == 0 && !session_id[0]);
        break;
    case 2: /* Half an identity is no remote either. */
        strcpy(session_id, "14413BE4996FEA4D");
        request("SETUP", "DACP-ID: 0011223344556677\r\n");
        assert(clears == 1 && sets == 0 && !session_id[0]);
        break;
    default: return 99;
    }
    return 0;
}
'''


def build(directory):
    handlers = (PORT / "rtsp_handlers.c").read_text(encoding="utf-8")
    message = (PORT / "rtsp_message.c").read_text(encoding="utf-8")
    source = directory / "airplay_dacp.c"
    source.write_text("\n\n".join([
        PREFIX, production_types(), production_conn(), STUBS,
        function(message, "const uint8_t *rtsp_find_header_end("),
        function(message, "int rtsp_parse_cseq("),
        function(message, "int rtsp_parse_content_length("),
        function(message, "const uint8_t *rtsp_get_body("),
        function(message, "int rtsp_request_parse("),
        function(handlers, "static const char *parse_raw_header("),
        function(handlers, "static bool request_uses_rtsp("),
        function(handlers, "int rtsp_dispatch("),
        MAIN]), encoding="utf-8")
    exe = directory / ("airplay_dacp.exe" if os.name == "nt" else "airplay_dacp")
    command, env = compiler_command(source, exe)
    built = subprocess.run(command, env=env, cwd=directory,
                           capture_output=True, text=True, timeout=120)
    if built.returncode:
        raise AssertionError("DACP harness compilation failed:\n" + built.stdout + built.stderr)
    return exe, env


class AirplayDacpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="airplay-dacp-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable, cls.env = build(Path(cls.temp.name))

    def run_case(self, number):
        result = subprocess.run([str(self.executable), str(number)], env=self.env,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_airplay1_sender_keeps_its_remote(self): self.run_case(0)
    def test_setup_without_dacp_clears_a_previous_senders_session(self): self.run_case(1)
    def test_half_an_identity_is_no_remote(self): self.run_case(2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
