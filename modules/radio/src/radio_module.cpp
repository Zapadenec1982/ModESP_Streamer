/**
 * @file radio_module.cpp
 * @brief Internet radio — HTTP in, the PCM ring out.
 *
 * The requirement is that it behaves like a radio (docs/NETWORK.md, "Internet radio"): no server in
 * the house, nothing to log into, and when the power comes back the last station plays. So
 * the station list, the tuning and the reconnecting all live here, and none of it depends on
 * anything else being up.
 *
 * Shape, which is deliberately the card player's shape: one task, not the audio task, doing
 * network and decoding — both block for tens of milliseconds and the audio task may never
 * block — and the two are separated by the lock-free ring in components/audio_dsp. The
 * stretch from decoder to ring is components/audio_pipe, shared with the player rather than
 * written twice.
 *
 * What is different from a file. A stream has no end and no length, so there is no duration
 * and no seeking. It arrives at whatever rate the network manages rather than as fast as a
 * card can be read, so the buffer is the thing that keeps it playing: several seconds of
 * COMPRESSED audio, which at 128 kbit/s costs far less memory than the same seconds of PCM.
 * And it stops without warning — a router reboots, a station restarts its encoder — so
 * reconnecting is not an error path, it is the normal path, and it never gives up.
 */

#include "radio_module.h"

#include "audio_pipe.h"
#include "pcm_ring.h"
#include "stations.h"
#include "safe_file.h"
#include "user_store.h"
#include "http_body.h"
#include "radio_art_task.h"
#include "radio_catalog_task.h"
#include "next_stream.h"
#include "stream_watch.h"
#include "modesp/liveness.h"
#include "modesp/net/http_routes.h"   // GET/POST /api/stations (stations_http.inc)
#include "state_enums.h"   // audio.source values, generated from the manifests

#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task.h"

#include <atomic>
#include <cstdio>
#include <new>            // placement new for the PSRAM-backed task state
#include <cstdlib>
#include <cstring>

namespace {

const char* TAG = "Radio";

// Both producers share core 0 with app_main. A one-tick sleep in one producer
// does not let main run when the other producer remains ready at a higher priority.
// Equal priorities let FreeRTOS time-slice all three, including during decoding
// and resampling. The real-time output task stays on core 1 at its own priority.
static_assert(configUSE_TIME_SLICING == 1, "Radio requires main-task time slicing");
constexpr UBaseType_t RADIO_TASK_PRIORITY = ESP_TASK_MAIN_PRIO;


/// The rate the output starts at. A station is played at its own rate; this is only the
/// fallback for one that never says, and the size the ring is cut to.
constexpr uint32_t OUT_RATE = audio_core::PIPE_OUT_RATE;

/// audio.source value that means "the network stream". From generated/state_enums.h and
/// the table in docs/CONCEPT.md, "`audio.source` values"; the modules share the MANIFEST, not each
/// other's headers, because a build may carry either one without the other.
constexpr int32_t AUDIO_SRC_RADIO = modesp::gen::AUDIO_SOURCE_RADIO;

/// Where the station list lives. Editable by hand, which is half the point of it being JSON.
// Resolved once, at load_stations(), through UserStore: /userdata/stations.json when the
// store is up, /data/stations.json on a panel still running the old partition table.
// Read and write go to the SAME place on purpose — unlike eq.json there is no factory
// list in the image to fall back to; radio_core::defaults() is the fallback.
char s_stations_path[160] = "/data/stations.json";

/// The compressed buffer: at 128 kbit/s this is eight seconds of audio for 128 KB, which is
/// the cheapest form of resilience there is. The same eight seconds as PCM would be 1.5 MB,
/// and the PCM ring stays half a second for the same reason it always was — what it protects
/// against is a late decode, not a late network.
constexpr size_t RAW_BYTES = 128 * 1024;
/// Start playing once this much is in hand. Too little and the first hiccup empties it; too
/// much and tuning feels broken. Two seconds at a typical bitrate.
constexpr size_t PRIME_BYTES = 32 * 1024;
/// One decode call's PCM. Generous: an AAC frame can decode to a lot at once.
constexpr size_t PCM_BYTES = 32 * 1024;
// esp_audio_codec 2.5.0 packs a mono AAC frame in place in this buffer with a loop twice as
// long as the frame: it writes up to 8 KB past out.buffer and reads up to 12 KB (fixed in
// 2.6.2, which needs P4 rev 3.0). At 12 KB or more the overrun stays inside our own buffer.
static_assert(PCM_BYTES >= 12 * 1024, "esp_audio_codec 2.5.0 mono AAC overruns a smaller out buffer");
/// What that buffer may grow to for a frame that does not fit it, as the decoder asks
/// (needed_size): 24-bit FLAC at 96 kHz is 48 KB a frame, 5.1 at 4096 samples 73 KB.
constexpr size_t PCM_MAX = 256 * 1024;
/// Frames converted and resampled at a time.
///
/// 1024, which is what the card player uses, and it is not an arbitrary number.
///
/// At 4096 the resampler took 887 ms out of every second for 44.1 -> 48 kHz, and a stream
/// played at a quarter speed while its own buffer sat 98% full. The same library at the same
/// taps and filter count did the same work for the card at 1.0x real time; the block size was
/// the whole difference. A block that walks 96 KB of PSRAM with a 48-tap window re-reads it
/// far past anything the cache holds.
///
/// The absolute cost has since come down a long way — moving the phase arithmetic off
/// soft-float doubles took a conversion from 55% of a core to 19% (bench 18.09.2026) — but
/// the cache argument is about locality, not arithmetic, so the block size still matters.
constexpr size_t CHUNK     = 1024;
/// One read from the socket. Small enough to stay responsive to a stop, large enough that
/// the TLS record overhead does not dominate.
constexpr size_t NET_CHUNK = 4096;
/// Long enough for a name to resolve and a TLS handshake to finish on a slow link.
constexpr int    CONNECT_MS = 8000;
/// One socket read while streaming. Short on purpose, and a timeout is NOT a lost stream:
/// it is how the reader notices it has been asked to stop, and how a station that has gone
/// quiet is given a few more chances before the connection is thrown away.
constexpr int    READ_MS = 1000;
/// Consecutive read timeouts that mean the station really has gone.
constexpr int    READ_GIVE_UP = 8;
/// How many times a pushed track is picked up again where it broke off.
///
/// A station reconnects without limit, because a radio that has to be restarted by hand is
/// not a radio. A track is different: it has an end, so a failure that repeats is a failure
/// that is not going to clear, and twenty attempts is already a minute of trying. The number
/// is deliberately generous, because this link has been measured losing a third to three
/// quarters of its packets under stream load (docs/NETWORK.md, "Wi-Fi through the ESP32-C6") — the
/// one thing that must not happen is a track stopping for good on a link that would have
/// come back.
constexpr int    RESUME_TRIES = 20;

/// 10 ms of fade at each end of a stream, which is all it takes to make a start or a stop
/// inaudible. The same shape the card player uses at a track join.
constexpr size_t EDGE = 480;

enum class Cmd : uint8_t { PLAY, STOP, NEXT, PREV, TUNE, URL, SEEK, CATALOG };

struct CmdMsg {
    Cmd     type;
    int32_t value = 0;                      ///< TUNE: the station index; SEEK: the second
    char uri[MODESP_MAX_LONG_STRING_LENGTH + 1]{};   ///< URL and CATALOG: the stream
};

/// `station` for the catalog station that is not in the list. Not −1: that is what `tuned`
/// holds when nothing is tuned, and "already on it" compares the two. Published as −1.
constexpr int32_t TRANSIENT = -2;

/// A URL that came from outside the list is longer than one that came from inside it.
///
/// radio_core::URL_LEN is 224, sized for what a station directory writes. A control point
/// pushes the media server's own item id and often a session token with it, and a URL cut
/// at 224 does not fail as a truncated URL — it fails as a 404 from a server that looks
/// fine. This matches what SharedState can carry, so nothing is lost on the way in either.
constexpr size_t PLAY_URL_LEN = MODESP_MAX_LONG_STRING_LENGTH + 1;

/// What the outside sees, as words rather than numbers, because these are for a person.
const char* const STATE_NAMES[] = {"idle", "connecting", "buffering", "playing", "reconnecting"};
enum State : uint8_t { ST_IDLE, ST_CONNECTING, ST_BUFFERING, ST_PLAYING, ST_RECONNECTING };

struct RadioTaskState {
    QueueHandle_t q = nullptr;
    TaskHandle_t  task = nullptr;
    TaskHandle_t  reader = nullptr;
    volatile bool run = false;
    bool          up = false;               ///< buffers and tasks exist right now
    /// One per task, bumped once per turn. Watched only while the radio is the selected
    /// source, because that is the only time these two tasks exist at all.
    modesp::liveness::Counter beat{0};
    modesp::liveness::Counter reader_beat{0};

    radio_core::StationList list;
    std::atomic<int32_t> station{0};        ///< index into `list`, or TRANSIENT
    /// A station played from the catalog without being in the list (stage 20б). Station
    /// semantics — it reconnects for ever — which a pushed URL does not have. Written by the
    /// radio task from the CATALOG command that carries its URL, and by a web edit on main
    /// that parks a list station here (apply_station_edit) — both under list_guard(), which
    /// the task's URL copies take too, so no reader sees a URL halfway through being written.
    radio_core::Station transient;
    /// The list station whose stream is open — or being opened — right now; −1 when none is
    /// (stopped, or playing a pushed URL). NOT `station`: on_update writes that the moment a
    /// tune is asked for, before the task has done anything, so "already on it" tested
    /// against `station` was always true and a tune while another station played was
    /// dropped (24.09.2026).
    std::atomic<int32_t> tuned{-1};
    std::atomic<uint8_t> state{ST_IDLE};

    esp_transport_handle_t tr = nullptr;
    bool                   tr_ssl = false;
    esp_audio_simple_dec_handle_t dec = nullptr;
    audio_core::RateConverter conv;

    // The compressed buffer, as a ring shared by the two tasks. A plain buffer with a
    // length only works when one task does both jobs, and that is exactly what made the
    // decoder wait: filling a 4 KB chunk from a 128 kbit/s station takes a quarter of a
    // second, and nothing was decoded while it did.
    uint8_t* raw = nullptr;
    std::atomic<uint32_t> raw_head{0};       ///< the reader writes here
    std::atomic<uint32_t> raw_tail{0};       ///< the decoder reads here
    uint8_t* pcm = nullptr;
    size_t   pcm_cap = PCM_BYTES;           ///< what `pcm` holds; grows, never shrinks
    // Working buffers, in PSRAM rather than as statics. As .bss they were twelve kilobytes
    // of INTERNAL memory, which on this board is the memory the co-processor's receive pool
    // competes for — and docs/CONCEPT.md, "Network" puts the confirmed failure threshold for
    // this transport at 6.7 KB of free DMA-capable RAM. None of this is touched by an interrupt.
    uint8_t* net_chunk = nullptr;           ///< one socket read, reader task
    uint8_t* feed = nullptr;                ///< one decode call's input, decoder task
    char*    meta = nullptr;                ///< the response head, then its audio tail
    char*    icy_buf = nullptr;             ///< one ICY metadata block, reader task
    float*   fbuf = nullptr;                ///< CHUNK frames at the stream rate
    float*   rbuf = nullptr;                ///< the same, converted — only when it must be

    // ICY metadata: every `icy_meta` bytes of audio there is a block saying what is playing.
    char     content_type[64] = "";         ///< from the response head
    // Audio that arrived in the same read as the response head. Radio servers send it that
    // way, and throwing it away would put a hole at the start of every stream.
    radio_core::HttpBody body;
    std::atomic<bool> eof{false};
    bool decoder_drained = false;
    radio_core::next_stream::Prepared* prepared = nullptr;
    uint32_t first_pcm_size = 0;
    std::atomic<uint32_t> track_generation{0};
    uint32_t pcm_generation = 0;           ///< ring generation claimed by this radio session
    size_t   head_len = 0;
    size_t   head_at = 0;
    char     icy_name[radio_core::NAME_LEN] = "";
    int      icy_meta = 0;                  ///< 0 = the station sends none
    int      icy_left = 0;                  ///< audio bytes until the next block
    int      icy_remaining = -1;            ///< -1: awaiting metadata length byte
    int      icy_have = 0;                  ///< metadata bytes retained across timeouts
    char     now[128] = "";                 ///< StreamTitle, as the station wrote it
    std::atomic<uint32_t> now_gen{0};

