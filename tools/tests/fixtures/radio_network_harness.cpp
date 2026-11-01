
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "http_body.h"
#define ESP_LOGI(...)
#define ESP_LOGW(...)
constexpr size_t PLAY_URL_LEN=257;
constexpr int CONNECT_MS=8000, READ_MS=1000;
const char* TAG="Radio";
namespace radio_core { constexpr size_t URL_LEN=224; namespace next_stream {int read(void*,uint8_t*,size_t){return -1;} }
 struct Station { char name[64]{}; char url[257]{}; char uuid[37]{}; bool valid() const { return url[0] != 0; } }; }
constexpr int32_t TRANSIENT = -2;
enum State:uint8_t {ST_IDLE,ST_CONNECTING,ST_BUFFERING,ST_PLAYING,ST_RECONNECTING};
struct StateData {
 int tr=0; bool tr_ssl=false; void* reader=(void*)1; void* prepared=nullptr;
 bool run=true; char meta_buffer[4081]{}; char* meta=meta_buffer;
 char content_type[64]{}; int icy_meta=0,icy_left=0,icy_remaining=-1,icy_have=0; uint32_t bitrate=0;
 char icy_name[96]{}; uint64_t body_bytes=0; radio_core::HttpBody body;
 std::atomic<bool> eof{false},lost{false}; bool decoder_drained=false;
 size_t head_len=0,head_at=0; char icy_buffer[4081]{}; char* icy_buf=icy_buffer;
 std::atomic<uint8_t> state{ST_PLAYING}; std::atomic<uint32_t> track_generation{0};
 struct {int count=2; radio_core::Station station[2];} list{2, {{"", "http://host/a", ""}, {"", "http://host/b", ""}}};
 radio_core::Station transient;
 std::atomic<int> station{0},tuned{-1}; std::atomic<bool> net_up{true},one_shot{false},wav{false};
 uint32_t pcm_generation=0;
 char now[128]{};std::atomic<uint32_t> now_gen{0};int retry=0;
 char url[257]="http://host/a";
 std::atomic<int32_t> dur_s{20}; uint32_t src_rate=44100;uint8_t src_bits=16,src_ch=2;
 uint32_t first_pcm_size=0;struct{void reset(){}}conv;size_t fade_in_left=0;
 std::atomic<uint64_t> frames_in{0};
 std::atomic<uint32_t> peer_ip{0},avoid_ip{0};
} s;
constexpr size_t EDGE=480;
std::string response; size_t transport_at=0,max_read=4080;
int esp_transport_tcp_init(){return 1;}int esp_transport_ssl_init(){return 1;}
int esp_crt_bundle_attach=0;
void esp_transport_ssl_crt_bundle_attach(int,int){}
void esp_transport_ssl_set_common_name(int,const char*){}
uint32_t peer_ipv4(int){return 0;}
bool other_address(const char*,uint32_t,char*,size_t){return false;}
struct in_addr{uint32_t s_addr;};
char* inet_ntoa_r(in_addr a,char* out,int n){snprintf(out,n,"%u.%u.%u.%u",unsigned(a.s_addr&255),unsigned(a.s_addr>>8&255),unsigned(a.s_addr>>16&255),unsigned(a.s_addr>>24));return out;}
std::vector<std::string> responses,hosts; size_t connects=0; // per-connection script, when set
int esp_transport_connect(int,const char* host,int,int){hosts.push_back(host);if(connects<responses.size()){response=responses[connects];transport_at=0;}++connects;return 0;}
std::string last_request;
int esp_transport_write(int,const char* d,int n,int){last_request.assign(d,n);return n;}
int esp_transport_read(int,char* out,int cap,int){int n=std::min<size_t>(std::min<size_t>(cap,max_read),response.size()-transport_at);memcpy(out,response.data()+transport_at,n);transport_at+=n;return n;}
void http_close(){s.state.store(ST_CONNECTING);s.tr=0;}
void close_stream(){http_close();}
int claims=0;bool paused=false;   // what the radio did to the ring
namespace audio_core {
 struct PcmRing {enum Owner {OWNER_RADIO=2}; bool claim(Owner){++claims;return true;} void set_paused(bool on){paused=on;};
   bool set_eos(Owner,uint32_t,bool){return true;} bool request_flush(Owner,uint32_t){return true;}
   bool flush_pending(){return false;} bool reset(Owner,uint32_t){return true;}
   Owner owner(){return OWNER_RADIO;} uint32_t generation(){return 0;}};
 PcmRing& pcm_ring(){static PcmRing x;return x;}
 constexpr int FLUSH_WAIT_NAPS=60;   // pcm_ring.h; the mock ring has nothing to fade
 template<class Nap> bool claim_after_fade(PcmRing& r,PcmRing::Owner who,Nap){return r.claim(who);}
 uint64_t stream_seek_byte(uint64_t b,int sec,int dur,uint8_t,uint8_t){return b*sec/dur;}
}
void raw_clear(){}; void fade_queued_audio(){};void set_modem_sleep(bool){};
void sleep_ms(uint32_t){};
void stop_station(){http_close();s.state.store(ST_IDLE);}
int tunes=0;bool tune(const char*){++tunes;return true;}
std::vector<std::string> icy_segments;size_t segment=0,segment_at=0;
int sock_read(char* out,int cap){if(segment>=icy_segments.size())return 0;auto& q=icy_segments[segment];if(q.empty()){++segment;return 0;}int n=std::min<size_t>(cap,q.size()-segment_at);memcpy(out,q.data()+segment_at,n);segment_at+=n;if(segment_at==q.size()){++segment;segment_at=0;}return n;}
void parse_icy(const char*,size_t){}
bool split_url(const char* url, bool& ssl, char* host, size_t host_sz, int& port,
               char* path, size_t path_sz) {
    const char* p = url;
    if (!strncasecmp(p, "https://", 8)) { ssl = true;  p += 8; port = 443; }
    else if (!strncasecmp(p, "http://", 7)) { ssl = false; p += 7; port = 80; }
    else return false;

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = nullptr;

    const size_t hlen = colon ? (size_t)(colon - p) : (slash ? (size_t)(slash - p) : strlen(p));
    if (!hlen || hlen >= host_sz) return false;
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    if (colon) port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return false;
    snprintf(path, path_sz, "%s", slash ? slash : "/");
    return true;
}

