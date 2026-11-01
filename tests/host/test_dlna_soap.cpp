#include "doctest.h"
#include "../../components/dlna_client/include/upnp_xml.h"
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <algorithm>

// Exercise the actual HTTP AVTransport adapter with only its platform seams mocked.
namespace soap_test {
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2,
              HTTPD_RESP_USE_STRLEN = -1;
int xSemaphoreTake(int, int) {
    return 1;
}
void xSemaphoreGive(int) {}
void *heap_caps_malloc(size_t n, int) {
    return malloc(n);
}
void *heap_caps_calloc(size_t n, size_t s, int) {
    return calloc(n, s);
}
void heap_caps_free(void *p) {
    free(p);
}
struct httpd_req_t {
    std::string body, action, response, status;
    size_t content_len = 0, read = 0;
};
int httpd_req_recv(httpd_req_t *r, char *out, size_t n) {
    n = std::min(n, r->body.size() - r->read);
    memcpy(out, r->body.data() + r->read, n);
    r->read += n;
    return (int)n;
}
int httpd_req_get_hdr_value_str(httpd_req_t *r, const char *, char *out, size_t cap) {
    if (r->action.size() >= cap)
        return -1;
    memcpy(out, r->action.c_str(), r->action.size() + 1);
    return 0;
}
void httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) {}
void httpd_resp_set_status(httpd_req_t *r, const char *s) {
    r->status = s;
}
void httpd_resp_set_type(httpd_req_t *, const char *) {}
int httpd_resp_send(httpd_req_t *r, const char *s, int n) {
    r->response.assign(s, n < 0 ? strlen(s) : n);
    return 0;
}
// The server's body reader (modesp/net/http_routes.h), a seam like the rest.
namespace modesp::http_routes {
bool read_body(httpd_req_t *r, char *out, bool) {
    if (httpd_req_recv(r, out, r->content_len) != (int)r->content_len)
        return false;
    out[r->content_len] = 0;
    return true;
}
} // namespace modesp::http_routes
constexpr const char *TS_STOPPED = "STOPPED", *TS_PLAYING = "PLAYING",
                     *TS_PAUSED = "PAUSED_PLAYBACK";
struct FakeState {
    dlna_core::Transport storage;
    dlna_core::Transport *model = &storage;
    int guard = 1;
    std::atomic<const char *> trans_state{TS_STOPPED};
    std::atomic<bool> ours{false}, command_error{false};
    std::atomic<int32_t> pos_s{0}, dur_s{0};
} d;
enum class Notify { AV_TRANSPORT };
void notify(Notify) {}
dlna_core::Mode transport_mode() {
    return !strcmp(d.trans_state.load(), TS_PLAYING)  ? dlna_core::Mode::Playing
           : !strcmp(d.trans_state.load(), TS_PAUSED) ? dlna_core::Mode::Paused
                                                      : dlna_core::Mode::Stopped;
}
#include "../../modules/dlna/src/dlna_soap.inc"
httpd_req_t call(const char *action, const std::string &args = "", const char *instance = "0") {
    httpd_req_t r;
    r.action = std::string("\"urn:schemas-upnp-org:service:AVTransport:1#") + action + "\"";
    r.body =
        std::string("<s:Envelope xmlns:s='http://schemas.xmlsoap.org/soap/envelope/'><s:Body><u:") +
        action + " xmlns:u='urn:schemas-upnp-org:service:AVTransport:1'><InstanceID>" + instance +
        "</InstanceID>" + args + "</u:" + action + "></s:Body></s:Envelope>";
    r.content_len = r.body.size();
    avt_soap(&r);
    return r;
}
void reset() {
    d.storage = dlna_core::Transport{};
    d.trans_state = TS_STOPPED;
    d.ours = false;
    d.command_error = false;
    d.pos_s = 0;
    d.dur_s = 0;
}
} // namespace soap_test
TEST_CASE("SOAP SetURI is atomic and XML is escaped once in replies") {
    using namespace soap_test;
    reset();
    auto r = call("SetAVTransportURI",
                  "<CurrentURI>http://nas/song?a=1&amp;b=2</"
                  "CurrentURI><CurrentURIMetaData>&lt;DIDL-Lite&gt;&lt;dc:title&gt;A &amp;amp; "
                  "B&lt;/dc:title&gt;&lt;/DIDL-Lite&gt;</CurrentURIMetaData>");
    CHECK(r.status.empty());
    CHECK(std::string(d.model->current.title) == "A & B");
    auto before = d.model->current;
    r = call("SetAVTransportURI", "<CurrentURI>http://nas/other</CurrentURI>");
    CHECK(r.status == "500 Internal Server Error");
    CHECK(std::string(d.model->current.uri) == before.uri);
    r = call("GetPositionInfo");
    CHECK(r.response.find("song?a=1&amp;b=2") != std::string::npos);
    CHECK(r.response.find("A &amp;amp; B") != std::string::npos);
}
TEST_CASE("SOAP faults distinguish bad instance, speed, mode, target and unknown action") {
    using namespace soap_test;
    reset();
    CHECK(call("GetTransportInfo", "", "1").response.find("<errorCode>718</errorCode>") !=
          std::string::npos);
    CHECK(call("Play", "<Speed>2</Speed>").response.find("<errorCode>717</errorCode>") !=
          std::string::npos);
    CHECK(call("Seek", "<Unit>TRACK_NR</Unit><Target>1</Target>")
              .response.find("<errorCode>710</errorCode>") != std::string::npos);
    CHECK(call("Seek", "<Unit>REL_TIME</Unit><Target>0:00:01</Target>")
              .response.find("<errorCode>711</errorCode>") != std::string::npos);
    CHECK(call("Unknown").response.find("<errorCode>401</errorCode>") != std::string::npos);
    CHECK(call("Next").response.find("<errorCode>711</errorCode>") != std::string::npos);
}
TEST_CASE("SOAP next URI is real state and Next preserves stopped mode") {
    using namespace soap_test;
    reset();
    d.model->current.set_uri("http://nas/first");
    CHECK(call("SetNextAVTransportURI", "<NextURI>http://nas/second</NextURI><NextURIMetaData/>")
              .status.empty());
    CHECK(call("GetMediaInfo").response.find("<NextURI>http://nas/second</NextURI>") !=
          std::string::npos);
    CHECK(call("Next").status.empty());
    dlna_core::Command cmd;
    REQUIRE(d.model->pop(cmd));
    CHECK(cmd.action == dlna_core::Action::Select);
    CHECK(cmd.mode == dlna_core::Mode::Stopped);
    CHECK(call("GetMediaInfo").response.find("<NextURI></NextURI>") != std::string::npos);
}
TEST_CASE("SOAP queue overflow is a fault, not a false success") {
    using namespace soap_test;
    reset();
    d.model->current.set_uri("http://nas/song");
    for (size_t i = 0; i < dlna_core::Transport::CAPACITY; ++i)
        CHECK(call("Play", "<Speed>1</Speed>").status.empty());
    CHECK(call("Play", "<Speed>1</Speed>").response.find("<errorCode>715</errorCode>") !=
          std::string::npos);
}