    std::atomic<bool> net_up{false};        ///< wifi.connected, mirrored for the task
    uint32_t src_rate = 0;
    bool     direct = false;                ///< playing at the stream's own rate
    /// The rate the ring is fed at: the output's answer, or the fallback until it comes.
    /// Zero until the stream's format is known.
    uint32_t clock = 0;
    std::atomic<bool> wav{false};           ///< seek requires exact data chunk layout
    uint8_t  src_bits = 0, src_ch = 0;
    uint32_t bitrate = 0;                   ///< kbit/s, for the format line a person reads
    uint32_t bitrate_bps = 0;               ///< bits/s, for the arithmetic — see audio_pipe.h
    /// The whole entity's length in bytes: Content-Length, or the total out of a
    /// Content-Range when the GET carried a Range. **Zero means no end**, which is what a
    /// station is, and what makes duration and seeking not apply to one.
    uint64_t body_bytes = 0;
    /// Source frames decoded since this track started, including what a seek skipped over.
    /// Divided by the source rate this is the position — counted where audio leaves the
    /// decoder rather than where bytes arrive, so a full buffer does not run it fast.
    std::atomic<uint64_t> frames_in{0};
    std::atomic<int32_t>  dur_s{0};
    /// The reader is inside a socket read right now, and the transport under it must not be
    /// destroyed. See park_reader().
    std::atomic<bool> in_read{false};
    char     url[PLAY_URL_LEN] = "";
    /// What was ASKED for, as opposed to `url`, which is what is playing. They differ while
    /// a playlist is being followed, and the request has to survive the trip through the
    /// queue; each URL command owns its own snapshot.
    char     url_req[PLAY_URL_LEN] = "";
    /// The track as the task was asked for it, kept when `url` moves on to a playlist's
    /// stream. radio.url, completed_uri and promoted_uri carry this one, because a control
    /// point knows its track only by the URL it pushed (deep review 2026-10-12, RP-3).
    /// Written by the radio task; main reads it for the report as it read `url`.
    char     asked[PLAY_URL_LEN] = "";
    /// How many times this track has been picked up again after a break.
    int      resumes = 0;
    /// One track, not a station.
    ///
    /// The two differ in exactly one place and it is the END. A station never ends, so the
    /// stream going quiet means the network broke and the only right answer is to reconnect,
    /// forever — that is what makes it a radio. A track ends on purpose, and reconnecting
    /// then plays it again from the top, which a control point sees as a device that will
    /// not stop. Same streamer, opposite reading of the same event.
    std::atomic<bool> one_shot{false};
    char     fmt[32] = "";
    size_t   fade_in_left = 0;
    std::atomic<bool> lost{false};          ///< the reader saw the stream end
    int      retry = 0;                     ///< how many reconnects in a row
    // Stream health, reported once every few seconds while a station is on. A radio that
    // stutters has exactly two possible reasons — the network is not keeping up, or the
    // decoder is not — and these two counters tell them apart without any guessing.
    std::atomic<uint32_t> net_bytes{0};      ///< received since the last report
    /// The server the stream is connected to (IPv4, network order; 0 when not known), and
    /// one it could not keep up with: the next connect to the same host prefers another of
    /// its addresses (stream_watch.h). Cleared when a station is tuned.
    std::atomic<uint32_t> peer_ip{0};
    std::atomic<uint32_t> avoid_ip{0};
    std::atomic<uint32_t> dec_frames{0};     ///< output frames since the last report
    std::atomic<uint32_t> pump_calls{0};
    std::atomic<uint32_t> pump_idle{0};
    std::atomic<uint32_t> dec_us{0};         ///< time inside the decoder itself
    std::atomic<uint32_t> conv_us{0};        ///< time inside the resampler
};

/// The radio task's whole state, in PSRAM.
///
/// 18 KB of the ~19 that follow are the station list: 64 x (64-byte name + 224-byte URL),
/// plus a second copy of the same list inside radio_core::StationStore for the screen. As
/// internal .bss that is memory taken permanently from the pool the SDIO co-processor's
/// receive buffers and the I2S DMA descriptors come out of, for a list that changes when
/// somebody edits it.
///
/// A reference rather than a pointer, so every `s.` below reads the same as before. Static
/// constructors run after do_core_init(), i.e. after esp_psram_init() and after the heap
/// exists, so the allocation here is safe at this point in startup.
///
/// Nothing in this file runs in an ISR and none of these fields are handed to DMA — the
/// buffers that ARE (raw/pcm/net_chunk/...) are allocated separately, and the same reasoning
/// is already written down where they are. PSRAM is unreachable while the flash cache is
/// off, and a task that touches it is suspended for the duration rather than faulting.
RadioTaskState& radio_task_state() {
    static RadioTaskState* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(RadioTaskState), 2,
                                            MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        // A board with no usable memory at all has bigger problems than the radio; the
        // configASSERT keeps this from being a null dereference three frames later.
        configASSERT(mem != nullptr);
        return new (mem) RadioTaskState();
    }();
    return *p;
}
RadioTaskState& s = radio_task_state();
/// Main task only: whether the station's server keeps up (stream_watch.h).
radio_core::StarveWatch s_starve;
SemaphoreHandle_t io_guard() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t value = xSemaphoreCreateMutexStatic(&storage);
    return value;
}
/// The station list and the transient slot: held by main while apply_station_edit() replaces
/// them, and by the radio task while it copies a URL out of them or into the slot. Never
/// held across anything that blocks.
SemaphoreHandle_t list_guard() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t value = xSemaphoreCreateMutexStatic(&storage);
    return value;
}
portMUX_TYPE end_mux = portMUX_INITIALIZER_UNLOCKED;
char completed_uri[PLAY_URL_LEN]{};
char promoted_uri[PLAY_URL_LEN]{};
uint32_t completed_sequence = 0;