bool next_line(const char*& p, const char* end, const char*& line, size_t& len) {
    if (p >= end) return false;
    const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
    if (!nl) return false;
    line = p;
    len = (size_t)(nl - p);
    if (len && line[len - 1] == '\r') len--;
    p = nl + 1;
    return true;
}

bool header_is(const char* line, size_t len, const char* name, const char*& value,
               size_t& vlen) {
    const size_t n = strlen(name);
    if (len < n + 1 || strncasecmp(line, name, n) || line[n] != ':') return false;
    const char* v = line + n + 1;
    size_t rest = len - n - 1;
    while (rest && (*v == ' ' || *v == '\t')) { v++; rest--; }
    value = v;
    vlen = rest;
    return true;
}

void copy_header(const char* v, size_t n, char* out, size_t out_sz) {
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, v, n);
    out[n] = '\0';
}

enum class Hop : uint8_t { FAILED, OPENED, REDIRECTED };
Hop http_open_hop(const char*, char*, size_t, int64_t, char*, size_t) {
    return Hop::FAILED;   // replaced by modules/radio/src/radio_module.cpp at test time
}
bool http_open(const char*, char*, size_t, int = 4, int64_t = -1) {
    return false;         // replaced by modules/radio/src/radio_module.cpp at test time
}

int stream_read(uint8_t* dst, size_t max) {
    if (s.prepared) return radio_core::next_stream::read(s.prepared, dst, max);
    size_t got = 0;
    while (got == 0 && s.run) {
        if (s.icy_meta && s.icy_left == 0) {
            // A length byte, then that many sixteens of metadata. Zero is the usual case:
            // the block is only filled when the title changes.
            uint8_t lenb = 0;
            int r = sock_read((char*)&lenb, 1);
            if (r <= 0) return got ? (int)got : r;
            const int mlen = (int)lenb * 16;
            if (mlen > 0) {
                char* meta = s.icy_buf;     // PSRAM; the reader task is the only caller
                int have = 0;
                while (have < mlen) {
                    r = sock_read(meta + have, mlen - have);
                    if (r <= 0) return got ? (int)got : r;
                    have += r;
                }
                meta[have] = '\0';
                parse_icy(meta, (size_t)have);
            }
            s.icy_left = s.icy_meta;
        }
        size_t want = max - got;
        if (s.icy_meta && (int)want > s.icy_left) want = (size_t)s.icy_left;
        if (!want) continue;
        const int r = sock_read((char*)(dst + got), (int)want);
        if (r <= 0) return got ? (int)got : r;
        got += (size_t)r;
        if (s.icy_meta) s.icy_left -= r;
    }
    return (int)got;
}

