"""Compile production AirPlay wire output with mocked transport and mDNS.

Direct run needs only Python stdlib + a native compiler:
    python tools/tests/test_airplay_mode.py
Also collected by pytest. Both profiles execute in the SAME native binary.
"""
import glob
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMP = ROOT / "components/airplay2"
PORT = COMP / "port"
UP = COMP / "upstream"


def function(source, signature):
    start = source.rindex(signature)
    return source[start:source.index("\n}", start) + 2]


def production(path):
    return re.sub(r"^\s*#include[^\n]*$", "", path.read_text(encoding="utf-8"),
                  flags=re.MULTILINE)


def compiler_command(source, exe):
    env = os.environ.copy()
    includes = [PORT, UP / "plist"]
    cc = shutil.which("cc") or shutil.which("gcc")
    if os.name == "nt" and not cc:
        cc = next((p for p in (r"C:\msys64\ucrt64\bin\gcc.exe",
                               r"C:\msys64\mingw64\bin\gcc.exe")
                   if Path(p).is_file()), None)
    if cc:
        env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
        return ([cc, "-std=c11", "-O0", str(source), "-o", str(exe)] +
                ["-I" + str(p) for p in includes]), env
    if os.name != "nt":
        raise AssertionError("A native C compiler is required for AirPlay wire tests")
    bases = [os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
             os.environ.get("ProgramFiles", r"C:\Program Files")]
    vcvars = sorted(p for base in bases for p in glob.glob(str(Path(base) /
        "Microsoft Visual Studio/*/*/VC/Auxiliary/Build/vcvars64.bat")))
    if not vcvars:
        raise AssertionError("Neither MinGW nor Visual Studio C build tools were found")
    # cmd call is required for the SDK's batch environment setup.
    setup = subprocess.run(f'call "{vcvars[-1]}" >nul && set', shell=True,
                           capture_output=True, text=True, timeout=60)
    if setup.returncode:
        raise AssertionError(setup.stdout + setup.stderr)
    for line in setup.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key:
            env[key] = value
    cc = shutil.which("cl", path=env.get("Path", env.get("PATH", "")))
    if not cc:
        raise AssertionError("Visual Studio environment did not supply cl.exe")
    return ([cc, "/nologo", "/std:c11", "/Od", "/D_CRT_SECURE_NO_WARNINGS",
             str(source), "/Fe:" + str(exe)] + ["/I" + str(p) for p in includes]), env


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include "plist.h"
#include "mode_policy.h"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_OK 0
#define MDNS_NAME_BUF_LEN 64
typedef int esp_err_t;
typedef ptrdiff_t ssize_t;
typedef struct { bool encrypted_mode; } rtsp_conn_t;
typedef struct {
    char method[32], path[256], protocol[16];
    int cseq; char content_type[64]; size_t content_length;
    const uint8_t *body; size_t body_len;
} rtsp_request_t;
static bool legacy, artwork = true;
static const uint8_t mac[6] = {2,4,6,8,10,12};
static const uint8_t pk[32] = {1,2,3,4};
bool airplay2_port_legacy(void) { return legacy; }
bool airplay2_port_artwork_wanted(void) { return artwork; }
const uint8_t *airplay2_port_mac(void) { return mac; }
const char *airplay2_port_name(void) { return "P4 Test"; }
const uint8_t *hap_get_public_key(void) { return pk; }
void rtsp_get_device_id(char *out, size_t n) { snprintf(out,n,"02:04:06:08:0A:0C"); }
static const char *info_name = "P4 Test";   /* the device name /info carries */
void settings_get_device_name(char *out, size_t n) { snprintf(out,n,"%s",info_name); }
esp_err_t settings_get_volume(float *db) { *db = -18.9f; return 0; }   /* the room at 37 % */
const char *esp_err_to_name(int err) { (void)err; return "mock"; }
static int host_strncasecmp(const char *a,const char *b,size_t n) {
    for(size_t i=0;i<n;++i) {
        int x=tolower((unsigned char)a[i]),y=tolower((unsigned char)b[i]);
        if(x!=y || !a[i] || !b[i]) return x-y;
    } return 0;
}
#define strncasecmp host_strncasecmp
static char *host_strcasestr(const char *a,const char *b) {
    size_t n=strlen(b);
    for(;*a;++a) if(!host_strncasecmp(a,b,n)) return (char*)a;
    return n ? NULL : (char*)a;
}
#define strcasestr host_strcasestr
static uint8_t captured[8192];
static size_t captured_len;
static int crypto_calls,send_calls;
ssize_t send(int socket,const void *data,size_t n,int flags) {
    (void)socket;(void)flags;
    if(n>11) n=11; /* Exercise the real partial-write send_all loop. */
    assert(captured_len+n<=sizeof(captured));
    memcpy(captured+captured_len,data,n); captured_len+=n; ++send_calls;
    return (ssize_t)n;
}
int rtsp_crypto_write_frame(int socket,rtsp_conn_t *conn,const uint8_t *data,size_t n) {
    (void)socket;(void)conn; assert(n<=sizeof(captured));
    memcpy(captured,data,n); captured_len=n; ++crypto_calls; return 0;
}
typedef struct { const char *key,*value; } mdns_txt_item_t;
typedef struct {
    bool exists; char type[16],name[80]; uint16_t port; size_t count;
    char keys[32][32],values[32][100];
} service_t;
static service_t services[2];
static int adds;
static int service_index(const char *type) {
    if(!strcmp(type,"_airplay")) return 0;
    assert(!strcmp(type,"_raop")); return 1;
}
esp_err_t mdns_hostname_get(char *host) { strcpy(host,"p4-test"); return ESP_OK; }
bool mdns_service_exists(const char *type,const char *proto,const char *name) {
    (void)proto;(void)name; return services[service_index(type)].exists;
}
esp_err_t mdns_service_add(const char *name,const char *type,const char *proto,
                           uint16_t port,mdns_txt_item_t *txt,size_t count) {
    assert(!strcmp(proto,"_tcp"));
    service_t *s=&services[service_index(type)];
    assert(!s->exists && count<=32); memset(s,0,sizeof(*s));
    s->exists=true; s->port=port; s->count=count;
    snprintf(s->type,sizeof(s->type),"%s",type);
    snprintf(s->name,sizeof(s->name),"%s",name);
    for(size_t i=0;i<count;++i) {
        snprintf(s->keys[i],sizeof(s->keys[i]),"%s",txt[i].key);
        snprintf(s->values[i],sizeof(s->values[i]),"%s",txt[i].value);
    } ++adds; return ESP_OK;
}
esp_err_t mdns_service_remove(const char *type,const char *proto) {
    (void)proto; services[service_index(type)].exists=false; return ESP_OK;
}
'''

MAIN = r'''
static void reset_capture(void) { captured_len=0; crypto_calls=0; send_calls=0; }
static void save_capture(int step,const char *kind,bool encrypted) {
    char path[80]; snprintf(path,sizeof(path),"%d-%s-%d.bin",step,kind,encrypted);
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fwrite(captured,1,captured_len,f)==captured_len); fclose(f);
    assert(encrypted ? crypto_calls==1 && send_calls==0 : crypto_calls==0 && send_calls>1);
}
static void save_services(int step) {
    char path[80]; snprintf(path,sizeof(path),"%d-mdns.txt",step);
    FILE *f=fopen(path,"wb"); assert(f);
    for(int j=0;j<2;++j) {
        service_t *s=&services[j]; if(!s->exists) continue;
        fprintf(f,"service=%s|%s|%u\n",s->type,s->name,s->port);
        for(size_t i=0;i<s->count;++i) fprintf(f,"%s=%s\n",s->keys[i],s->values[i]);
    } fclose(f);
}
int main(void) {
    /* AP2 -> legacy -> AP2: the firmware latches one per boot, both must be linked.
       Then AP2 once more with covers not asked for (airplay2_config_t::artwork), and a
       name that is not ASCII. */
    for(int step=0;step<4;++step) {
        legacy=step==1;
        artwork=step!=3;
        info_name=step==3 ? STEP3_NAME : "P4 Test";
        for(int encrypted=0;encrypted<2;++encrypted) {
            rtsp_conn_t conn={.encrypted_mode=encrypted!=0};
            const char body[]={'a','\0','b'};
            for(int variant=0;variant<4;++variant) {
                reset_capture();
                assert(rtsp_send_response(7,&conn,200,"OK",42,
                    (variant&1) ? "X-Test: preserved\r\n" : NULL,
                    (variant&2) ? body : NULL,(variant&2) ? sizeof(body) : 0)==0);
                char kind[16]; snprintf(kind,sizeof(kind),"rtsp%d",variant);
                save_capture(step,kind,encrypted!=0);
            }
            reset_capture();
            assert(rtsp_send_http_response(7,&conn,200,"OK",
                "application/octet-stream",body,sizeof(body))==0);
            save_capture(step,"http",encrypted!=0);
            rtsp_request_t req={0}; strcpy(req.path,"/info"); req.cseq=42;
            for(int rtsp=0;rtsp<2;++rtsp) {
                strcpy(req.protocol,rtsp ? "RTSP/1.0" : "HTTP/1.1");
                reset_capture(); handle_get(7,&conn,&req,NULL,0);
                save_capture(step,rtsp ? "info-binary" : "info-xml",encrypted!=0);
            }
        }
        adds=0; mdns_airplay_init(); assert(adds==(legacy ? 1 : 2));
        mdns_airplay_init(); assert(adds==(legacy ? 1 : 2));
        save_services(step);
        mdns_airplay_remove(); assert(!services[0].exists && !services[1].exists);
    }
    /* The binary /info alone, for each of NAMES. */
    for(size_t i=0;i<sizeof(NAMES)/sizeof(NAMES[0]);++i) {
        uint8_t body[1024];
        size_t n=bplist_build_info_response_vol(body,sizeof(body),"02:04:06:08:0A:0C",
                                                NAMES[i],pk,32,0,2,-18.9);
        char path[32]; snprintf(path,sizeof(path),"name-%zu.bin",i);
        FILE *f=fopen(path,"wb"); assert(f && n);
        assert(fwrite(body,1,n,f)==n); fclose(f);
    } return 0;
}
'''


def c_string(raw):
    """Bytes as a C string literal, every byte escaped (no greedy \\x run-ons)."""
    return '"' + "".join(f"\\x{b:02x}" for b in raw) + '"'


# The device name /info carries at step 3 of the harness.
STEP3_NAME = "Кухня і зала \U0001F3B5"
# Names for the binary /info alone: the UTF-8 the builder is given, the string a plist
# reader must find, and the object that holds it (marker and length, then the text).
NAME_CASES = [
    (b"P4 Test", "P4 Test", b"\x57P4 Test"),
    ("Кухня".encode(), "Кухня", b"\x65" + "Кухня".encode("utf-16-be")),
    # 15 code units take the long length form, as an ASCII string of 15 bytes would.
    (STEP3_NAME.encode(), STEP3_NAME, b"\x6f\x10\x0f" + STEP3_NAME.encode("utf-16-be")),
    ("\U0001F3B5".encode(), "\U0001F3B5", b"\x62\xd8\x3c\xdf\xb5"),
    # Bytes that start no well-formed sequence are U+FFFD, one each: a lone 0xFF, a lead
    # cut short by the end, an overlong '/', a UTF-8 surrogate.
    (b"Bad \xff\xd0", "Bad \ufffd\ufffd", None),
    (b"\xc0\xaf\xed\xa0\x80", "\ufffd" * 5, None),
]


def build_harness(directory):
    handlers = (PORT / "rtsp_handlers.c").read_text(encoding="utf-8")
    builder = production(PORT / "bplist_builder.c")
    info_builder = (builder[:builder.index("size_t bplist_build_initial_setup(")] +
                    function(builder, "size_t bplist_build_info_response_vol(") +
                    function(builder, "size_t bplist_build_info_response("))
    names = ("#define STEP3_NAME " + c_string(STEP3_NAME.encode()) + "\n" +
             "static const char *const NAMES[] = {" +
             ", ".join(c_string(raw) for raw, _, _ in NAME_CASES) + "};\n")
    pieces = [PREFIX, names, production(UP / "plist/base64.c"),
              production(UP / "plist/plist_xml.c"), info_builder,
              production(PORT / "rtsp_message.c"),
              function(handlers, "static bool request_uses_rtsp("),
              function(handlers, "static void plist_dict_real("),
              function(handlers, "static void handle_get(int socket, rtsp_conn_t *conn, const rtsp_request_t *req,"),
              "#define TAG MDNS_TAG\n" + production(PORT / "mdns_airplay.c") + "\n#undef TAG",
              MAIN]
    source = directory / "airplay_mode.c"
    source.write_text("\n\n".join(pieces), encoding="utf-8")
    exe = directory / ("airplay_mode.exe" if os.name == "nt" else "airplay_mode")
    command, env = compiler_command(source, exe)
    built = subprocess.run(command, env=env, cwd=directory,
                           capture_output=True, text=True, timeout=120)
    if built.returncode:
        raise AssertionError("AirPlay harness compilation failed:\n"+built.stdout+built.stderr)
    ran = subprocess.run([str(exe)], env=env, cwd=directory,
                         capture_output=True, text=True, timeout=30)
    if ran.returncode:
        raise AssertionError(f"AirPlay harness exited {ran.returncode}:\n{ran.stdout}{ran.stderr}")


# The AirPlay 2 profile's features: upstream's, plus the classic-metadata bits a sender reads
# instead of md - 17 text, 16 progress, and 15 artwork when covers are asked for.
AP2_FEATURES = 0x1C340405C4A00 | 1 << 17 | 1 << 16
AP2_FEATURES_COVERS = AP2_FEATURES | 1 << 15
LEGACY_FEATURES = 0x5C4A00


def expected_features(step):
    return (LEGACY_FEATURES if step == 1 else
            AP2_FEATURES if step == 3 else AP2_FEATURES_COVERS)


class AirplayModeWireTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="airplay-mode-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        build_harness(cls.directory)

    def capture(self, step, kind, encrypted=0):
        return (self.directory / f"{step}-{kind}-{encrypted}.bin").read_bytes()

    def response(self, step, kind, encrypted=0):
        head, body = self.capture(step, kind, encrypted).split(b"\r\n\r\n", 1)
        lines = head.decode("ascii").split("\r\n")
        headers = dict(line.split(": ", 1) for line in lines[1:])
        if "Content-Length" in headers:
            self.assertEqual(int(headers["Content-Length"]), len(body))
        return lines[0], headers, body

    def test_rtsp_bytes_preserve_all_body_header_variants(self):
        for step in range(3):
            version = "105.1" if step == 1 else "377.40.00"
            for variant in range(4):
                expected = ("RTSP/1.0 200 OK\r\nCSeq: 42\r\n"
                            f"Server: AirTunes/{version}\r\n")
                if variant & 1:
                    expected += "X-Test: preserved\r\n"
                if variant & 2:
                    expected += "Content-Length: 3\r\n"
                expected = (expected+"\r\n").encode() + (b"a\0b" if variant & 2 else b"")
                for encrypted in range(2):
                    with self.subTest(step=step,variant=variant,encrypted=encrypted):
                        self.assertEqual(self.capture(step,f"rtsp{variant}",encrypted),expected)

    def test_http_bytes_preserve_binary_body_and_mode(self):
        for step in range(3):
            for encrypted in range(2):
                status, headers, body = self.response(step,"http",encrypted)
                self.assertEqual(status,"HTTP/1.1 200 OK")
                self.assertEqual(headers["Server"],"AirTunes/"+("105.1" if step==1 else "377.40.00"))
                self.assertEqual(headers["Content-Type"],"application/octet-stream")
                self.assertEqual(body,b"a\0b")
            self.assertEqual(self.capture(step,"http",0),self.capture(step,"http",1))

    def test_info_binary_and_xml_agree_on_runtime_mode(self):
        for step in range(4):
            decoded=[]
            for kind in ("info-binary","info-xml"):
                status,headers,body=self.response(step,kind)
                self.assertTrue(status.endswith("200 OK"))
                data=plistlib.loads(body); decoded.append(data)
                self.assertEqual(data["features"],expected_features(step))
                self.assertEqual(data["vv"],1 if step==1 else 2)
                self.assertEqual(data["srcvers"],"105.1" if step==1 else "377.40.00")
                self.assertEqual(data["model"],"Shairport" if step==1 else "AudioAccessory5,1")
                self.assertEqual(data["deviceid"],"02:04:06:08:0A:0C")
                self.assertEqual(data["name"],STEP3_NAME if step==3 else "P4 Test")
                self.assertEqual(data["pk"],bytes([1,2,3,4])+bytes(28))
                # The room's level, so a sender starts its slider there (UPSTREAM.md item 15).
                self.assertEqual(data["initialVolume"],-18.9)
                self.assertEqual(self.capture(step,kind,0),self.capture(step,kind,1))
            self.assertEqual(decoded[0],decoded[1])

    def test_binary_info_name_is_any_text(self):
        # CoreFoundation reads a 0x5X string as ASCII: the name is UTF-16 once it is not.
        for i, (_, name, encoded) in enumerate(NAME_CASES):
            with self.subTest(name=name):
                body = (self.directory / f"name-{i}.bin").read_bytes()
                self.assertEqual(plistlib.loads(body)["name"], name)
                if encoded:
                    self.assertIn(encoded, body)

    def test_mdns_legacy_excludes_ap2_service_and_pairing(self):
        for step in range(4):
            services={}
            for line in (self.directory/f"{step}-mdns.txt").read_text().splitlines():
                key,value=line.split("=",1)
                if key=="service":
                    kind,name,port=value.split("|")
                    current=services[kind]={"name":name,"port":int(port)}
                else:
                    current[key]=value
            self.assertEqual(set(services),{"_raop"} if step==1 else {"_raop","_airplay"})
            raop=services["_raop"]
            self.assertEqual(raop["name"],"020406080A0C@P4 Test")
            self.assertEqual(raop["port"],5000 if step==1 else 7000)
            self.assertEqual(raop["md"],"0,2" if step==3 else "0,1,2")
            self.assertEqual(raop["am"],"Shairport" if step==1 else "AudioAccessory5,1")
            if step==1:
                for forbidden in ("ft","pk","sf","vv","vs"):
                    self.assertNotIn(forbidden,raop)
                self.assertEqual(raop["et"],"0,1")
                self.assertEqual(raop["cn"],"0,1")
                self.assertEqual(raop["vn"],"3")
            else:
                f=expected_features(step)
                self.assertEqual(raop["ft"],f"0x{f & 0xFFFFFFFF:X},0x{f >> 32:X}")
                self.assertEqual(raop["vv"],"2")
                self.assertEqual(raop["vs"],"377.40.00")
                self.assertEqual(services["_airplay"]["port"],7000)
                self.assertEqual(services["_airplay"]["features"],raop["ft"])


if __name__=="__main__":
    unittest.main(verbosity=2)