void sleep_ms(uint32_t ms) {
    const TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

// ── the compressed ring ───────────────────────────────────────────────────────────────────
//
// One reader task and one decoder task, one index each, no locks — the same shape as the
// PCM ring, for the same reason: the decoder must never wait on the network, and the reader
// must never wait on the decoder. One byte is left unused so full and empty are different.

size_t raw_used() {
    const uint32_t h = s.raw_head.load(std::memory_order_acquire);
    const uint32_t t = s.raw_tail.load(std::memory_order_acquire);
    return (h >= t) ? (h - t) : (RAW_BYTES - t + h);
}
size_t raw_space() { return RAW_BYTES - 1 - raw_used(); }
void   raw_clear() {
    s.raw_head.store(0, std::memory_order_release);
    s.raw_tail.store(0, std::memory_order_release);
}

size_t raw_write(const uint8_t* src, size_t n) {
    const size_t room = raw_space();
    if (n > room) n = room;
    if (!n) return 0;
    const uint32_t h = s.raw_head.load(std::memory_order_relaxed);
    const size_t first = (h + n <= RAW_BYTES) ? n : (RAW_BYTES - h);
    memcpy(s.raw + h, src, first);
    if (n > first) memcpy(s.raw, src + first, n - first);
    s.raw_head.store((uint32_t)((h + n) % RAW_BYTES), std::memory_order_release);
    return n;
}

/// Copies out without consuming: the decoder is told how much of what it was given was
/// actually used, and only that much may be dropped.
size_t raw_peek(uint8_t* dst, size_t n) {
    const size_t have = raw_used();
    if (n > have) n = have;
    if (!n) return 0;
    const uint32_t t = s.raw_tail.load(std::memory_order_relaxed);
    const size_t first = (t + n <= RAW_BYTES) ? n : (RAW_BYTES - t);
    memcpy(dst, s.raw + t, first);
    if (n > first) memcpy(dst + first, s.raw, n - first);
    return n;
}

void raw_drop(size_t n) {
    const size_t have = raw_used();
    if (n > have) n = have;
    const uint32_t t = s.raw_tail.load(std::memory_order_relaxed);
    s.raw_tail.store((uint32_t)((t + n) % RAW_BYTES), std::memory_order_release);
}

// ── the stream ────────────────────────────────────────────────────────────────────────────

esp_audio_simple_dec_type_t type_from_content(const char* ct, const char* url) {
    if (ct) {
        if (strstr(ct, "mpeg") || strstr(ct, "mp3"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        if (strstr(ct, "aacp") || strstr(ct, "aac"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        // FLAC before Ogg, because "audio/ogg; codecs=flac" contains both words and the Ogg
        // parser handles Vorbis and Opus only. Tested the other way round, an Ogg FLAC
        // station was handed to a parser that cannot read it and simply produced nothing.
        // Bare "application/ogg" carries no codec at all and still lands on Ogg below, which
        // is the right guess: Vorbis and Opus are what that type is usually used for.
        if (strstr(ct, "flac"))                       return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        if (strstr(ct, "ogg") || strstr(ct, "opus"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
        // WAV, which no radio station sends and a media server sends often. It was missing
        // here until the first track was pushed over DLNA (bench 18.09.2026): the type fell
        // through to the MP3 default, the MP3 decoder found nothing it recognised in a RIFF
        // header, and the log read "connected … playing" the whole way through four seconds
        // of silence. The ConnectionManager sink list advertises this type, so refusing to
        // decode it was a promise broken in the quietest possible way.
        if (strstr(ct, "wav") || strstr(ct, "wave"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        if (strstr(ct, "mp4") || strstr(ct, "m4a"))   return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    // Some servers answer with application/octet-stream and leave you to guess. A station's
    // URL rarely has a usable extension; a pushed track's URL usually does, because it is a
    // file on somebody's disk.
    if (url) {
        if (strstr(url, ".aac") || strstr(url, "aacp")) return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        if (strstr(url, ".wav"))                        return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
        if (strstr(url, ".flac"))                       return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
        if (strstr(url, ".m4a") || strstr(url, ".mp4")) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    }
    // And MP3 is still the overwhelming default for anything that says nothing useful.
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

/// Is this URL a playlist rather than a stream? Station directories hand out .m3u and .pls
/// far more often than they hand out the stream itself.
bool looks_like_playlist(const char* url, const char* ct) {
    if (ct && (strstr(ct, "mpegurl") || strstr(ct, "scpls") || strstr(ct, "x-scpls"))) return true;
    const char* q = strchr(url, '?');
    const size_t n = q ? (size_t)(q - url) : strlen(url);
    auto ends = [&](const char* ext) {
        const size_t e = strlen(ext);
        return n > e && !strncasecmp(url + n - e, ext, e);
    };
    return ends(".m3u") || ends(".m3u8") || ends(".pls");
}

/// Pulls the first stream URL out of an .m3u or .pls body.
///
/// Both formats are lists of lines; .pls writes `File1=http://...` and .m3u writes the URL
/// bare, with `#` for comments. Taking the FIRST entry is what a radio should do — the rest
/// are usually the same stream on other servers, and trying them in turn is the reconnect
/// path's job, not the tuning path's.
bool first_url_in_playlist(const char* body, size_t len, char* out, size_t out_sz) {
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && body[e] != '\n' && body[e] != '\r') e++;
        const char* line = body + i;
        size_t n = e - i;
        while (n && (line[0] == ' ' || line[0] == '\t')) { line++; n--; }
        if (n && line[0] != '#') {
            const char* http = nullptr;
            for (size_t k = 0; k + 4 < n; k++) {
                if (!strncasecmp(line + k, "http", 4)) { http = line + k; n -= k; break; }
            }
            if (http) {
                while (n && (http[n - 1] == ' ' || http[n - 1] == '\t')) n--;
                if (n < out_sz) {
                    memcpy(out, http, n);
                    out[n] = '\0';
                    return true;
                }
            }
        }
        i = e;
        while (i < len && (body[i] == '\n' || body[i] == '\r')) i++;
    }
    return false;
}

/// Splits a URL into the four things a request needs.
///
/// No general-purpose parser: a station URL is a scheme, a host, maybe a port and a path,
/// and everything else in the specification is somebody else's problem.
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

/// Get the reader out of the socket before the socket goes away.
///
/// The reader blocks inside esp_transport_read for up to READ_MS, and it is the DECODER
/// task that tears connections down — at a station change, at a stop, and now at every
/// seek. Destroying the transport under a blocked read frees the handle that read is
/// holding. It has not been seen to crash, because at a station change the reader is
/// between reads far more often than not; a seek makes the same teardown routine, which is
/// reason enough to stop relying on that.
///
/// Parking changes admission state before taking the reader's I/O mutex. A timed-out
/// socket read eventually releases it; buffers are never freed beneath an active read.
void park_reader() {
    if (!s.reader) return;
    s.state.store(ST_CONNECTING, std::memory_order_release);
    // The same mutex covers the reader's state check, read and ring publication.
    // Never destroy a transport while a blocked read still owns it.
    xSemaphoreTake(io_guard(), portMAX_DELAY);
    xSemaphoreGive(io_guard());
}

void http_close() {
    park_reader();
    if (s.tr) {
        esp_transport_close(s.tr);
        esp_transport_destroy(s.tr);
        s.tr = nullptr;
    }
    if (s.prepared) {
        radio_core::next_stream::destroy(s.prepared);
        s.prepared = nullptr;
    }
}

/// One line of the response, without its terminator. Returns false at the end of the buffer.
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

/// Opens `url`, reads the response head, and keeps whatever audio came with it.
///
/// Written against the socket rather than through esp_http_client. The reason that survives
/// checking is ICY METADATA, not the two that were written here before.
///
/// A station interleaves its "StreamTitle=" blocks into the audio every icy-metaint bytes,
/// and something has to pull them back out before the decoder sees the stream. Nothing in
/// Espressif's stack does that: esp_gmf_io_http.c and ESP-ADF's http_stream.c contain no
/// icy, metaint or StreamTitle at all. So the de-interleaving is ours either way, and once
/// it is ours the client underneath saves very little.
///
/// Two earlier claims are withdrawn, because leaving a wrong reason in a comment is worse
/// than leaving none. IDF's bundled parser DOES understand "ICY 200 OK": http_parser.c
/// carries states s_res_or_resp_I, s_res_I and s_res_IC, commented "for ICY URIs", and
/// drives them at the 'I' case of the status line. And the fetch_headers assert has a
/// documented one-call cure, esp_http_client_clear_response_buffer(), declared in
/// esp_http_client.h with a comment describing exactly the stale-cache case that fires it.
///
/// Parsing a response head is thirty lines, so this stays. docs/NETWORK.md, "Internet radio" asked
/// for the GET without a wrapper, and that decision is still the right one — for one reason
/// rather than three.
///
/// What one request/response exchange ended in. http_open() follows the redirects.
enum class Hop : uint8_t { FAILED, OPENED, REDIRECTED };

/// The server a transport is connected to, IPv4 in network order; 0 when not known.
uint32_t peer_ipv4(esp_transport_handle_t tr) {
    const int fd = tr ? esp_transport_get_socket(tr) : -1;
    struct sockaddr_in a = {};
    socklen_t len = sizeof(a);
    if (fd < 0 || getpeername(fd, reinterpret_cast<struct sockaddr*>(&a), &len) != 0 ||
        a.sin_family != AF_INET) {
        return 0;
    }
    return a.sin_addr.s_addr;
}

/// Another of `host`'s addresses than `avoid`, as dotted text. False when the host has no
/// other, or does not resolve: then the name is connected to as always. Needs the resolver to
/// keep more than one address per name (CONFIG_LWIP_DNS_MAX_HOST_IP).
bool other_address(const char* host, uint32_t avoid, char* out, size_t out_sz) {
    struct addrinfo hints = {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    uint32_t list[8];
    int n = 0;
    for (struct addrinfo* p = res; p && n < (int)(sizeof(list) / sizeof(list[0])); p = p->ai_next) {
        if (p->ai_family == AF_INET) list[n++] = reinterpret_cast<struct sockaddr_in*>(p->ai_addr)->sin_addr.s_addr;
    }
    freeaddrinfo(res);
    const int i = radio_core::pick_address(list, n, avoid);
    if (i < 0) return false;
    struct in_addr a;
    a.s_addr = list[i];
    return inet_ntoa_r(a, out, out_sz) != nullptr;
}

/// One exchange: connect, send the GET, read the head. On a 3xx the Location goes to `next`
/// and the connection is left for the next hop to close; `url` may be `next` itself, which
/// is safe because it is not read again once split_url() has taken it apart.
Hop http_open_hop(const char* url, char* ct, size_t ct_sz, int64_t range_from,
                  char* next, size_t next_sz) {
    ESP_LOGI(TAG, "opening %s", url);
    http_close();
    s.content_type[0] = '\0';
    s.icy_meta = 0;

    bool ssl = false;
    int  port = 0;
    char host[128], path[PLAY_URL_LEN];
    if (!split_url(url, ssl, host, sizeof(host), port, path, sizeof(path))) {
        ESP_LOGW(TAG, "not a URL this understands: %s", url);
        return Hop::FAILED;
    }

    s.tr = ssl ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!s.tr) return Hop::FAILED;
    s.tr_ssl = ssl;
    if (ssl) esp_transport_ssl_crt_bundle_attach(s.tr, esp_crt_bundle_attach);

    // A server the stream could not keep up with is left for another of the host's addresses
    // (stream_watch.h). Connected to by number, so the name goes into the request and into
    // the TLS handshake (SNI and the certificate check) instead.
    char addr[16] = "";
    const uint32_t avoid = s.avoid_ip.load(std::memory_order_acquire);
    if (avoid && other_address(host, avoid, addr, sizeof(addr))) {
        ESP_LOGI(TAG, "%s: another of its servers than the last, %s", host, addr);
        if (ssl) esp_transport_ssl_set_common_name(s.tr, host);
    }
    if (esp_transport_connect(s.tr, addr[0] ? addr : host, port, CONNECT_MS) < 0) {
        ESP_LOGW(TAG, "cannot reach %s:%d", addr[0] ? addr : host, port);
        http_close();
        return Hop::FAILED;
    }
    s.peer_ip.store(peer_ipv4(s.tr), std::memory_order_release);

    // A ranged GET is how a seek is done: nothing in an MP3 says where a second lives, so
    // the offset is computed from the length and asked for by byte. A station ignores the
    // header and answers 200 with its live stream, which is the right thing for it to do
    // and costs nothing here.
    char range[48] = "";
    if (range_from >= 0) {
        snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", (long long)range_from);
    }
    char req[512];
    const int rn = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s:%d\r\n"
                            "User-Agent: modesp-player/1.0\r\n"
                            "Icy-MetaData: 1\r\n"
                            "Accept: */*\r\n"
                            "%s"
                            "\r\n",
                            path, host, port, range);
    if (rn <= 0 || rn >= (int)sizeof(req) || esp_transport_write(s.tr, req, rn, CONNECT_MS) < rn) {
        ESP_LOGW(TAG, "request failed");
        http_close();
        return Hop::FAILED;
    }

    // Read until the blank line that ends the head. Whatever follows it in the same read is
    // already audio, and goes straight into the buffer rather than being thrown away.
    char* head = (char*)s.meta;                 // borrowed: the reader is not running yet
    constexpr size_t HEAD_MAX = 16 * 255;
    size_t have = 0, body_at = 0;
    while (have < HEAD_MAX) {
        const int r = esp_transport_read(s.tr, head + have, (int)(HEAD_MAX - have), CONNECT_MS);
        if (r <= 0) { ESP_LOGW(TAG, "no response head"); http_close(); return Hop::FAILED; }
        have += (size_t)r;
        head[have] = '\0';                  // meta has a dedicated terminator byte
        for (size_t i = 3; i < have; i++) {
            if (head[i] == '\n' && head[i - 1] == '\r' && head[i - 2] == '\n' && head[i - 3] == '\r') {
                body_at = i + 1;
                break;
            }
        }
        if (body_at) break;
    }
    if (!body_at) { ESP_LOGW(TAG, "response head too long"); http_close(); return Hop::FAILED; }

    const char* p = head;
    const char* end = head + body_at;
    const char* line = nullptr;
    size_t len = 0;
    if (!next_line(p, end, line, len)) { http_close(); return Hop::FAILED; }

    // "HTTP/1.1 200 OK" or "ICY 200 OK" — the second is what Shoutcast answers.
    int code = 0;
    if (const char* sp = (const char*)memchr(line, ' ', len)) code = atoi(sp + 1);
    if (code != 200 && code != 206 && !(code >= 300 && code < 400)) {
        ESP_LOGW(TAG, "station answered %d", code);
        http_close();
        return Hop::FAILED;
    }

    char location[PLAY_URL_LEN] = "";        // the next hop's size, not a station's
    bool sized = false, chunked = false, valid_range = false;
    uint64_t response_length = 0, range_size = 0;
    const uint64_t expected_total = s.body_bytes;

    while (next_line(p, end, line, len)) {
        if (!len) break;
        const char* v = nullptr;
        size_t vlen = 0;
        if (header_is(line, len, "Content-Type", v, vlen)) {
            copy_header(v, vlen, s.content_type, sizeof(s.content_type));
        } else if (header_is(line, len, "icy-metaint", v, vlen)) {
            char n[16];
            copy_header(v, vlen, n, sizeof(n));
            const int m = atoi(n);
            s.icy_meta = (m > 0 && m <= 64 * 1024) ? m : 0;
        } else if (header_is(line, len, "icy-br", v, vlen)) {
            char n[16];
            copy_header(v, vlen, n, sizeof(n));
            s.bitrate = (uint32_t)atoi(n);
        } else if (header_is(line, len, "icy-name", v, vlen)) {
            copy_header(v, vlen, s.icy_name, sizeof(s.icy_name));
        } else if (header_is(line, len, "Content-Length", v, vlen)) {
            char length_text[24];
            copy_header(v, vlen, length_text, sizeof(length_text));
            char* tail = nullptr;
            response_length = strtoull(length_text, &tail, 10);
            if (!length_text[0] || length_text[0] == '-' || !tail || *tail) { http_close(); return Hop::FAILED; }
            sized = true;
            // Only from a whole-entity response: on a ranged one this is what REMAINS, and
            // reading it as the length would make every seek shorten the track.
            if (range_from < 0) {
                char n[24];
                copy_header(v, vlen, n, sizeof(n));
                s.body_bytes = (uint64_t)strtoull(n, nullptr, 10);
            }
        } else if (header_is(line, len, "Content-Range", v, vlen)) {
            // "bytes 1500000-5999999/6000000" — the number after the slash is the length,
            // and a ranged response states it nowhere else.
            char n[48];
            copy_header(v, vlen, n, sizeof(n));
            unsigned long long start = 0, finish = 0, total_length = 0;
            if (sscanf(n, "bytes %llu-%llu/%llu", &start, &finish, &total_length) == 3 &&
                start <= finish && finish + 1 == total_length &&
                start == (range_from < 0 ? 0 : (uint64_t)range_from) &&
                (range_from < 0 || !expected_total || total_length == expected_total)) {
                valid_range = true; range_size = finish - start + 1;
            }
            if (const char* slash = strrchr(n, '/')) {
                const uint64_t total = (uint64_t)strtoull(slash + 1, nullptr, 10);
                if (total) s.body_bytes = total;
            }
        } else if (header_is(line, len, "Transfer-Encoding", v, vlen)) {
            if (vlen != 7 || strncasecmp(v, "chunked", 7)) { http_close(); return Hop::FAILED; }
            chunked = true;
        } else if (header_is(line, len, "Location", v, vlen)) {
            // Cut short, a signed redirect fails as a 403 or 404 from a server that looks
            // fine (see PLAY_URL_LEN), so one that does not fit is refused, with the reason
            // in the log (deep review 2026-10-12, RP-5).
            if (vlen >= sizeof(location) && code >= 300 && code < 400) {
                ESP_LOGW(TAG, "redirect target too long (%u bytes)", (unsigned)vlen);
                http_close();
                return Hop::FAILED;
            }
            copy_header(v, vlen, location, sizeof(location));
        }
    }

    if (code >= 300 && code < 400) {
        if (!location[0]) {
            ESP_LOGW(TAG, "redirect with nowhere to go");
            http_close();
            return Hop::FAILED;
        }
        snprintf(next, next_sz, "%s", location);
        return Hop::REDIRECTED;
    }

    if ((range_from >= 0 && (code != 206 || !valid_range)) ||
        (code == 206 && (!valid_range || (sized && response_length != range_size)))) {
        ESP_LOGW(TAG, "server did not honor the requested byte range");
        http_close(); return Hop::FAILED;
    }
    s.body.reset(sized, response_length, chunked);
    s.eof.store(false);
    s.decoder_drained = false;
    if (ct && ct_sz) snprintf(ct, ct_sz, "%s", s.content_type);
    s.icy_left = s.icy_meta;
    s.icy_remaining = -1;
    s.icy_have = 0;
    // The audio that came with the head. Counted against the ICY interval like any other
    // audio, because as far as the station is concerned that is what it is.
    s.head_len = have - body_at;
    if (s.head_len) memmove(s.meta, head + body_at, s.head_len);
    s.head_at = 0;
    // Which of the host's servers: a far one is what starves a stream (stream_watch.h).
    char server[16] = "?";
    if (const uint32_t ip = s.peer_ip.load(std::memory_order_acquire)) {
        struct in_addr x;
        x.s_addr = ip;
        inet_ntoa_r(x, server, sizeof(server));
    }
    ESP_LOGI(TAG, "connected: %s, icy-metaint %d, %lu kbit/s, %u bytes with the head, "
                  "length %llu, server %s",
             s.content_type[0] ? s.content_type : "(no type)", s.icy_meta,
             (unsigned long)s.bitrate, (unsigned)s.head_len,
             (unsigned long long)s.body_bytes, server);
    return Hop::OPENED;
}

/// `redirects` bounds the Location chain; station directories hand out URLs that move.
///
/// A loop, not recursion (04.10.2026). Each hop's frame holds the host, the path, the
/// request and the Location — about 1.6 KB — and it used to call itself for every redirect.
/// A CDN that answered with four of them (zeno.fm -> surfernetwork, http and https) took
/// the 12 KB "radio" stack past its end inside the "opening" log line, and the panel
/// rebooted on the stack guard (coredump, _local/bench/coredump-20261004-2323). Now the
/// stack holds one hop whatever the chain's length.
bool http_open(const char* url, char* ct, size_t ct_sz, int redirects = 4,
               int64_t range_from = -1) {
    char next[PLAY_URL_LEN];
    for (;;) {
        switch (http_open_hop(url, ct, ct_sz, range_from, next, sizeof(next))) {
        case Hop::OPENED:     return true;
        case Hop::FAILED:     return false;
        case Hop::REDIRECTED: break;
        }
        if (redirects-- <= 0) {
            ESP_LOGW(TAG, "too many redirects");
            http_close();
            return false;
        }
        url = next;
    }
}

/// Takes `StreamTitle='...';` out of an ICY metadata block.
void parse_icy(const char* block, size_t len) {
    static const char KEY[] = "StreamTitle=";
    for (size_t i = 0; i + sizeof(KEY) - 1 < len; i++) {
        if (strncmp(block + i, KEY, sizeof(KEY) - 1)) continue;
        size_t p = i + sizeof(KEY) - 1;
        if (p >= len) return;
        const char quote = block[p];
        if (quote != '\'' && quote != '"') return;
        p++;
        size_t e = p;
        // The value ends at the quote that is followed by a semicolon: a title may well
        // contain an apostrophe, and stations do not escape it.
        while (e < len && !(block[e] == quote && (e + 1 >= len || block[e + 1] == ';'))) e++;
        size_t n = e - p;
        if (n >= sizeof(s.now)) n = sizeof(s.now) - 1;
        if (!strncmp(s.now, block + p, n) && s.now[n] == '\0') return;   // unchanged
        memcpy(s.now, block + p, n);
        s.now[n] = '\0';
        s.now_gen.fetch_add(1, std::memory_order_release);
        ESP_LOGI(TAG, "now playing: %s", s.now);
        return;
    }
}

/// One read from the stream, taking the bytes that arrived with the response head first.
///
/// Those bytes are already audio and already counted by the station against its metadata
/// interval; handing them back here rather than dropping them is what keeps the very start
/// of a stream intact and the ICY offsets in step.
int wire_read(char* dst, size_t len) {
    if (s.head_at == s.head_len) {
        if (!s.tr) return -2;
        const int r = esp_transport_read(s.tr, s.meta, 16 * 255, READ_MS);
        // tcp_transport: zero is timeout, -1 is clean FIN. A positive 263 is DATA,
        // not ESP_ERR_TIMEOUT (which shares that numeric value).
        if (r <= 0) return r == -1 ? -1 : r == 0 ? 0 : -2;
        s.head_at = 0; s.head_len = (size_t)r;
    }
    const size_t left = s.head_len - s.head_at;
    const size_t n = len < left ? len : left;
    memcpy(dst, s.meta + s.head_at, n); s.head_at += n;
    return (int)n;
}
int sock_read(char* dst, int len) {
    return s.body.read(dst, (size_t)len, wire_read);
}

/// Reads one socket-read's worth of AUDIO into the compressed ring, consuming any ICY
/// metadata blocks on the way.
///
/// One read, not a filled buffer: at 128 kbit/s a 4 KB chunk takes a quarter of a second to
/// arrive, and waiting for all of it is a quarter of a second in which nothing is decoded.
/// The decoder is a separate task now, so this blocking is harmless — but it is still the
/// reason this returns as soon as the socket has given it anything.
///
/// Returns 0 on a clean end of stream and −1 on an error; the caller treats both the same,
/// because for a radio they are the same — the stream stopped and it has to come back.
int stream_read(uint8_t* dst, size_t max) {
    if (s.prepared) return radio_core::next_stream::read(s.prepared, dst, max);
    size_t got = 0;
    while (got == 0 && s.run) {
        if (s.icy_meta && s.icy_left == 0) {
            if (s.icy_remaining < 0) {
                uint8_t lenb = 0;
                const int r = sock_read((char*)&lenb, 1);
                if (r <= 0) return got ? (int)got : r;
                s.icy_remaining = (int)lenb * 16;
                s.icy_have = 0;
            }
            while (s.icy_remaining > 0) {
                const int r = sock_read(s.icy_buf + s.icy_have, s.icy_remaining);
                if (r <= 0) return got ? (int)got : r;
                s.icy_have += r;
                s.icy_remaining -= r;
            }
            if (s.icy_have) {
                s.icy_buf[s.icy_have] = '\0';
                parse_icy(s.icy_buf, (size_t)s.icy_have);
            }
            s.icy_remaining = -1;
            s.icy_have = 0;
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

// ── decoder to ring ───────────────────────────────────────────────────────────────────────

inline float edge_gain(float p) { return p * p * (3.0f - 2.0f * p); }

void ring_put(const float* stereo, size_t frames) {
    auto& ring = audio_core::pcm_ring();
    size_t written = 0;
    int spins = 0;
    while (written < frames && s.run) {
        const size_t w = ring.write(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation,
                                    stereo + written * 2, frames - written);
        written += w;
        if (w == 0) {
            sleep_ms(5);
            if (ring.owner() != audio_core::PcmRing::OWNER_RADIO ||
                ring.generation() != s.pcm_generation ||
                uxQueueMessagesWaiting(s.q)) return;
            // Pause is intentional backpressure. Keep this decoded block until
            // Resume instead of silently dropping it after two seconds. It is also progress
            // as far as the liveness watch goes: a DLNA pause past 30 s was reported as a
            // stuck task (deep review 2026-10-12, RP-7).
            if (ring.paused()) { modesp::liveness::tick(s.beat); spins = 0; continue; }
            // The consumer is not reading: either the source moved away from radio or the
            // audio core is gone. Either way, dropping these frames is right — holding them
            // would wedge this task forever.
            if (++spins > 200) return;
        }
    }
}

void push_frames(float* stereo, size_t frames) {
    s.dec_frames.fetch_add((uint32_t)frames, std::memory_order_relaxed);
    audio_core::fade_start(stereo, frames, s.fade_in_left, EDGE);
    ring_put(stereo, frames);
}

void close_stream() {
    http_close();
    s.head_len = 0;
    s.head_at = 0;
    raw_clear();
    s.lost.store(false, std::memory_order_release);
    s.eof.store(false);
    s.decoder_drained = false;
    s.first_pcm_size = 0;
    if (s.dec) { esp_audio_simple_dec_close(s.dec); s.dec = nullptr; }
    s.src_rate = 0;
    s.direct = false;
    s.clock = 0;
    s.wav = false;
    s.src_bits = 0;
    s.src_ch = 0;
    s.icy_meta = 0;
    s.icy_left = 0;
    s.icy_remaining = -1;
    s.icy_have = 0;
    s.body_bytes = 0;
    s.bitrate_bps = 0;
    s.frames_in.store(0, std::memory_order_release);
    s.dur_s.store(0, std::memory_order_release);
    s.fmt[0] = '\0';
}

/// Resolve the station URL, open the stream and the decoder. False means try again later.
bool tune(const char* want_url) {
    close_stream();
    snprintf(s.url, sizeof(s.url), "%s", want_url);
    snprintf(s.asked, sizeof(s.asked), "%s", want_url);
    s.state.store(ST_CONNECTING, std::memory_order_release);

    char ct[64] = "";
    if (!http_open(s.url, ct, sizeof(ct))) return false;

    if (looks_like_playlist(s.url, ct)) {
        // Read the little file, take the first stream out of it, and start again. Bounded:
        // a playlist is a few hundred bytes and anything claiming to be one but running to
        // megabytes is not worth following.
        char body[2048];
        int have = 0;
        while (have < (int)sizeof(body) - 1) {
            const int r = sock_read(body + have, (int)sizeof(body) - 1 - have);
            if (r <= 0) break;
            have += r;
        }
        body[have > 0 ? have : 0] = '\0';
        http_close();

        char inner[radio_core::URL_LEN];
        if (have <= 0 || !first_url_in_playlist(body, (size_t)have, inner, sizeof(inner))) {
            ESP_LOGW(TAG, "playlist %s had no stream in it", s.url);
            return false;
        }
        ESP_LOGI(TAG, "playlist -> %s", inner);
        snprintf(s.url, sizeof(s.url), "%s", inner);
        ct[0] = '\0';
        if (!http_open(s.url, ct, sizeof(ct))) return false;
    }

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type_from_content(ct[0] ? ct : nullptr, s.url);
    s.wav = cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    // HE-AAC is off unless asked for (esp_aac_dec.h: aac_plus_enable defaults to false), and
    // without it an "AAC+" station plays its AAC-LC core only: half the sample rate, and mono
    // where the stereo was carried as PS. That was one station in seven in Radio Browser on
    // 04.10.2026. A station is ADTS; a track pushed over DLNA can be M4A, which has its own
    // config with the same switch. The cost is small: the SBR path's deepest static call
    // chain is about 1.5 KB of stack, under FLAC's 4 KB on this same task, and the decoder's
    // state goes to PSRAM like the rest (malloc above 1 KB). The decoder copies the struct.
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_m4a_dec_cfg_t m4a = {};          // track_idx 0: the first audio track, as before
    aac.aac_plus_enable = m4a.aac_plus_enable = true;
    if (cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_AAC) {
        cfg.dec_cfg  = &aac;
        cfg.cfg_size = sizeof(aac);
    } else if (cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_M4A) {
        cfg.dec_cfg  = &m4a;
        cfg.cfg_size = sizeof(m4a);
    }
    if (esp_audio_simple_dec_open(&cfg, &s.dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed for %s", ct);
        http_close();
        return false;
    }
    ESP_LOGI(TAG, "stream heap: internal %u, DMA %u (largest %u); stack free %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    s.fade_in_left = EDGE;
    s.state.store(ST_BUFFERING, std::memory_order_release);
    return true;
}

// Keep the queued tail at its current rate and append a prepared first block.
// A different rate or an unavailable prefix falls back to a drained transition.
bool promote_prepared() {
    if (!s.one_shot.load() || audio_core::pcm_ring().paused()) return false;
    auto* p = radio_core::next_stream::take(s.track_generation.load(), s.src_rate);
    if (!p) return false;
    char ended[PLAY_URL_LEN];
    snprintf(ended, sizeof(ended), "%s", s.asked);
    const bool direct = s.direct;
    const uint32_t clock = s.clock;
    close_stream();
    s.prepared = p;
    s.dec = p->decoder;
    p->decoder = nullptr;
    snprintf(s.url, sizeof(s.url), "%s", p->uri);
    snprintf(s.asked, sizeof(s.asked), "%s", p->uri);
    s.src_rate = p->info.sample_rate;
    s.src_bits = p->info.bits_per_sample;
    s.src_ch = p->info.channel ? p->info.channel : 2;
    s.direct = direct;
    s.clock = clock;
    s.wav = type_from_content(nullptr, s.url) == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    s.body_bytes = p->total;
    s.bitrate_bps = audio_core::stream_bps(p->info.bitrate, s.src_rate, s.src_ch, s.src_bits);
    s.bitrate = s.bitrate_bps / 1000;
    s.dur_s.store(audio_core::stream_duration_s(s.body_bytes, s.bitrate_bps));
    snprintf(s.fmt, sizeof(s.fmt), "%.1f kHz %u ch %lu kbps",
             (double)s.src_rate / 1000.0, (unsigned)s.src_ch, (unsigned long)s.bitrate);
    raw_write(p->prefix + p->consumed, p->size - p->consumed);
    memcpy(s.pcm, p->pcm, p->pcm_size);
    s.first_pcm_size = p->pcm_size;
    s.eof.store(p->complete);
    s.fade_in_left = 0;
    s.resumes = 0;
    s.frames_in.store(0);
    s.track_generation.fetch_add(1);
    audio_core::pcm_ring().set_eos(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation, false);
    s.state.store(ST_PLAYING);
    portENTER_CRITICAL(&end_mux);
    snprintf(completed_uri, sizeof(completed_uri), "%s", ended);
    snprintf(promoted_uri, sizeof(promoted_uri), "%s", s.asked);
    ++completed_sequence;
    portEXIT_CRITICAL(&end_mux);
    ESP_LOGI(TAG, "prepared handoff: %s, %u source Hz; PCM tail retained", s.url, (unsigned)s.src_rate);
    return true;
}

enum class Pump { WORKED, IDLE, LOST, FINISHED };

/// Feed the ring at `out`: straight through when it is the stream's own rate, converted when
/// not. False when there is no memory for the converter.
bool use_clock(uint32_t out) {
    s.clock = out;
    s.direct = (out == s.src_rate);
    return s.direct || s.conv.ensure(s.src_rate, out);
}

/// One turn of the decoder's wheel. The network is somebody else's job now — this only ever
/// looks at the compressed ring, so a slow station cannot stall it.
Pump pump() {
    s.pump_calls.fetch_add(1, std::memory_order_relaxed);
    auto& ring = audio_core::pcm_ring();

    if (s.decoder_drained) {
        if (promote_prepared()) return Pump::WORKED;
        ring.set_eos(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation, true);
        return ring.available() == 0 ? Pump::FINISHED : Pump::IDLE;
    }
    // Hold back until there is a cushion. A stream that starts the moment the first bytes
    // arrive stutters on the first hiccup, and the first hiccup is usually immediate.
    if (s.state.load(std::memory_order_acquire) == ST_BUFFERING) {
        if (raw_used() < PRIME_BYTES && !s.eof.load()) { s.pump_idle.fetch_add(1, std::memory_order_relaxed); return Pump::IDLE; }
        s.state.store(ST_PLAYING, std::memory_order_release);
        s.retry = 0;
        ESP_LOGI(TAG, "playing");
    }
    if (ring.space() < CHUNK) { s.pump_idle.fetch_add(1, std::memory_order_relaxed); return Pump::IDLE; }

    // ── decode one call's worth ──
    const size_t have = raw_peek(s.feed, NET_CHUNK);
    if (!s.first_pcm_size && have < 512 && !s.eof.load()) { s.pump_idle.fetch_add(1, std::memory_order_relaxed); return Pump::IDLE; }          // not enough to be a frame; wait for the reader

    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = s.feed;
    raw.len    = (uint32_t)have;
    // HTTP may finish while many compressed blocks remain in the raw ring.
    // Only its final block is the decoder's end of input.
    raw.eos = s.eof.load(std::memory_order_acquire) && have == raw_used();
    esp_audio_simple_dec_out_t out = {};
    out.buffer = s.pcm;
    out.len    = (uint32_t)s.pcm_cap;

    const int64_t t_dec = esp_timer_get_time();
    esp_audio_err_t err = ESP_AUDIO_ERR_OK;
    if (s.first_pcm_size) {
        out.decoded_size = s.first_pcm_size;
        s.first_pcm_size = 0;
    } else err = esp_audio_simple_dec_process(s.dec, &raw, &out);
    s.dec_us.fetch_add((uint32_t)(esp_timer_get_time() - t_dec), std::memory_order_relaxed);
    if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        if (s.eof.load() && have == 0) { s.decoder_drained = true; return Pump::IDLE; }
        // A broken frame in a stream is not a reason to hang up: drop a little and resync,
        // which is what the decoder does after any dropout.
        raw_drop(have < 512 ? have : 512);
        return Pump::WORKED;
    }
    raw_drop(raw.consumed);
    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        // The frame's PCM does not fit, and the decoder keeps it until it does. Read as
        // "nothing decoded yet" it was asked again with the same buffer for ever, in silence
        // (deep review 2026-10-12, RP-4). Grow as asked and try again, or give the track up.
        if (out.needed_size > s.pcm_cap && out.needed_size <= PCM_MAX) {
            if (void* p = heap_caps_realloc(s.pcm, out.needed_size,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
                s.pcm = (uint8_t*)p;
                s.pcm_cap = out.needed_size;
                ESP_LOGI(TAG, "PCM buffer grown to %u B", (unsigned)s.pcm_cap);
                return Pump::WORKED;
            }
        }
        ESP_LOGW(TAG, "decoded frame needs %u B, cannot hold it", (unsigned)out.needed_size);
        return Pump::FINISHED;
    }
    if (!out.decoded_size) {
        if (s.eof.load() && !raw.consumed) {
            if (have) { ESP_LOGW(TAG, "undecodable stream tail"); return Pump::LOST; }
            s.decoder_drained = true;
        }
        return raw.consumed ? Pump::WORKED : Pump::IDLE;
    }

    if (!s.src_rate) {
        esp_audio_simple_dec_info_t info = {};
        if (esp_audio_simple_dec_get_info(s.dec, &info) == ESP_AUDIO_ERR_OK && info.sample_rate) {
            s.src_rate = info.sample_rate;
            s.src_bits = info.bits_per_sample;
            s.src_ch   = info.channel ? info.channel : 2;
            if (info.bitrate) {
                s.bitrate = info.bitrate < 10000 ? info.bitrate : info.bitrate / 1000;
            }
            // How long it is, when the server said how big it is. A station sends no
            // Content-Length and this stays zero, which is the honest answer for something
            // with no end; a media server pushing a track over DLNA always sends one.
            // The arithmetic — including the kilobits trap the card player was bitten by —
            // is in components/audio_pipe, one copy for both paths.
            s.bitrate_bps = audio_core::stream_bps(info.bitrate, s.src_rate, s.src_ch,
                                                   s.src_bits);
            s.bitrate = s.bitrate_bps / 1000;
            s.dur_s.store(audio_core::stream_duration_s(s.body_bytes, s.bitrate_bps),
                          std::memory_order_release);
            snprintf(s.fmt, sizeof(s.fmt), "%.1f kHz %u ch %lu kbps",
                     (double)s.src_rate / 1000.0, (unsigned)s.src_ch,
                     (unsigned long)s.bitrate);
            // Ask for the stream's own rate and wait to hear whether it was granted.
            // Converting cost 800 ms of CPU per second of audio before the phase path was
            // taken off soft-float; it is far cheaper now, but still not free — thirty-five
            // times the MP3 decoder — and no ESP32 radio worth copying does it; but a rate
            // the PLL cannot make still has to be converted, and only the consumer knows.
            auto& ring = audio_core::pcm_ring();
            ring.ask_rate(s.src_rate);
            uint32_t out = 0;
            for (int i = 0; i < 60 && !(out = ring.out_rate()); i++) sleep_ms(5);
            if (!out) out = audio_core::PIPE_OUT_RATE;   // no answer yet; start on the fallback
            // Both sides of the merge belong here: the converter is told which rate the
            // clock actually settled on, and a pushed track says how long it is.
            if (!use_clock(out)) return Pump::LOST;
            const int32_t dur = s.dur_s.load(std::memory_order_acquire);
            if (dur > 0) {
                ESP_LOGI(TAG, "stream: %s, %ld s (%llu bytes)", s.fmt, (long)dur,
                         (unsigned long long)s.body_bytes);
            } else {
                ESP_LOGI(TAG, "stream: %s, no end", s.fmt);
            }
        } else {
            return Pump::WORKED;
        }
    }

    // The answer the wait above gave up on, or a later one: convert for the clock the output
    // is really at, not the fallback the stream started on (audio_core::clock_moved()).
    const uint32_t answer = audio_core::pcm_ring().out_rate();
    if (audio_core::clock_moved(s.clock, answer)) {
        ESP_LOGW(TAG, "output at %lu Hz, not the %lu assumed; converting for it now",
                 (unsigned long)answer, (unsigned long)s.clock);
        if (!use_clock(answer)) return Pump::LOST;
    }

    const size_t bps = (size_t)(s.src_bits / 8) * s.src_ch;
    const size_t total = bps ? out.decoded_size / bps : 0;
    size_t done = 0;
    // A low-bitrate station at 22.05 or 24 kHz makes more frames than it is given, and
    // rbuf is a fixed CHUNK * 2 — see RateConverter::max_in().
    const size_t max_in = s.conv.max_in(CHUNK * 2);
    const size_t in_cap = max_in < CHUNK ? max_in : CHUNK;
    while (done < total) {
        const size_t n = audio_core::pcm_to_f32(s.pcm + done * bps, (total - done) * bps,
                                                s.src_bits, s.src_ch, s.fbuf, in_cap);
        if (!n) break;
        done += n;
        size_t made = n;
        const float* src = s.fbuf;
        if (!s.direct) {
            const int64_t t_cv = esp_timer_get_time();
            src = s.conv.process(s.fbuf, n, s.rbuf, CHUNK * 2, made);
            s.conv_us.fetch_add((uint32_t)(esp_timer_get_time() - t_cv), std::memory_order_relaxed);
        }
        push_frames(const_cast<float*>(src), made);
    }
    // The position, counted in SOURCE frames: `done` is what the decoder produced at the
    // stream's own rate, before any resampling, so the number means the same thing whether
    // or not the output clock happened to match.
    if (done) s.frames_in.fetch_add(done, std::memory_order_relaxed);
    return Pump::WORKED;
}

/// Jump to `sec` in the track that is playing.
///
/// Only a track: a station has no length, so there is no fraction to take of it. The offset
/// is the same fraction of the bytes as of the seconds (audio_pipe.h says why that is close
/// enough), and the connection is made again from there with a Range header.
///
/// The decoder is NOT reopened, for the reason the card player does not reopen it either:
/// for a container like WAV the header has already been read and reopening would lose it,
/// and for everything else resynchronising at the next frame header is cheaper than parsing
/// the file from the top — and is what the decoder does after any dropout anyway.
bool seek_stream(int32_t sec) {
    // A WAV byte fraction can land in RIFF/JUNK headers or between PCM frames.
    // Until this sequential HTTP transport has a verified data layout, leave the
    // current stream untouched and report seeking unavailable.
    if (s.wav) return false;
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
    auto& ring = audio_core::pcm_ring();
    if (!ring.request_flush(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation)) return false;
    for (int i = 0; i < audio_core::FLUSH_WAIT_NAPS && ring.flush_pending(); ++i) sleep_ms(5);
    if (ring.flush_pending()) ring.reset(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation);
    if (ring.owner() != audio_core::PcmRing::OWNER_RADIO) return false;
    s.pcm_generation = ring.generation();

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

void stop_station();
void handle_seek(int32_t sec) {
    if (!seek_stream(sec) && !s.tr) stop_station();
}

// ── the list ──────────────────────────────────────────────────────────────────────────────

/// Scratch for the station file, from PSRAM and only while it is being read or written.
///
/// It used to be two `static char buf[8192]`, i.e. 16 KB of internal .bss held for the life
/// of the firmware so that a file could be parsed twice on a boot and once per preset save.
/// Not a stack buffer: this runs on the main task, whose stack is 8 KB.
struct StationScratch {
    char*  p = nullptr;
    size_t n = 0;
    explicit StationScratch(size_t bytes)
        : p((char*)heap_caps_calloc_prefer(1, bytes, 2,
                                           MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
          n(p ? bytes : 0) {}
    ~StationScratch() { if (p) heap_caps_free(p); }
    StationScratch(const StationScratch&) = delete;
    StationScratch& operator=(const StationScratch&) = delete;
};
/// 64 stations with long URLs and a uuid each come to ~22 KB; 8 KB held the list before
/// uuids and was already near full with 64 entries. PSRAM, see StationScratch.
constexpr size_t STATIONS_JSON_MAX = 24 * 1024;

void save_stations() {
    StationScratch buf(STATIONS_JSON_MAX);
    if (!buf.p) { ESP_LOGW(TAG, "no memory to write the station list"); return; }
    radio_core::stations().copy(s.list);
    const size_t w = radio_core::serialize(s.list, buf.p, buf.n);
    if (!w) { ESP_LOGW(TAG, "station list too large to write"); return; }
    // write -> read back -> rename (safe_file, plan item 11.2). The old fopen/fwrite left a
    // truncated list on a power cut, and a truncated list is not a smaller list: radio_core
    // ::parse rejects it, and load_stations() then overwrites it with the built-in defaults.
    // Someone's stations were one badly timed reset away from being replaced by ours.
    if (!safe_file::save(s_stations_path, buf.p, w)) {
        ESP_LOGW(TAG, "cannot write %s — the previous list is still there", s_stations_path);
        return;
    }
    ESP_LOGI(TAG, "station list saved");
}

void load_stations() {
    // Where the list lives is decided here, once, and every later read and write uses the
    // answer. UserStore::begin() has already run by now — ConfigService brings it up while
    // it mounts, and config is CRITICAL, so it is done before any module inits.
    //
    // The scratch stays the PSRAM one from item 7.3. The persistence work arrived with a
    // `static char buf[8192]` here, which is the 8 KB of .bss that item deleted; taking it
    // back would have undone half that stage without a compiler saying a word.
    modesp::UserStore::write_path("stations.json", s_stations_path, sizeof(s_stations_path));
    StationScratch buf(STATIONS_JSON_MAX);
    size_t n = 0;
    if (buf.p) {
        if (FILE* f = fopen(s_stations_path, "rb")) {
            n = fread(buf.p, 1, buf.n - 1, f);
            fclose(f);
        }
    }
    if (n == 0 || !radio_core::parse(buf.p, n, s.list)) {
        ESP_LOGI(TAG, "no usable %s — starting from the built-in list", s_stations_path);
        radio_core::defaults(s.list);
        const size_t w = buf.p ? radio_core::serialize(s.list, buf.p, buf.n) : 0;
        if (w) safe_file::save(s_stations_path, buf.p, w);
    }
    // Hand it to the store the screen reads from; the list itself stays here, and the task
    // that tunes copies a URL out of it under list_guard() (RP-6).
    radio_core::stations().set(s.list);
    ESP_LOGI(TAG, "%d stations", s.list.count);
}

/// Modem sleep off while a station plays, and back on when it stops.
///
/// The default parks the Wi-Fi radio between beacons and lets the access point buffer what
/// arrives in between, which on this co-processor link shows up as loss rather than
/// latency — 50% of pings dropped while a stream ran, and TCP down to a few hundred bytes
/// a second (bench 17.09.2026). Keeping it off ALL the time is the wrong trade, though: it
/// costs the co-processor continuous work on the same core the card player decodes on, and
/// the card player must not pay for a feature it is not using.
void set_modem_sleep(bool allow) {
    static int8_t applied = -1;
    const int8_t want = allow ? 1 : 0;
    if (want == applied) return;
    const esp_err_t e = esp_wifi_set_ps(allow ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (e == ESP_OK) {
        applied = want;
        ESP_LOGI(TAG, "modem sleep %s", allow ? "on" : "off");
    }
}

// Manual changes must close the consumer's envelope before replacing a stream.
// claim(OWNER_RADIO) deliberately preserves the queue for same-owner/gapless
// transitions; it cannot provide the fade needed by an explicit Next/Stop.
// Match the card player's close_track() handshake, without changing natural EOF.
void fade_queued_audio() {
    auto& ring = audio_core::pcm_ring();
    if (ring.owner() != audio_core::PcmRing::OWNER_RADIO) return;
    const uint32_t generation = ring.generation();
    if (!ring.request_flush(audio_core::PcmRing::OWNER_RADIO, generation)) return;
    for (int i = 0; i < audio_core::FLUSH_WAIT_NAPS && ring.flush_pending(); ++i) sleep_ms(5);
    if (ring.flush_pending()) {
        // Same bounded fallback as the file player when no consumer acknowledges.
        ESP_LOGW(TAG, "manual transition: PCM fade acknowledgement timed out");
        ring.reset(audio_core::PcmRing::OWNER_RADIO, generation);
    }
    if (ring.owner() == audio_core::PcmRing::OWNER_RADIO)
        s.pcm_generation = ring.generation();
}

/// The station at `idx` — the list's, or the catalog's when idx is TRANSIENT. Null if none.
const radio_core::Station* station_at(int idx) {
    if (idx == TRANSIENT) return s.transient.valid() ? &s.transient : nullptr;
    return s.list.at(idx);
}

/// The URL of the station at `idx`, copied while the list cannot change. A pointer into the
/// list was held through the fade and the old stream's close — a second or more — and a web
/// edit in that time put another station's URL under it (deep review 2026-10-12, RP-6).
/// False if there is no such station. Radio task.
bool station_url(int idx, char* out, size_t n) {
    xSemaphoreTake(list_guard(), portMAX_DELAY);
    const radio_core::Station* st = station_at(idx);
    if (st) snprintf(out, n, "%s", st->url);
    xSemaphoreGive(list_guard());
    return st != nullptr;
}

void start_station(int idx) {
    s.track_generation.fetch_add(1);
    char url[sizeof(radio_core::Station::url)];
    if (!station_url(idx, url, sizeof(url))) return;
    // Already on it and playing: leave it alone. Otherwise a second request for the same
    // station — the screen sends one, the state change sends another — would drop a working
    // stream and start it again, which is heard as the radio refusing to start.
    const uint8_t st = s.state.load(std::memory_order_acquire);
    if (idx == s.tuned.load(std::memory_order_acquire) &&
        (st == ST_BUFFERING || st == ST_PLAYING)) {
        return;
    }
    // A server left for being too slow is left for that station only (stream_watch.h).
    s.avoid_ip.store(0, std::memory_order_release);
    s.station.store(idx, std::memory_order_release);
    s.tuned.store(idx, std::memory_order_release);
    s.one_shot.store(false, std::memory_order_release);
    s.now[0] = '\0';
    s.now_gen.fetch_add(1, std::memory_order_release);
    s.retry = 0;
    fade_queued_audio();
    set_modem_sleep(false);
    // From the card or AirPlay, whose music may still be playing: faded, not cut (pcm_ring.h).
    audio_core::claim_after_fade(audio_core::pcm_ring(), audio_core::PcmRing::OWNER_RADIO,
                                 [] { sleep_ms(5); });
    s.pcm_generation = audio_core::pcm_ring().generation();
    audio_core::pcm_ring().set_paused(false);
    audio_core::pcm_ring().set_eos(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation, false);
    if (!s.net_up.load(std::memory_order_acquire)) {
        // Asked for before the network is up: only the connecting waits for a route, and
        // the reconnect path does it as soon as there is one. Everything above must happen
        // now — that path only tunes, and a station tuned into a ring the radio had not
        // claimed played silence while it said "playing", and a push's one_shot left set
        // stopped it outright.
        close_stream();
        s.state.store(ST_RECONNECTING, std::memory_order_release);
        return;
    }
    if (!tune(url)) {
        s.state.store(ST_RECONNECTING, std::memory_order_release);
    }
}

void stop_station() {
    s.track_generation.fetch_add(1);
    s.tuned.store(-1, std::memory_order_release);
    radio_core::next_stream::request(s.track_generation.load(), "");
    fade_queued_audio();
    close_stream();
    set_modem_sleep(true);
    s.state.store(ST_IDLE, std::memory_order_release);
    audio_core::pcm_ring().set_eos(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation, true);
    audio_core::pcm_ring().release(audio_core::PcmRing::OWNER_RADIO);
}

/// Play one URL that is in nobody's list — pushed by a control point over DLNA, or typed
/// into the web UI.
///
/// Everything a station gets, this gets too: the same socket, the same decoder, the same
/// ring. What it does not get is the reconnect loop, because this one is allowed to end
/// (see RadioTaskState::one_shot). A failed connect therefore stops rather than retrying
/// forever against a URL that may only have been valid for one request.
void start_url(const char* url) {
    s.track_generation.fetch_add(1);
    if (!url || !*url) return;
    if (!s.net_up.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "asked to play a URL with no network");
        return;
    }
    s.one_shot.store(true, std::memory_order_release);
    s.tuned.store(-1, std::memory_order_release);      // not a list station: PLAY must retune
    s.frames_in.store(0, std::memory_order_release);
    s.resumes = 0;
    s.now[0] = '\0';
    s.now_gen.fetch_add(1, std::memory_order_release);
    s.retry = 0;
    fade_queued_audio();
    set_modem_sleep(false);
    audio_core::claim_after_fade(audio_core::pcm_ring(), audio_core::PcmRing::OWNER_RADIO,
                                 [] { sleep_ms(5); });
    s.pcm_generation = audio_core::pcm_ring().generation();
    audio_core::pcm_ring().set_paused(false);
    audio_core::pcm_ring().set_eos(audio_core::PcmRing::OWNER_RADIO, s.pcm_generation, false);
    ESP_LOGI(TAG, "playing pushed URL %s", url);
    if (!tune(url)) stop_station();
}

/// How long to wait before trying again. Quick at first, then backing off to fifteen
/// seconds — and never giving up, because a radio that has to be restarted by hand after
/// the router reboots is not a radio.
uint32_t backoff_ms(int retry) {
    static const uint32_t STEPS[] = {500, 1000, 2000, 4000, 8000, 15000};
    const int n = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
    return STEPS[retry < n ? retry : n - 1];
}

/// Recover a failed finite stream with a ranged GET. Natural completion is
/// reported separately, only after HTTP EOF, decoder flush and PCM drain.
/// Estimated duration must never turn a truncated transfer into success.
void finish_or_resume() {
    if (s.wav) {
        ESP_LOGW(TAG, "WAV stream interrupted; no verified data range for resume");
        stop_station();
        return;
    }
    const uint32_t sr  = s.src_rate;
    const int32_t  dur = s.dur_s.load(std::memory_order_acquire);
    const int32_t  pos = sr ? (int32_t)(s.frames_in.load(std::memory_order_acquire) / sr) : 0;

    if (!s.body_bytes || dur <= 0 || s.resumes >= RESUME_TRIES) {
        if (s.body_bytes && dur > 0) {
            ESP_LOGW(TAG, "giving up at %ld s of %ld after %d attempts",
                     (long)pos, (long)dur, s.resumes);
        } else {
            ESP_LOGW(TAG, "cannot resume failed stream at %ld s of %ld", (long)pos, (long)dur);
        }
        stop_station();
        return;
    }

    // A failed connection is still a recoverable dropout. Previously the first
    // connect timeout called stop_station(), making RESUME_TRIES ineffective.
    // Bound a recovery episode as well as the total attempts per track. Check
    // commands between attempts so Stop/new URL can supersede this recovery.
    constexpr int CONNECT_ATTEMPTS = 3;
    for (int attempt = 0; attempt < CONNECT_ATTEMPTS && s.resumes < RESUME_TRIES; ++attempt) {
        if (!s.run || uxQueueMessagesWaiting(s.q)) return;
        if (attempt) {
            for (int waited = 0; waited < 500; waited += 50) {
                if (!s.run || uxQueueMessagesWaiting(s.q)) return;
                sleep_ms(50);
            }
        }
        ++s.resumes;
        ESP_LOGW(TAG, "stream broke at %ld s of %ld — picking it up again (attempt %d)",
                 (long)pos, (long)dur, s.resumes);
        if (seek_stream(pos)) return;
    }
    ESP_LOGW(TAG, "recovery failed at %ld s after %d total attempts", (long)pos, s.resumes);
    stop_station();
}

/// The reader: one blocking socket read at a time into the compressed ring, and nothing
/// else. Shares the decoder and main-loop priority; a full buffer blocks this task.
void reader_task(void*) {
    uint8_t* chunk = s.net_chunk;
    int quiet = 0;
    while (s.run) {
        modesp::liveness::tick(s.reader_beat);
        xSemaphoreTake(io_guard(), portMAX_DELAY);
        const uint8_t st = s.state.load(std::memory_order_acquire);
        if ((st != ST_BUFFERING && st != ST_PLAYING) || (!s.tr && !s.prepared) || s.eof.load() || s.lost.load() || raw_space() < NET_CHUNK) {
            xSemaphoreGive(io_guard()); sleep_ms(20); continue;
        }
        s.in_read.store(true, std::memory_order_release);
        const int r = stream_read(chunk, NET_CHUNK);
        if (r > 0) { quiet = 0; raw_write(chunk, (size_t)r); s.net_bytes.fetch_add(r); }
        else if (r == radio_core::HttpBody::END) { quiet = 0; s.eof.store(true, std::memory_order_release); }
        else if (r < 0 || ++quiet >= READ_GIVE_UP) s.lost.store(true, std::memory_order_release);
        s.in_read.store(false, std::memory_order_release);
        xSemaphoreGive(io_guard());
    }
    s.reader = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

/// Bound each batch of decode work even though main can now preempt it through
/// equal-priority time slicing. Blocking also leaves time for lower-priority work.
constexpr int64_t WORK_BUDGET_US = 30000;

void radio_task(void*) {
    CmdMsg m;
    int64_t yield_at = 0;
    while (s.run) {
        modesp::liveness::tick(s.beat);
        const uint8_t st = s.state.load(std::memory_order_acquire);
        const bool busy = (st == ST_BUFFERING || st == ST_PLAYING);
        if (xQueueReceive(s.q, &m, pdMS_TO_TICKS(busy ? 0 : 50)) == pdTRUE) {
            switch (m.type) {
            case Cmd::PLAY: start_station(s.station.load()); break;
            case Cmd::STOP: stop_station(); break;
            case Cmd::TUNE: start_station(m.value); break;
            // Nothing sends NEXT/PREV since RP-1: on_update resolves the neighbour on main,
            // publishes radio.station and sends TUNE. Resolving it here left radio.station
            // behind, and main's pick check tuned straight back.
            case Cmd::NEXT:
                // From a catalog station, the list starts at its first.
                if (s.list.count) {
                    const int i = s.station.load();
                    start_station(i >= 0 ? (i + 1) % s.list.count : 0);
                }
                break;
            case Cmd::PREV:
                if (s.list.count) {
                    const int i = s.station.load();
                    start_station(i > 0 ? i - 1 : s.list.count - 1);
                }
                break;
            case Cmd::URL: start_url(m.uri); break;
            case Cmd::CATALOG: {
                // A new catalog station replaces the last one: the URL changed even though the
                // index did not, so "already on it" must not answer for it. A catalog row's
                // URL fits a station's; one that does not would be a broken URL, not a short one.
                const size_t n = strnlen(m.uri, sizeof(m.uri));
                if (n >= sizeof(s.transient.url)) {
                    ESP_LOGW(TAG, "catalog URL too long for a station (%u bytes)", (unsigned)n);
                    break;
                }
                xSemaphoreTake(list_guard(), portMAX_DELAY);
                memcpy(s.transient.url, m.uri, n + 1);
                xSemaphoreGive(list_guard());
                s.station.store(TRANSIENT, std::memory_order_release);
                s.tuned.store(-1, std::memory_order_release);
                start_station(TRANSIENT);
                break;
            }
            case Cmd::SEEK: handle_seek(m.value); break;
            }
        }
        if (!s.run) break;

        const uint8_t now_st = s.state.load(std::memory_order_acquire);
        if (now_st == ST_BUFFERING || now_st == ST_PLAYING) {
            if (s.lost.load(std::memory_order_acquire)) {
                s.lost.store(false, std::memory_order_release);
                if (s.one_shot.load(std::memory_order_acquire)) {
                    finish_or_resume();
                    continue;
                }
                ESP_LOGW(TAG, "stream lost after %s", s.fmt[0] ? s.fmt : "no audio");
                close_stream();
                s.state.store(ST_RECONNECTING, std::memory_order_release);
                continue;
            }
            if (!yield_at) yield_at = esp_timer_get_time() + WORK_BUDGET_US;
            switch (pump()) {
            case Pump::FINISHED:
                if (s.one_shot.load()) {
                    portENTER_CRITICAL(&end_mux);
                    snprintf(completed_uri, sizeof(completed_uri), "%s", s.asked);
                    promoted_uri[0] = 0;
                    ++completed_sequence;
                    portEXIT_CRITICAL(&end_mux);
                    ESP_LOGI(TAG, "track drained to end");
                    http_close();
                    audio_core::pcm_ring().release(audio_core::PcmRing::OWNER_RADIO);
                    s.state.store(ST_IDLE);
                } else {
                    close_stream(); s.state.store(ST_RECONNECTING);
                }
                break;
            case Pump::WORKED:
                if (esp_timer_get_time() >= yield_at) {
                    sleep_ms(5);           // at least one tick between decode batches
                    yield_at = 0;
                }
                break;
            case Pump::IDLE:   sleep_ms(5); yield_at = 0; break;
            case Pump::LOST:
                if (s.one_shot.load(std::memory_order_acquire)) {
                    finish_or_resume();
                    break;
                }
                ESP_LOGW(TAG, "stream lost after %s", s.fmt[0] ? s.fmt : "no audio");
                close_stream();
                s.state.store(ST_RECONNECTING, std::memory_order_release);
                break;
            }
        } else if (now_st == ST_RECONNECTING) {
            // The invariant, stated where it is enforced: a pushed track never reconnects.
            // The reconnect below tunes the STATION at `station`, which for a pushed URL is
            // whatever was playing before it — so a path that reaches here holding one_shot
            // would not retry the track, it would silently start a different one.
            if (s.one_shot.load(std::memory_order_acquire)) { stop_station(); continue; }
            // No network yet is not a failure worth backing off for — it is the ordinary
            // state of a device that has just been switched on, and it clears by itself.
            if (!s.net_up.load(std::memory_order_acquire)) {
                sleep_ms(200);
                continue;
            }
            const uint32_t wait = backoff_ms(s.retry);
            if (s.retry < 100) s.retry++;
            ESP_LOGI(TAG, "reconnecting in %lu ms (attempt %d)", (unsigned long)wait, s.retry);
            // Sleep in slices so a stop or a change of station is not stuck behind the wait.
            for (uint32_t slept = 0; slept < wait && s.run; slept += 50) {
                if (uxQueueMessagesWaiting(s.q)) break;
                sleep_ms(50);
            }
            if (s.state.load(std::memory_order_acquire) == ST_RECONNECTING &&
                !uxQueueMessagesWaiting(s.q)) {
                char url[sizeof(radio_core::Station::url)];
                if (station_url(s.station.load(), url, sizeof(url)) && !tune(url)) {
                    s.state.store(ST_RECONNECTING, std::memory_order_release);
                }
            }
        } else {
            sleep_ms(50);
            yield_at = 0;
        }
    }
    set_modem_sleep(true);
    s.task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

/// Everything the radio needs in order to play, made when it is asked to play.
///
/// Nothing here exists while the card is the source, and that is the point. Measured on the
/// bench (18.09.2026): with this module merely PRESENT and idle, the card player decoded at
/// 0.6x real time with its ring at 6% and the audio task counting forty underruns a second;
/// without it, 1.0x with the ring at 91%. Two sources are never wanted at once, so the one
/// that is not selected should not be holding a filter bank, two task stacks and a
/// megabyte of buffers, nor waking up eighty times a second to find there is nothing to do.
bool radio_up() {
    if (s.up) return true;

    if (!audio_core::pcm_ring().init(OUT_RATE / 2)) {
        ESP_LOGE(TAG, "PCM ring allocation failed");
        return false;
    }
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder registration failed");
        return false;
    }

    s.raw       = (uint8_t*)heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.pcm       = (uint8_t*)heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.pcm_cap   = PCM_BYTES;                // a grown one went with the last radio_down()
    s.net_chunk = (uint8_t*)heap_caps_malloc(NET_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.feed      = (uint8_t*)heap_caps_malloc(NET_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.meta      = (char*)heap_caps_malloc(16 * 255 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.icy_buf   = (char*)heap_caps_malloc(16 * 255 + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.fbuf      = (float*)heap_caps_malloc(CHUNK * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.rbuf      = (float*)heap_caps_malloc(CHUNK * 2 * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.raw || !s.pcm || !s.fbuf || !s.rbuf || !s.net_chunk || !s.feed || !s.meta ||
        !s.icy_buf) {
        ESP_LOGE(TAG, "buffer allocation failed");
        return false;
    }
    if (!s.q) s.q = xQueueCreate(4, sizeof(CmdMsg));
    if (!s.q) return false;
    xQueueReset(s.q);
    raw_clear();

    if (!radio_core::next_stream::init()) ESP_LOGW(TAG, "next-stream preparation unavailable");
    s.run = true;
    // Core 0 with the card player: core 1 belongs to the audio task, and a late decode
    // costs buffer while a late audio block costs a click.
    // HTTP/TLS, redirect parsing and logging exceed the old 6 KB decoder stack.
    // Neither producer runs with the flash cache disabled; use PSRAM stacks to
    // preserve the internal DMA pool needed by ESP-Hosted.
    s.up = true;
    if (xTaskCreatePinnedToCoreWithCaps(radio_task, "radio", 12 * 1024, nullptr,
            RADIO_TASK_PRIORITY, &s.task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
        xTaskCreatePinnedToCoreWithCaps(reader_task, "radio_net", 6 * 1024, nullptr,
            RADIO_TASK_PRIORITY, &s.reader, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s.run = false;
        ESP_LOGE(TAG, "stream task creation failed");
        return false; // radio_down polls task exit before reclaiming buffers.
    }
    // 30 s each. Generous on purpose: a connect sits inside an eight-second timeout and a
    // reconnect backs off for longer still, so anything tighter would report a station that
    // is merely unreachable as a firmware fault.
    modesp::liveness::watch("radio", &s.beat, 30000);
    modesp::liveness::watch("radio_net", &s.reader_beat, 30000);
    ESP_LOGI(TAG, "up: %d stations, %u KB of stream buffer",
             s.list.count, (unsigned)(RAW_BYTES / 1024));
    return true;
}

void radio_down() {
    radio_core::next_stream::request(s.track_generation.load(), "");
    if (!s.up) return;
    // Unwatched first: these tasks are torn down on purpose whenever the radio stops being
    // the source, and a counter that stops because its task was asked to exit is not a
    // fault to report.
    modesp::liveness::unwatch("radio");
    modesp::liveness::unwatch("radio_net");
    s.run = false;
    // Teardown is polled from main, never waited there. A connect may still be
    // inside its eight-second timeout; its buffers must remain alive until exit.
    if (s.task || s.reader) return;
    close_stream();
    audio_core::pcm_ring().release(audio_core::PcmRing::OWNER_RADIO);
    s.conv.free_bank();

    free(s.raw);       s.raw = nullptr;
    free(s.pcm);       s.pcm = nullptr;
    free(s.net_chunk); s.net_chunk = nullptr;
    free(s.feed);      s.feed = nullptr;
    free(s.meta);      s.meta = nullptr;
    free(s.icy_buf);   s.icy_buf = nullptr;
    free(s.fbuf);      s.fbuf = nullptr;
    free(s.rbuf);      s.rbuf = nullptr;
    s.state.store(ST_IDLE, std::memory_order_release);
    s.up = false;
    ESP_LOGI(TAG, "down");
}

bool send(Cmd type, int32_t value = 0) {
    if (!s.q) return false;
    CmdMsg m{};
    m.type = type;
    m.value = value;
    if(type==Cmd::URL||type==Cmd::CATALOG)snprintf(m.uri,sizeof(m.uri),"%s",s.url_req);
    if(type==Cmd::STOP)xQueueReset(s.q);
    return xQueueSend(s.q, &m, 0)==pdTRUE;
}

/// The main task's side of a catalog station: what was asked for, until the task has it,
/// and what radio.name / radio.url / radio.uuid show while it plays. Main task only.
struct CatalogAsk {
    bool                pending = false;
    radio_core::Station want;
    radio_core::Station shown;
    char                want_favicon[radio_core::URL_LEN]  = "";   ///< the row's logo URL
    char                shown_favicon[radio_core::URL_LEN] = "";
};
CatalogAsk& cat_ask() {
    // In PSRAM like the task state: two stations are ~650 bytes of .bss otherwise.
    static CatalogAsk* p = [] {
        void* mem = heap_caps_calloc_prefer(1, sizeof(CatalogAsk), 2,
                                            MALLOC_CAP_SPIRAM  | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        configASSERT(mem != nullptr);
        return new (mem) CatalogAsk();
    }();
    return *p;
}

/// The uuid of what is playing: the list station's, or the catalog station's.
const char* current_uuid() {
    const int32_t idx = s.station.load(std::memory_order_acquire);
    if (idx == TRANSIENT) return cat_ask().shown.uuid;
    const radio_core::Station* st = s.list.at(idx);
    return st ? st->uuid : "";
}

#include "stations_http.inc"

/// radio.list_rev as last published; the store's generation is never all ones.
uint32_t s_list_rev = UINT32_MAX;

}  // namespace

void RadioModule::play_catalog(const char* name, const char* url, const char* uuid,
                               const char* favicon) {
    CatalogAsk& c = cat_ask();
    snprintf(c.want.name, sizeof(c.want.name), "%s", name ? name : "");
    snprintf(c.want.url, sizeof(c.want.url), "%s", url ? url : "");
    snprintf(c.want.uuid, sizeof(c.want.uuid), "%s", uuid ? uuid : "");
    snprintf(c.want_favicon, sizeof(c.want_favicon), "%s", favicon ? favicon : "");
    c.pending = c.want.url[0] != '\0';
    state_set("audio.source", AUDIO_SRC_RADIO);
    // Say at once that the list's station is not the one wanted: the next pass compares
    // radio.station against `station`, and a stale index there would tune the list's
    // station instead.
    s.station.store(TRANSIENT, std::memory_order_release);
    state_set("radio.station", (int32_t)-1);
}

int RadioModule::add_station(const char* name, const char* url, const char* uuid, int preset) {
    if (!url || !url[0]) return -1;
    const bool to_preset = preset >= 0 && preset < radio_core::PRESETS;
    // The store holds the presets the screen stored since the last save; the list here
    // may not, and setting the store from it below would undo them.
    radio_core::stations().copy(s.list);
    const int have = s.list.find(url);
    if (have >= 0) {
        if (to_preset) {
            radio_core::stations().set_preset(preset, have);
            save_stations();
            ESP_LOGI(TAG, "preset %d = station %d", preset, have);
        }
        return have;
    }
    if (!s.list.add(name, url, uuid)) return -1;
    const int idx = s.list.count - 1;
    radio_core::stations().set(s.list);
    if (to_preset) radio_core::stations().set_preset(preset, idx);
    save_stations();
    state_set("radio.count", (int32_t)s.list.count);
    ESP_LOGI(TAG, "station %d added: %s%s", idx, s.list.station[idx].name, to_preset ? ", as a preset" : "");
    return idx;
}

int RadioModule::keep_playing_station(int preset) {
    const int32_t now = s.station.load(std::memory_order_acquire);
    if (now >= 0 && now < s.list.count && preset >= 0 && preset < radio_core::PRESETS &&
        !s.one_shot.load(std::memory_order_acquire)) {
        radio_core::stations().set_preset(preset, now);   // already in the list: the preset only
        save_stations();
        ESP_LOGI(TAG, "preset %d = station %d", preset, (int)now);
        return now;
    }
    if (now != TRANSIENT) return -1;
    const radio_core::Station& st = cat_ask().shown;
    const int idx = add_station(st.name, st.url, st.uuid, preset);
    if (idx < 0) return -1;
    // The same stream goes on playing; it is simply the list's station now, so a preset
    // can hold it and a reconnect finds it there.
    s.tuned.store(idx, std::memory_order_release);
    s.station.store(idx, std::memory_order_release);
    state_set("radio.station", (int32_t)idx);
    return idx;
}

RadioModule::RadioModule() : modesp::BaseModule("radio") {}

bool RadioModule::on_init() {
    state_set("radio.state", "idle");
    // What PersistService restored, not 0: this key is persisted, and writing 0 over it here
    // put the first station back at every boot, so Play after a power cut tuned station 0
    // instead of the last one. An index the list no longer has is caught where it is read.
    state_set("radio.station", read_int("radio.station", 0));
    state_set("radio.name", "");
    state_set("radio.url", "");
    state_set("radio.uuid", "");
    state_set("radio.now_playing", "");
    state_set("radio.format", "");
    state_set("radio.count", (int32_t)0);
    state_set("radio.buffer", (int32_t)0);
    state_set("radio.mode", "station");
    state_set("radio.pos_s", (int32_t)0);
    state_set("radio.dur_s", (int32_t)0);
    state_set("radio.seekable", false);
    state_set("radio.seek_s", (int32_t)-1);
    state_set("radio.save", (int32_t)0);

    load_stations();
    // The web edits the list through its own routes (stations_http.inc), registered before
    // the static wildcard like every module route (modesp/net/http_routes.h).
    if (!modesp::http_routes::add(register_station_routes)) {
        ESP_LOGW(TAG, "no room for /api/stations: the web cannot edit the station list");
    }
    // Nothing else. The buffers, the filter bank and the two tasks are made when the radio
    // becomes the selected source and destroyed when it stops being one — see radio_up().
    return true;
}

void RadioModule::on_update(uint32_t dt_ms) {
    // The catalog first: browsing works whatever is playing (src/catalog.cpp).
    catalog_update(dt_ms);

    // An edit of the list from the web, whatever is playing, and the generation the web
    // watches to know the list changed — by its hand, the panel's or the catalog's.
    {
        int32_t st = 0;
        if (apply_station_edit(st)) {
            state_set("radio.station", st == TRANSIENT ? (int32_t)-1 : st);
            state_set("radio.count", (int32_t)s.list.count);
        }
        const uint32_t rev = radio_core::stations().generation();
        if (rev != s_list_rev) {
            s_list_rev = rev;
            state_set("radio.list_rev", (int32_t)rev);
        }
    }

    // The source decides whether this module exists at all.
    const bool mine = (read_int("audio.source", 0) == AUDIO_SRC_RADIO);
    if (!mine) {
        if (s.up) radio_down();
        report_ms_ = 0;
        health_ms_ = 0;
        // Anything posted while another source owns the output is stale by the time this
        // one gets it back, and the rest of this function — which is where commands are
        // consumed — does not run. Drop them rather than let eight presses accumulate and
        // all fire at once on the switch back. The state key this replaced behaved the same
        // way by construction: only its last value survived.
        modesp::BaseModule::Command drop;
        while (next_command(drop)) {}
        return;
    }
    if (s.up && !s.run) { radio_down(); if(s.up)return; }
    if (!radio_up()) return;
    char next_url[PLAY_URL_LEN]{};
    read_string("radio.next_url", next_url, sizeof(next_url));
    const int32_t duration = s.dur_s.load();
    const uint32_t rate = s.src_rate;
    const int32_t position = rate ? (int32_t)(s.frames_in.load() / rate) : 0;
    const bool near_end = s.one_shot.load() && s.state.load() == ST_PLAYING &&
                          !audio_core::pcm_ring().paused() &&
                          duration > 0 && duration - position <= 12;
    radio_core::next_stream::request(s.track_generation.load(), near_end ? next_url : "");

    if (!restored_) {
        restored_ = true;
        int32_t idx = read_int("radio.station", 0);
        if (idx < 0 || idx >= s.list.count) idx = 0;
        s.station.store(idx, std::memory_order_release);
        last_station_ = idx;
    }

    // A preset stored from the screen: the store already has it, this writes it down.
    if (read_int("radio.save", 0) != 0) {
        state_set("radio.save", (int32_t)0);
        save_stations();
    }

    {
        const bool up = read_bool("wifi.connected", false);
        static bool was = false;
        if (up != was) { was = up; ESP_LOGI(TAG, "network %s", up ? "up" : "down"); }
        s.net_up.store(up, std::memory_order_release);
    }

    // A loop, not an `if`: radio.cmd is a queue now (BaseModule::post_command), so a
    // double press is two commands. See the same spot in player_module.cpp.
    modesp::BaseModule::Command rc;
    while (next_command(rc)) {
        const int32_t cmd = rc.value;
        if (cmd == 0) continue;
        switch (cmd) {
        case 1:  // play, and take the output — the DSP keeps playing whichever source is
                 // selected, so tuning has to claim it or nothing is heard.
            state_set("audio.source", AUDIO_SRC_RADIO);
            send(Cmd::PLAY);
            break;
        case 2: send(Cmd::STOP); break;
        case 3:     // next
        case 4: {   // previous
            // Worked out HERE and published at once, as play_catalog does. Left to the task,
            // `station` moved there while radio.station still held the old index until the
            // next report, and the station pick below read that as a pick of the old station
            // and tuned it straight back (deep review 2026-10-12, RP-1).
            state_set("audio.source", AUDIO_SRC_RADIO);
            const int32_t count = s.list.count;
            if (!count) break;
            int32_t i = s.station.load(std::memory_order_acquire);
            if (i >= count) i = -1;
            // From a catalog station, the list starts at its first.
            const int32_t target = (cmd == 3) ? (i >= 0 ? (i + 1) % count : 0)
                                              : (i > 0 ? i - 1 : count - 1);
            s.station.store(target, std::memory_order_release);
            state_set("radio.station", target);
            send(Cmd::TUNE, target);
            break;
        }
        default: break;
        }
    }

    // A URL pushed from outside the list: DLNA's SetAVTransportURI, or a box in the web UI.
    //
    // Consumed rather than held — the key is a request, not a setting. Leaving it set would
    // make every pass through here look like a fresh push, and the same track would restart
    // a hundred times a second.
    {
        char pushed[PLAY_URL_LEN];
        if (read_string("radio.play_url", pushed, sizeof(pushed)) && pushed[0]) {
            snprintf(s.url_req, sizeof(s.url_req), "%s", pushed);
            if(send(Cmd::URL)) {
                state_set("radio.play_url", "");
                state_set("audio.source", AUDIO_SRC_RADIO);
            }
        }
    }

    // A catalog station asked for (play_catalog), handed to the task now that it exists.
    {
        CatalogAsk& c = cat_ask();
        if (c.pending) {
            snprintf(s.url_req, sizeof(s.url_req), "%s", c.want.url);
            if (send(Cmd::CATALOG)) {
                c.pending = false;
                c.shown = c.want;
                memcpy(c.shown_favicon, c.want_favicon, sizeof(c.shown_favicon));
                ESP_LOGI(TAG, "catalog station: %s", c.shown.name);
            }
        }
    }

    // A seek, from the screen, from MQTT, or from a DLNA control point's progress bar.
    // Consumed like play_url: it is a request, not a setting, and -1 means nothing pending.
    // Consumed only once the task's queue took it: with the queue full the position stays
    // asked for and goes on the next pass, instead of being dropped (display review, item 4).
    {
        const int32_t want = read_int("radio.seek_s", -1);
        if (want >= 0 && send(Cmd::SEEK, want)) state_set("radio.seek_s", (int32_t)-1);
    }

    // A station picked from the screen or from MQTT.
    //
    // The wanted station is recorded HERE, before the request goes to the task, because
    // this runs a hundred times a second and the task takes a moment to pick the message
    // up. Comparing against what the task has already tuned meant every tick in between
    // looked like another change, and one tap on a preset became a burst of connections —
    // the station never had time to start before the next request tore it down.
    const int32_t want = read_int("radio.station", -1);
    if (want >= 0 && want < s.list.count && want != s.station.load(std::memory_order_acquire)) {
        s.station.store(want, std::memory_order_release);
        state_set("audio.source", AUDIO_SRC_RADIO);
        send(Cmd::TUNE, want);
    }

    report_ms_ += dt_ms;
    if (report_ms_ < 500) return;
    report_ms_ = 0;

    // ── stream health ──
    {
        const uint8_t st = s.state.load(std::memory_order_acquire);
        health_ms_ += 500;
        if (st == ST_BUFFERING || st == ST_PLAYING) {
            if (health_ms_ >= 5000) {
                const uint32_t bytes = s.net_bytes.exchange(0, std::memory_order_relaxed);
                const uint32_t frames = s.dec_frames.exchange(0, std::memory_order_relaxed);
                const uint32_t need = s.bitrate_bps ? s.bitrate_bps / 8 :
                                      (s.bitrate ? s.bitrate * 1000 / 8 : 16000);
                const uint32_t calls = s.pump_calls.exchange(0, std::memory_order_relaxed);
                const uint32_t idle  = s.pump_idle.exchange(0, std::memory_order_relaxed);
                const uint32_t dus   = s.dec_us.exchange(0, std::memory_order_relaxed);
                const uint32_t cus   = s.conv_us.exchange(0, std::memory_order_relaxed);
                const uint32_t secs  = health_ms_ / 1000;
                const uint32_t rate  = bytes / secs;
                const unsigned fill  = (unsigned)(raw_used() * 100 / RAW_BYTES);
                ESP_LOGI(TAG, "stream: buffer %u%%, net %lu B/s (need %lu), audio %.2fx",
                         fill, (unsigned long)rate, (unsigned long)need,
                         (double)frames / (double)(s.src_rate ? s.src_rate : OUT_RATE)
                             / ((double)health_ms_ / 1000.0));
                ESP_LOGI(TAG, "  pump %lu/s (idle %lu), decoder %lu ms/s, resampler %lu ms/s",
                         (unsigned long)(calls / secs), (unsigned long)(idle / secs),
                         (unsigned long)(dus / secs / 1000), (unsigned long)(cus / secs / 1000));
                health_ms_ = 0;

                // A station's server that cannot keep up is left for another of its addresses
                // (stream_watch.h; the bench of 05.10.2026 had one ~200 ms away starve a 320k
                // stream). Stations only: a pushed track is finished or resumed by its own rules.
                const uint32_t gen = s.track_generation.load(std::memory_order_acquire);
                if (gen != starve_gen_) { starve_gen_ = gen; s_starve.reset(); }
                if (!s.one_shot.load(std::memory_order_acquire) &&
                    s_starve.update(rate, need, fill, (uint32_t)(esp_timer_get_time() / 1000000))) {
                    const uint32_t ip = s.peer_ip.load(std::memory_order_acquire);
                    char a[16] = "?";
                    if (ip) { struct in_addr x; x.s_addr = ip; inet_ntoa_r(x, a, sizeof(a)); }
                    ESP_LOGW(TAG, "stream: %lu B/s of %lu with the buffer dry for %d reports; "
                                  "leaving server %s for another", (unsigned long)rate,
                             (unsigned long)need, radio_core::StarveWatch::WINDOWS, a);
                    s.avoid_ip.store(ip, std::memory_order_release);
                    s.lost.store(true, std::memory_order_release);   // the radio task reconnects
                }
            }
        } else {
            health_ms_ = 0;
            s.net_bytes.store(0, std::memory_order_relaxed);
            s.dec_frames.store(0, std::memory_order_relaxed);
        }
    }

    const uint8_t st = s.state.load(std::memory_order_acquire);
    state_set("radio.state", STATE_NAMES[st < 5 ? st : 0]);
    char ended[PLAY_URL_LEN], promoted[PLAY_URL_LEN]; uint32_t sequence;
    portENTER_CRITICAL(&end_mux);
    memcpy(ended, completed_uri, sizeof(ended));
    memcpy(promoted, promoted_uri, sizeof(promoted)); sequence = completed_sequence;
    portEXIT_CRITICAL(&end_mux);
    state_set("radio.completed_uri", ended);
    state_set("radio.promoted_uri", promoted);
    state_set("radio.completed", (int32_t)sequence);
    const int32_t idx = s.station.load(std::memory_order_acquire);
    state_set("radio.station", idx == TRANSIENT ? (int32_t)-1 : idx);
    state_set("radio.count", (int32_t)s.list.count);
    state_set("radio.now_playing", s.now);
    // A cover for it, found by the title (src/art.cpp): a new title asks, none clears.
    radio_art_task::update(s.now, read_bool("radio.covers", true));
    state_set("radio.format", s.fmt);
    // The compressed buffer, as a percentage: on a good link it sits near full, and when a
    // station stutters this is the number that says whether the network or the decoder is
    // behind. Published rather than logged, because it belongs on the screen.
    state_set("radio.buffer", (int32_t)(raw_used() * 100 / RAW_BYTES));
    // WHAT is playing, as opposed to which engine is playing it. audio.source answers
    // "who owns the output" and stays 5 for both; the screen asks a different question and
    // needs a different answer, or a pushed track gets labelled as the radio. Everything
    // that arrives by being pushed — DLNA today, AirPlay or slimproto later — is "push",
    // so none of them needs a source number of its own (customer, 18.09.2026).
    const bool pushed = s.one_shot.load(std::memory_order_acquire);
    state_set("radio.mode", pushed ? "push" : "station");
    const uint32_t sr = s.src_rate ? s.src_rate : 0;
    state_set("radio.pos_s",
              (int32_t)(sr ? s.frames_in.load(std::memory_order_acquire) / sr : 0));
    state_set("radio.dur_s", s.dur_s.load(std::memory_order_acquire));
    state_set("radio.seekable", pushed && !s.wav.load(std::memory_order_acquire) &&
              s.dur_s.load(std::memory_order_acquire) > 0);
    if (s.one_shot.load(std::memory_order_acquire)) {
        // A pushed track is in nobody's list, so there is no station name to show. What is
        // playing is the URL itself; whoever pushed it owns the title (dlna.title).
        state_set("radio.name", "");
        state_set("radio.url", s.asked);
        state_set("radio.uuid", "");
    } else if (idx == TRANSIENT) {
        const radio_core::Station& c = cat_ask().shown;
        state_set("radio.name", c.name);
        state_set("radio.url", c.url);
        state_set("radio.uuid", c.uuid);
    } else if (const radio_core::Station* st_p = s.list.at(idx)) {
        state_set("radio.name", st_p->name);
        state_set("radio.url", st_p->url);
        state_set("radio.uuid", st_p->uuid);
    }

    // The station's logo, once per tune (src/art.cpp): the catalog row's favicon when there
    // is one, else Radio Browser's by the station's uuid. An HTTPS-only logo is fetched only
    // for a station whose own stream is plain HTTP — the TLS session is the station's.
    {
        const uint32_t gen = s.track_generation.load(std::memory_order_acquire);
        if ((st == ST_BUFFERING || st == ST_PLAYING) && gen != logo_gen_) {
            logo_gen_ = gen;
            if (s.one_shot.load(std::memory_order_acquire)) {
                radio_art_task::station("", "", false);       // a pushed track: no station
            } else {
                const bool cat = (idx == TRANSIENT);
                const radio_core::Station* lst = cat ? nullptr : s.list.at(idx);
                const char* url = cat ? cat_ask().shown.url : lst ? lst->url : "";
                radio_art_task::station(current_uuid(), cat ? cat_ask().shown_favicon : "",
                                        strncmp(url, "https://", 8) != 0);
            }
        }
    }

    // A play is counted with Radio Browser once per tune of a station that came from it —
    // what the service asks in return for no key and no account, and what keeps its
    // "popular" lists true. Only once the audio is playing.
    {
        const uint32_t gen = s.track_generation.load(std::memory_order_acquire);
        if (st == ST_PLAYING && gen != click_gen_) {
            click_gen_ = gen;
            const char* uuid = s.one_shot.load() ? "" : current_uuid();
            if (uuid[0] && read_bool("radio.report_clicks", true)) radio_catalog_task::click(uuid);
        }
    }
    if (idx != last_station_) last_station_ = idx;
}

void RadioModule::on_stop() { radio_down(); }