const radio_core::Station* station_at(int idx) {
 if (idx == TRANSIENT) return s.transient.valid() ? &s.transient : nullptr;
 return (idx >= 0 && idx < s.list.count) ? &s.list.station[idx] : nullptr;
}

// The firmware copies under list_guard(); one task here, so no lock.
bool station_url(int idx, char* out, size_t n) {
 const radio_core::Station* st = station_at(idx);
 if (st) snprintf(out, n, "%s", st->url);
 return st != nullptr;
}

void start_station(int idx) {
    s.track_generation.fetch_add(1);
    if (idx < 0 || idx >= s.list.count) return;
    // Already on it and playing: leave it alone. Otherwise a second request for the same
    // station — the screen sends one, the state change sends another — would drop a working
    // stream and start it again, which is heard as the radio refusing to start.
    const uint8_t st = s.state.load(std::memory_order_acquire);
    if (idx == s.station.load(std::memory_order_acquire) &&
        (st == ST_BUFFERING || st == ST_PLAYING)) {
        return;
    }
    if (!s.net_up.load(std::memory_order_acquire)) {
        // Asked for before the network is up: remember what was wanted and let the
        // reconnect path bring it in as soon as there is a route.
        s.station.store(idx, std::memory_order_release);
        s.state.store(ST_RECONNECTING, std::memory_order_release);
        return;
    }
    s.station.store(idx, std::memory_order_release);
    s.one_shot.store(false, std::memory_order_release);
    s.now[0] = '\0';
    s.now_gen.fetch_add(1, std::memory_order_release);
    s.retry = 0;
    fade_queued_audio();
    set_modem_sleep(false);
    audio_core::pcm_ring().claim(audio_core::PcmRing::OWNER_RADIO);
    audio_core::pcm_ring().set_paused(false);
    audio_core::pcm_ring().set_eos(false);
    if (!tune(s.list.station[idx].url)) {
        s.state.store(ST_RECONNECTING, std::memory_order_release);
    }
}

bool seek_stream(int32_t sec) {
    const int32_t dur = s.dur_s.load(std::memory_order_acquire);
    if (!s.body_bytes || dur <= 0 || !s.src_rate) {
        ESP_LOGW(TAG, "nothing to seek in: no length yet");
        return false;
    }
    if (sec < 0) sec = 0;
    if (sec > dur - 1) sec = dur - 1;

    const uint64_t byte = audio_core::stream_seek_byte(s.body_bytes, sec, dur,
                                                       s.src_bits, s.src_ch);
    ESP_LOGI(TAG, "seek to %ld s = byte %llu of %llu", (long)sec,
             (unsigned long long)byte, (unsigned long long)s.body_bytes);

    // What is already queued belongs to where we were. Ask the consumer to fade it out and
    // drop it rather than cutting it off, which is a step and therefore a click.
    audio_core::pcm_ring().request_flush();

    http_close();
    raw_clear();
    s.head_len = 0;
    s.head_at  = 0;
    s.lost.store(false, std::memory_order_release);
    if (!http_open(s.url, nullptr, 0, 4, (int64_t)byte)) {
        ESP_LOGW(TAG, "seek could not reconnect");
        return false;
    }
    s.first_pcm_size = 0;
    s.conv.reset();                       // the filter history is from before the jump
    s.fade_in_left = EDGE;                // arrive at the new place, do not cut into it
    s.frames_in.store((uint64_t)sec * s.src_rate, std::memory_order_release);
    s.state.store(ST_BUFFERING, std::memory_order_release);
    return true;
}

// Replaced by the production function (test_network_radio_browser.py).
bool looks_like_playlist(const char*, const char*) {
 return false;
}

int main(){
 s.station.store(1);s.tuned.store(0);s.state.store(ST_PLAYING);
 start_station(1);
 if(tunes!=1 || s.tuned.load()!=1) return 1; // requested station is not the tuned station
 // Chosen with no network: the ring is the radio's and a push's one_shot is gone before the
 // wait, since the reconnect path only tunes; only the connect waits.
 s.net_up.store(false);s.one_shot.store(true);s.retry=3;s.tr=1;paused=true;claims=0;tunes=0;
 s.state.store(ST_PLAYING);
 start_station(0);
 if(s.state.load()!=ST_RECONNECTING || s.one_shot.load() || s.retry || claims!=1 || paused ||
    s.tr || tunes || s.tuned.load()!=0) return 4;
 s.net_up.store(true);

 response="HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: 10000\r\n\r\n";
 response.append(4080-response.size(),'Q');
 transport_at=0;max_read=4080;
 if(!http_open("http://host/a",nullptr,0) || s.head_len!=4080-response.find("\r\n\r\n")-4 ||
    s.meta[s.head_len-1]!='Q') return 2; // full read must keep the final body byte

 response="HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nABCD";
 transport_at=0;max_read=3;
 if(!http_open("http://host/a",nullptr,0)) return 3; // split header

 for(int split=-1;split<16;++split){
   s.prepared=nullptr;s.icy_meta=4;s.icy_left=0;s.icy_remaining=-1;s.icy_have=0;
   segment=0;segment_at=0;
   std::string meta="StreamTitle='X';"; // one complete 16-byte metadata block
   if(split<0) icy_segments={"",std::string(1,1),meta,"WXYZ"};
   else if(split==0) icy_segments={std::string(1,1),"",meta,"WXYZ"};
   else icy_segments={std::string(1,1),meta.substr(0,split),"",meta.substr(split),"WXYZ"};
   uint8_t out[4]{};
   int first=stream_read(out,4), second=stream_read(out,4);
   if(first!=0 || second!=4 || memcmp(out,"WXYZ",4)) return 10+split+1;
 }

 max_read=4080;response.clear();transport_at=0;s.body_bytes=3528044;
 s.dur_s.store(20);s.src_rate=44100;s.state.store(ST_PLAYING);s.wav.store(false);s.tr=1;
 handle_seek(10);
 if(s.state.load()!=ST_IDLE || s.tr!=0) return 40; // failed reconnect cannot strand CONNECTING

 s.wav.store(true);s.state.store(ST_PLAYING);s.tr=1;
 handle_seek(10);
 if(s.state.load()!=ST_PLAYING || s.tr!=1) return 41; // unknown WAV layout leaves playback alone
 // Redirects: four are followed, a fifth is refused, a 3xx without a Location fails. The
 // chain is the one that overflowed the radio stack on 04.10.2026, when every hop recursed.
 auto redirect=[](const char* to){return std::string("HTTP/1.1 302 Found\r\nLocation: ")+to+"\r\nContent-Length: 0\r\n\r\n";};
 const std::string ok="HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nABCD";
 max_read=4080;
 responses={redirect("http://h1/x"),redirect("https://h2/y"),redirect("http://h3/z"),redirect("https://h4/w?token=1"),ok};
 connects=0;hosts.clear();
 if(!http_open("http://h0/a",nullptr,0) || connects!=5 || hosts.back()!="h4" || s.head_len!=4) return 50;
 responses={redirect("http://h1/x"),redirect("http://h2/x"),redirect("http://h3/x"),redirect("http://h4/x"),redirect("http://h5/x"),ok};
 connects=0;hosts.clear();
 if(http_open("http://h0/a",nullptr,0) || connects!=5) return 51; // the fifth redirect is refused
 responses={"HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n"};
 connects=0;
 if(http_open("http://h0/a",nullptr,0) || connects!=1) return 52; // nowhere to go
 // A Location longer than a station URL but within a pushed one is followed whole; a longer
 // one is refused rather than cut into a URL that fails as a 403.
 const std::string signed_path="/s?zt="+std::string(230,'t');
 responses={redirect(("http://h1"+signed_path).c_str()),ok};
 connects=0;
 if(!http_open("http://h0/a",nullptr,0) || connects!=2 ||
    last_request.compare(0,4+signed_path.size()+1,"GET "+signed_path+" ")) return 53;
 responses={redirect(("http://h1/"+std::string(300,'t')).c_str()),ok};
 connects=0;
 if(http_open("http://h0/a",nullptr,0) || connects!=1) return 54;
 responses.clear();
 // A playlist by the answer's type when it says anything: an audio type is a stream whatever
 // the URL ends in, and only a type that says neither leaves it to the extension.
 if(looks_like_playlist("http://x/listen.pls","audio/mpeg") ||
    !looks_like_playlist("http://x/listen.pls","text/plain") ||
    !looks_like_playlist("http://x/listen.M3U?sid=1",nullptr) ||
    !looks_like_playlist("http://x/a","audio/x-mpegurl") ||
    !looks_like_playlist("http://x/a","audio/x-scpls") ||
    !looks_like_playlist("http://x/listen.pls","audio/x-pls") ||
    looks_like_playlist("http://x/a","application/octet-stream")) return 60;
 puts("radio network regressions passed");
 return 0;
}
