/**
 * @file player_module.cpp
 * @brief Local playback: file → decoder → resampler → the PCM ring the audio core reads.
 *
 * Its own task, not the audio task. Reading a card and decoding both block for milliseconds
 * at a time, and the audio task must never block — so the two are separated by the lock-free
 * ring in components/audio_dsp, and the audio core simply finds frames there or does not.
 *
 * Rate: the output clock follows the session's first track (docs/AUDIO.md, "Output rate and
 * sessions"); a later track at another rate is converted to it. The resampler written for
 * Bluetooth therefore sits on this path too — but at a CONSTANT ratio, because a file has no clock of its own to drift against.
 * That makes it strictly simpler here than the case it was written for: no ring-fill feedback,
 * no ppm trim.
 */

#include "player_module.h"
#include "pcm_ring.h"
#include "audio_pipe.h"
#include "audio_seek.h"
#include "pcm_producer.h"
#include "safe_file.h"
#include "user_store.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "playlist.h"
#include "audio_tags.h"
#include "cover.h"
#include "modesp/liveness.h"
#include "state_enums.h"   // audio.source values, generated from the manifests
#include <dirent.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <cstdio>
#include <cstring>
#include <climits>
#include <strings.h>

static const char* TAG = "Player";

namespace {

/// The rate the output STARTS at, not the rate it stays at.
///
/// The clock has to be running at something before any source has said what it wants, and
/// this is that something. Everything that turns frames into seconds asks fs() instead —
/// a track at 44.1 kHz is played at 44.1 kHz, and a position counted at 48 would run fast.
constexpr uint32_t OUT_RATE    = 48000;
/// Half a second of audio.
///
/// Two seconds was a guess, taken to "absorb a card read that stalls", and it made every
/// flush expensive: a track switch empties the ring and then has to refill it from nothing
/// before playback can resume, so the buffer's size IS the latency of switching. A card read
/// stalls for tens of milliseconds, not for seconds — the margin was never spent on anything
/// and cost a quarter of a second of dead air at every press.
/// Half a second at the highest rate we start at; a little more at 44.1, which is fine —
/// what matters is that it is long enough, not that it is exact.
constexpr size_t   RING_FRAMES = OUT_RATE / 2;
constexpr size_t   RAW_BYTES   = 16 * 1024;        // compressed bytes held for the parser
constexpr size_t   PCM_BYTES   = 32 * 1024;        // decoded PCM per process() call
constexpr size_t   PCM_MAX     = 256 * 1024;       // ...grown to for a frame that needs it
constexpr size_t   CHUNK       = 1024;             // frames converted/resampled at a time
// esp_audio_codec 2.5.0 packs a mono AAC frame in place in the out buffer with a loop twice
// as long as the frame: it writes up to 8 KB past out.buffer and reads up to 12 KB (fixed in
// 2.6.2, which needs P4 rev 3.0). At 12 KB or more the overrun stays inside our own buffer.
static_assert(PCM_BYTES >= 12 * 1024, "esp_audio_codec 2.5.0 mono AAC overruns a smaller out buffer");
// The converter — its fixed-ratio bank, the cost of building one and why it is kept per
// rate — is audio_core::RateConverter in components/audio_pipe: one copy for the card and
// the radio, with the reasoning in its header rather than repeated here.

enum class Cmd : uint8_t { PLAY, PAUSE, RESUME, STOP, NEXT, PREV, TOGGLE, QUEUE, SEEK };
/// Where the last track is remembered, as a path relative to the card's root. Not NVS: the
/// framework persists numbers, and a path is a string. One small file on the data
/// partition, rewritten only when the track actually changes.
// Resolved once, on the first pass of on_update(), through UserStore — /userdata/last_track
// when the store is up, /data/last_track on a panel still running the old partition table.
// The file is last_track and holds a track, not a directory; the constant that pointed at it
// was called LAST_DIR_PATH.
char s_last_track_path[160] = "/data/last_track";

/// audio.source value that means "the local file ring".
///
/// From generated/state_enums.h, not from audio_module.h: modules are selected
/// independently and a build may carry either one without the other, so depending on the
/// audio module's header would be a lie about how they are wired. The generated header is
/// the MANIFEST set, which is exactly the contract these two modules do share — see
/// docs/CONCEPT.md, "`audio.source` values". It used to be this number, written out here by hand.
constexpr int32_t AUDIO_SRC_FILE = modesp::gen::AUDIO_SOURCE_FILE;

/// As long as anything SharedState can carry for a path — the side table's width, since
/// player.track is one of the keys the manifest marks `long`. A path cannot be cut a second
/// time on its way through this module.
constexpr size_t PATH_MAX_LEN = MODESP_MAX_LONG_STRING_LENGTH + 1;

struct CmdMsg {
    Cmd     type;
    uint32_t generation = 0;          ///< cancels PCM belonging to an older queued command
    int32_t value = 0;                ///< SEEK: the second to jump to
    char    path[PATH_MAX_LEN];
};

/// Which decoder a file name asks for.
///
/// This list was three entries long while the decoder underneath registered twenty. Both
/// registries are populated at startup — esp_audio_dec_register_default() for the elementary
/// streams and esp_audio_simple_dec_register_default() for the containers — so M4A, AAC and
/// Ogg were decodable the whole time and were being skipped here, by name, before anything
/// was opened. An ALAC rip sitting on the card looked like an unsupported format when the
/// only unsupported thing was this function.
///
/// Extensions rather than sniffing: the card is a user's music folder, not an untrusted
/// stream, and a wrong guess costs one refused file rather than a wedged decoder. The radio
/// goes the other way and reads Content-Type, because a stream has no name.
esp_audio_simple_dec_type_t type_from_name(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    if (!strcasecmp(dot, ".mp3"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (!strcasecmp(dot, ".flac")) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (!strcasecmp(dot, ".wav"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    // One container, several things inside it. M4A carries AAC or ALAC and the parser works
    // out which, so .m4a, .mp4 and .alac all arrive here.
    if (!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".mp4") ||
        !strcasecmp(dot, ".alac")) return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    // Ogg likewise: Vorbis, Opus or FLAC, decided by the stream header.
    if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga") ||
        !strcasecmp(dot, ".opus")) return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    if (!strcasecmp(dot, ".aac"))  return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (!strcasecmp(dot, ".ts"))   return ESP_AUDIO_SIMPLE_DEC_TYPE_TS;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

/// Decoded PCM → interleaved stereo float in −1..1: the shared conversion, which this file
/// carried a verbatim copy of. One implementation, and the 24-bit sign extension it gets
/// right is pinned by tests/host/test_audio_pipe.cpp.
using audio_core::pcm_to_f32;

} // namespace

/// Everything the task owns. Kept out of the header so the decoder and resampler types stay
/// private to this file.
struct PlayerTaskState {
    QueueHandle_t q = nullptr;
    TaskHandle_t  task = nullptr;
    std::atomic<bool> run{true};
    /// Set by player_task as its last act: on_stop() closes the track only once the task that
    /// reads it has left (see there).
    std::atomic<bool> exited{false};
    std::atomic<bool> selected{true};
    std::atomic<uint32_t> request_generation{0};
    uint32_t command_generation = 0;
    uint32_t ring_generation = 0;
    /// Bumped once per turn of player_task, so WatchdogService can tell a decoder that is
    /// idle from one that is wedged — the loop turns over every 50 ms at worst when there
    /// is nothing to do.
    modesp::liveness::Counter beat{0};

    FILE*    f = nullptr;
    esp_audio_simple_dec_handle_t dec = nullptr;
    /// The converter, which remembers the rate pair its bank was built for; `!asrc` reads
    /// as "no bank". The ratio is kept beside it because the input bound below asks for it.
    audio_core::RateConverter asrc;
    double    ratio = 1.0;

    uint8_t* raw = nullptr;
    size_t   raw_len = 0;
    uint8_t* pcm = nullptr;
    size_t   pcm_cap = PCM_BYTES;   // what `pcm` holds; grows, never shrinks
    float*   fbuf = nullptr;    // CHUNK frames of float stereo, decoder rate
    float*   rbuf = nullptr;    // resampled, 48 kHz

    // What the file says about itself. Read once, at open: a file name is the ripper's
    // punctuation, the tag is what the person who made the track wrote.
    audio_tags::Tags tags;
    uint32_t src_rate = 0;
    /// The rate of the track before this one, kept across close_file(): a track at the same
    /// rate as its predecessor gets the same answer, so it need not wait for the queue.
    uint32_t prev_src_rate = 0;
    /// True when the output clock was moved to the source rate, so nothing is converted.
    bool     direct = false;
    /// The rate the ring is fed at: the output's answer, or the fallback until it comes.
    /// Zero until the track's format is known.
    uint32_t clock = 0;
    uint8_t  src_bits = 0, src_ch = 0;
    uint64_t frames_out = 0;    // output frames produced — NOT yet heard; see pos_s
    uint64_t file_bytes = 0;    // the whole file, for the duration estimate
    bool is_wav = false;
    audio_core::WavPcmData wav_data;
    bool     file_eof = false;
    // The edges of a track are shaped HERE, because only this side knows where they are.
    // The consumer sees one continuous stream: the last frames of one track followed by
    // the first of the next, with nothing to mark the join — and a join between two
    // unrelated waveforms is a step, i.e. a click and a burst across the whole row. So
    // the producer holds back the last EDGE frames of every track and only lets them go
    // once it knows what follows: more of the same track (pass through), the end (fade
    // out), or a flush (drop — the consumer fades what it already has). The first EDGE
    // frames of each track fade in the same way.
    static constexpr size_t EDGE = 480;                 ///< ~10 ms, near enough at any rate
    float    tail[EDGE * 2] = {};                       ///< held back, not yet in the ring
    size_t   tail_len = 0;
    size_t   fade_in_left = 0;                          ///< frames of fade-in still to apply
    bool     cover_todo = false;                        ///< artwork not looked for yet; see load_cover()
    int      resync = 0;                                ///< decode errors still forgiven after a seek
    /// A seek asked for before the track's length is known, to be made once it is. The length
    /// comes from the decoder's first answer, so a SEEK sent right behind a PLAY — the track
    /// put back where it was lent — found none, was dropped, and the track restarted from
    /// its first second. −1 = none.
    int32_t  pending_seek = -1;
    char     path[PATH_MAX_LEN] = "";
    char     fmt[32] = "";
    int      state = 0;         // 0 idle, 1 playing, 2 paused, 3 error
    // Read by the main loop, written by the player task. Seconds rather than frames so the
    // reader needs no 64-bit atomic, and already corrected for what is still queued.
    std::atomic<int32_t> pos_s{0};
    std::atomic<int32_t> dur_s{0};
    std::atomic<bool> shuffle{false};
    std::atomic<player_core::Repeat> repeat{player_core::Repeat::OFF};
};

namespace {
PlayerTaskState s;

bool interrupts_pcm(Cmd cmd) {
    return cmd == Cmd::PLAY || cmd == Cmd::STOP || cmd == Cmd::NEXT || cmd == Cmd::PREV ||
           cmd == Cmd::SEEK || cmd == Cmd::QUEUE;
}

void queue_command(CmdMsg& msg) {
    const bool accepted = player_core::queue_pcm_command(s.request_generation,
        interrupts_pcm(msg.type), [&](uint32_t generation) {
            msg.generation = generation;
            if (xQueueSend(s.q, &msg, 0) == pdTRUE) return true;
            if (msg.type != Cmd::STOP) return false;
            // STOP supersedes pending navigation when the queue is full. There is only
            // one sender (on_update), so reset leaves room for this command immediately.
            xQueueReset(s.q);
            return xQueueSend(s.q, &msg, 0) == pdTRUE;
        });
    if (!accepted) ESP_LOGW(TAG, "command queue full; ignored %u", (unsigned)msg.type);
}

/// The rate the output is running at for this track: the source's own when nothing is being
/// converted, and the fixed output rate when something is.
uint32_t fs() {
    if (s.direct && s.src_rate) return s.src_rate;
    const uint32_t o = audio_core::pcm_ring().out_rate();
    return o ? o : OUT_RATE;
}

void sleep_at_least_a_tick(uint32_t ms);   // defined below, next to the pump loop

/// Smoothstep: p*p*(3-2p). Leaves and arrives with zero slope, which a straight line does
/// not — its corners are audible on a steady tone. Same shape the consumer's gate uses.
inline float edge_gain(float p) { return p * p * (3.0f - 2.0f * p); }

/// Write to the ring, waiting when it is full. The ring is sized so this rarely spins, but
/// when the consumer is slow the right thing is to wait rather than to drop audio.
bool ring_put(const float* stereo, size_t frames) {
    auto& ring = audio_core::pcm_ring();
    return player_core::put_pcm(ring, audio_core::PcmRing::OWNER_FILE, s.ring_generation,
        stereo, frames, [] {
            return !s.run.load() || !s.selected.load() ||
                   player_core::command_cancelled(s.request_generation, s.command_generation);
        }, [] { sleep_at_least_a_tick(5); });
}

/// Send decoded frames on their way, through the fade-in and the hold-back (see EDGE).
bool push_frames(float* stereo, size_t frames) {
    audio_core::fade_start(stereo, frames, s.fade_in_left, PlayerTaskState::EDGE);
    constexpr size_t EDGE = PlayerTaskState::EDGE;
    if (frames >= EDGE) {
        // Everything held so far is older than the new tail: out it goes, then the chunk
        // except its last EDGE frames, which become the new tail.
        if (!ring_put(s.tail, s.tail_len) || !ring_put(stereo, frames - EDGE)) return false;
        memcpy(s.tail, stereo + (frames - EDGE) * 2, EDGE * 2 * sizeof(float));
        s.tail_len = EDGE;
        return true;
    }
    const size_t total = s.tail_len + frames;
    if (total > EDGE) {
        const size_t out = total - EDGE;
        if (!ring_put(s.tail, out)) return false;
        memmove(s.tail, s.tail + out * 2, (s.tail_len - out) * 2 * sizeof(float));
        s.tail_len -= out;
    }
    memcpy(s.tail + s.tail_len * 2, stereo, frames * 2 * sizeof(float));
    s.tail_len += frames;
    return true;
}

/// The track has ended on its own: fade the held tail to silence and let it go.
bool release_tail() {
    for (size_t i = 0; i < s.tail_len; i++) {
        const float g = edge_gain(1.0f - (float)i / (float)s.tail_len);
        s.tail[2 * i] *= g;
        s.tail[2 * i + 1] *= g;
    }
    if (!ring_put(s.tail, s.tail_len)) return false;
    s.tail_len = 0;
    return true;
}

/// `len` bytes at `off` of an open file, in a PSRAM buffer the caller frees. Null when the
/// length is absurd or the read fails.
uint8_t* read_blob(FILE* f, uint64_t off, uint32_t len) {
    constexpr uint32_t MAX_JPEG = 1u << 20;     // a megabyte of artwork is already absurd
    if (!len || len > MAX_JPEG) return nullptr;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return nullptr;
    if (fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len) return buf;
    free(buf);
    return nullptr;
}

/// One track's artwork search, handed from this task to a worker of its own.
///
/// It used to run here, and a hardware decode is milliseconds - but a PNG or a progressive
/// JPEG goes to stb_image at about 1.1 us a source pixel, and this task waited for it with
/// half a second of audio in the ring. A 1000 px progressive cover was an audible gap a
/// moment into every track of its album, and the UI froze for as long, because the decoder
/// runs at its caller's priority and this one's is 5.
struct CoverJob {
    uint8_t* jpg;                   ///< the picture inside the track, or null; the worker frees it
    uint32_t len;
    uint32_t tag_len;               ///< what the tag said, for the log line
    int      mime;
    char     dir[PATH_MAX_LEN];     ///< the track's folder, to look in when that is not enough
};
QueueHandle_t cover_q = nullptr;
/// Bumped once per turn of the worker, which waits with a timeout so that this is a
/// heartbeat - as bt_cover's is.
modesp::liveness::Counter cover_beat{0};
/// 6 KB is what the other cover workers decode in (ap_cover, bt_cover); the card's open,
/// seek and read come on top. The software decode has its own task (SOFT_STACK in cover.cpp).
constexpr uint32_t COVER_STACK = 8 * 1024;

/// Two places to look, in the order a music player should: the picture inside the file,
/// then the one next to it in the folder. An album folder usually has both and they are
/// usually the same image; the embedded one belongs to THIS track, which matters on a
/// compilation where every track has its own.
void find_cover(const CoverJob& job) {
    using player_core::CoverSource;
    // Taken now, not when the job was posted. The previous track's job may still have been
    // decoding then; its picture moves the generation on when it lands, and a number taken
    // before that would cancel this track's own picture and leave the last one up.
    const uint32_t gen = player_core::cover_view(CoverSource::File).gen;
    bool done = job.jpg && player_core::cover_set_jpeg(job.jpg, job.len, CoverSource::File, gen);
    if (!done) {
        // The names ripping software actually writes, in the order of how likely they are.
        static const char* const NAMES[] = {"cover.jpg", "folder.jpg", "front.jpg",
                                            "cover.jpeg", "Cover.jpg", "Folder.jpg",
                                            "AlbumArt.jpg", "albumart.jpg"};
        for (const char* nm : NAMES) {
            char path[PATH_MAX_LEN];
            if (snprintf(path, sizeof(path), "%s/%s", job.dir, nm) >= (int)sizeof(path)) continue;
            FILE* cf = fopen(path, "rb");
            if (!cf) continue;
            uint32_t len = 0;
            if (fseek(cf, 0, SEEK_END) == 0) {
                const long end = ftell(cf);
                if (end > 0) len = (uint32_t)end;
            }
            uint8_t* buf = read_blob(cf, 0, len);
            fclose(cf);
            if (!buf) continue;
            done = player_core::cover_set_jpeg(buf, len, CoverSource::File, gen);
            free(buf);
            if (done) break;
        }
    }
    // Nothing for this track: empty the card rather than leave the last album's picture next
    // to it - unless a picture has landed since this search began.
    if (!done && player_core::cover_view(CoverSource::File).gen == gen) player_core::cover_clear();
    ESP_LOGI(TAG, "artwork: %s (tag %u bytes, mime %d)", done ? "loaded" : "none",
             (unsigned)job.tag_len, job.mime);
}

void cover_worker(void*) {
    CoverJob job;
    for (;;) {
        modesp::liveness::tick(cover_beat);
        if (xQueueReceive(cover_q, &job, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        find_cover(job);
        free(job.jpg);
    }
}

/// Hands the worker this track's search, dropping one still waiting: only the newest track
/// matters. Without a worker the search runs here, as it always used to.
void post_cover(CoverJob& job) {
    if (!cover_q) {
        find_cover(job);
        free(job.jpg);
        return;
    }
    CoverJob stale;
    if (xQueueReceive(cover_q, &stale, 0) == pdTRUE) free(stale.jpg);
    if (xQueueSend(cover_q, &job, 0) != pdTRUE) free(job.jpg);
}

/// The artwork for the track that is open, looked for once, in slack time.
///
/// Called from the pump loop's idle branch, which is where this task sits when the ring is
/// full. What is left here is the one read that needs this task's open file - the picture
/// inside the track, a card read of tens of milliseconds at most, which the full ring
/// covers. The folder and the decoding are the worker's (CoverJob). Doing even this at
/// open() would put a card read between two tracks, exactly where there is no time to spare.
void load_cover() {
    if (!s.cover_todo) return;
    s.cover_todo = false;

    CoverJob job{};
    if (s.f && s.tags.has_pic() && s.tags.pic_mime == audio_tags::PicMime::JPEG) {
        // The decoder is reading this same file; put the position back where it was.
        const long keep = ftell(s.f);
        job.jpg = read_blob(s.f, s.tags.pic_off, s.tags.pic_len);
        if (job.jpg) job.len = s.tags.pic_len;
        if (keep >= 0) fseek(s.f, keep, SEEK_SET);
    }
    job.tag_len = s.tags.pic_len;
    job.mime    = (int)s.tags.pic_mime;
    // The folder this track lives in — which, now that the index spans the whole card,
    // is no longer the same thing as the root.
    snprintf(job.dir, sizeof(job.dir), "%s", s.path);
    if (char* slash = strrchr(job.dir, '/')) *slash = '\0';
    post_cover(job);
}

/// Let go of the file and the decoder, leaving the ring alone.
///
/// Separate from close_track() because of what happens at the end of a track: the ring still
/// holds up to two seconds of the music that just finished, and that tail has not been heard
/// yet. Clearing it to start the next track would cut the end off every song.
void close_file() {
    if (s.dec) { esp_audio_simple_dec_close(s.dec); s.dec = nullptr; }
    if (s.f)   { fclose(s.f); s.f = nullptr; }
    // The filter history belongs to the track that just ended, but the BANK belongs to the
    // sample rate and the next track almost certainly shares it. Freeing it here is what put
    // thirty thousand double-precision sine evaluations between one track and the next.
    // RateConverter::ensure() clears the history when the next track opens.
    s.raw_len = 0;
    s.file_eof = false;
    s.frames_out = 0;
    s.file_bytes = 0;
    s.wav_data = {};
    s.is_wav = false;
    s.pos_s.store(0, std::memory_order_relaxed);
    s.dur_s.store(0, std::memory_order_relaxed);
    if (s.src_rate) s.prev_src_rate = s.src_rate;
    s.src_rate = 0;
    s.direct = false;
    s.clock = 0;
    s.fmt[0] = '\0';
    s.tags = audio_tags::Tags{};
}

/// Random access to the open file, for the tag parser — which does no I/O of its own.
size_t tag_read(void* ctx, uint64_t off, void* buf, size_t n) {
    FILE* f = (FILE*)ctx;
    if (off > LONG_MAX) return 0;
    if (fseek(f, (long)off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, f);
}

/// Everything close_file() does, plus getting rid of whatever is still queued.
///
/// Asked for, not done here. The samples already in the ring belong to the consumer now, and
/// it is the only side that can end them without a step — it fades what is queued and then
/// clears it. Dropping them from underneath is what made switching tracks click. Waiting
/// costs about ten milliseconds, which is less than opening the next file takes anyway.
void close_track() {
    close_file();
    s.tail_len = 0;             // nobody is going to hear it; the consumer fades the rest
    auto& ring = audio_core::pcm_ring();
    if (!ring.request_flush(audio_core::PcmRing::OWNER_FILE, s.ring_generation)) return;
    for (int i = 0; i < audio_core::FLUSH_WAIT_NAPS && ring.flush_pending(); i++) sleep_at_least_a_tick(5);
    if (ring.flush_pending()) {
        // The consumer is not running — nothing is being played, so there is nothing to
        // fade and no reason to keep waiting.
        ring.reset(audio_core::PcmRing::OWNER_FILE, s.ring_generation);
    }
    s.ring_generation = ring.generation();
}

/// The root of the card, as everything relative is measured from.
constexpr const char* CARD_ROOT = "/sd";

/// How many decode calls a seek is allowed to spend finding the next frame header. At a
/// chunk apiece this is a few tens of kilobytes, far more than any frame is long.
constexpr int RESYNC_TRIES = 16;

/// Is this a file the decoder can play?
bool playable(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".flac") || !strcasecmp(dot, ".wav");
}

/// One level of the card, adding what it finds and descending into what it does not know.
///
/// `rel` is where we are, relative to the root, and is extended and cut back in place — a
/// separate buffer per level would put a kilobyte of path on the stack at the depth a real
/// collection reaches. Depth is capped because a card can hold a symbolic-link loop, and
/// FAT cannot, but a card that is not FAT can.
void scan_dir(char* rel, size_t rel_cap, int depth) {
    auto& q = player_core::playlist();
    if (depth > 6 || q.pending() >= player_core::Playlist::MAX_TRACKS) return;

    char full[PATH_MAX_LEN];
    snprintf(full, sizeof(full), "%s%s%s", CARD_ROOT, rel[0] ? "/" : "", rel);
    DIR* d = opendir(full);
    if (!d) return;

    const size_t base = strlen(rel);
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;            // hidden, and "." and ".."
        const size_t nlen = strlen(de->d_name);
        const size_t need = base + (base ? 1 : 0) + nlen + 1;
        if (need > rel_cap) continue;                  // too deep to name; skip, do not cut
        // memcpy, not snprintf: the length has just been checked, and the compiler cannot
        // see that through a format string — it refuses to build one it cannot bound.
        size_t at = base;
        if (base) rel[at++] = '/';
        memcpy(rel + at, de->d_name, nlen);
        rel[at + nlen] = '\0';
        if (de->d_type == DT_DIR) {
            scan_dir(rel, rel_cap, depth + 1);
        } else if (playable(de->d_name)) {
            if (!q.add(rel)) break;                    // full: play what we have
        }
        rel[base] = '\0';
        if (q.pending() >= player_core::Playlist::MAX_TRACKS) break;
    }
    closedir(d);
}

/// Index the whole card: one recursive read, and after it every question is answered from
/// RAM. Done in the player's own task — reading a card blocks for milliseconds at a time,
/// and the main loop feeds the watchdog.
///
/// Sorted by relative path, which keeps an album together and in track order without any
/// special case for folders.
void index_card(const char* keep_rel) {
    auto& q = player_core::playlist();
    const int64_t t0 = esp_timer_get_time();
    q.begin(CARD_ROOT);
    char rel[player_core::Playlist::MAX_REL] = "";
    scan_dir(rel, sizeof(rel), 0);
    q.sort_pending();
    const int cursor = keep_rel ? q.find_pending(keep_rel) : -1;
    q.commit(cursor < 0 ? 0 : cursor);
    ESP_LOGI(TAG, "index: %d tracks in %lld ms, cursor %d",
             q.count(), (long long)((esp_timer_get_time() - t0) / 1000), q.cursor());
}

/// Jump to `sec` in the track that is playing.
///
/// There is no index in an MP3 or a FLAC saying where a second lives, and building one
/// means decoding the whole file. What there is, is a bitrate: the fraction of the track
/// asked for is the fraction of the bytes, give or take what variable bitrate does to it.
/// So the file position moves and the decoder picks the stream up again at the next frame
/// header it recognises — which is exactly what a decoder does after a dropout, and the
/// reason it can do it at all.
///
/// The decoder is NOT reopened. For a container like WAV the header has already been read
/// and reopening would lose it; for the others, resynchronising is cheaper than parsing
/// the file again from the top.
void seek_to(int32_t sec) {
    if (!s.f || !s.file_bytes) return;
    const int32_t dur = s.dur_s.load(std::memory_order_relaxed);
    if (dur <= 0) return;
    if (sec < 0) sec = 0;
    if (sec > dur - (s.is_wav ? 0 : 1)) sec = dur - (s.is_wav ? 0 : 1);
    if (s.is_wav && !s.wav_data.valid()) return; // compressed/unknown WAV cannot seek as PCM
    const uint64_t byte = s.is_wav ? audio_core::pcm_seek_byte(s.wav_data, sec)
        : audio_core::stream_seek_byte(s.file_bytes, sec, dur, 0, 0);
    if (byte > LONG_MAX) return;
    if (fseek(s.f, (long)byte, SEEK_SET) != 0) return;

    s.raw_len  = 0;
    s.file_eof = false;
    s.tail_len = 0;                       // the held-back edge belongs to where we were
    s.fade_in_left = PlayerTaskState::EDGE;   // arrive at the new place, do not cut into it
    s.resync = RESYNC_TRIES;
    s.frames_out = (uint64_t)sec * fs();
    s.pos_s.store(sec, std::memory_order_relaxed);
    s.asrc.reset();                       // the filter history is from before the jump

    // Drop what is queued, or the old position plays out first and the jump arrives late.
    auto& ring = audio_core::pcm_ring();
    if (!ring.request_flush(audio_core::PcmRing::OWNER_FILE, s.ring_generation)) return;
    for (int i = 0; i < audio_core::FLUSH_WAIT_NAPS && ring.flush_pending(); i++) sleep_at_least_a_tick(5);
    if (ring.flush_pending()) ring.reset(audio_core::PcmRing::OWNER_FILE, s.ring_generation);
    s.ring_generation = ring.generation();
    ESP_LOGI(TAG, "seek to %ld s (byte %llu of %llu)",
             (long)sec, (unsigned long long)byte, (unsigned long long)s.file_bytes);
}

/// Put the cursor on `path`, indexing the card first if that has not happened yet.
///
/// A path arrives from MQTT or the web API; the screen plays by index, because the list it
/// shows IS this index.
void cue_path(const char* path) {
    auto& q = player_core::playlist();
    const size_t rootlen = strlen(CARD_ROOT);
    const char* rel = path;
    if (!strncmp(path, CARD_ROOT, rootlen) && path[rootlen] == '/') rel = path + rootlen + 1;
    if (q.count() == 0) { index_card(rel); return; }
    const int i = q.find(rel);
    if (i >= 0) q.set_cursor(i);
}

bool open_track(const char* path, bool flush_ring) {
    s.pending_seek = -1;          // a seek belongs to the track it was asked for
    // Two producers share this ring — the card and the radio — and only the owner may
    // write. Claiming an unowned ring empties it; claiming one we already hold does
    // nothing, which is what a track boundary needs. Claiming it from a station that is
    // still playing fades that out first (pcm_ring.h, claim_after_fade).
    audio_core::claim_after_fade(audio_core::pcm_ring(), audio_core::PcmRing::OWNER_FILE,
                                 [] { sleep_at_least_a_tick(5); });
    s.ring_generation = audio_core::pcm_ring().generation();
    audio_core::pcm_ring().set_paused(false);   // a new track is never born paused
    if (flush_ring) close_track(); else close_file();
    const esp_audio_simple_dec_type_t type = type_from_name(path);
    s.is_wav = type == ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
        ESP_LOGW(TAG, "%s: unknown extension", path);
        return false;
    }
    if (esp_audio_simple_check_audio_type(type) != ESP_AUDIO_ERR_OK) {
        // The decoder was compiled out. Saying so beats failing later with a parse error.
        ESP_LOGW(TAG, "%s: decoder not built into this firmware", path);
        return false;
    }
    s.f = fopen(path, "rb");
    if (!s.f) {
        ESP_LOGW(TAG, "%s: cannot open", path);
        return false;
    }
    // A bigger stdio buffer turns one FAT cluster read per 4 KB into far fewer, which is the
    // difference between smooth playback and a card that cannot keep up with FLAC.
    setvbuf(s.f, nullptr, _IOFBF, 8 * 1024);
    if (fseek(s.f, 0, SEEK_END) == 0) {
        const long end = ftell(s.f);
        if (end > 0) s.file_bytes = (uint64_t)end;
    }
    if (s.is_wav) audio_core::read_wav_pcm_data(s.f, tag_read, s.file_bytes, s.wav_data);

    // Tags before decoding, while the file position is still ours to move. The parser reads
    // a few kilobytes near the start; the artwork it finds is only LOCATED, so a cover the
    // size of the track costs nothing until somebody asks to see it.
    {
        audio_tags::Reader tr;
        tr.ctx  = s.f;
        tr.read = tag_read;
        tr.size = s.file_bytes;
        audio_tags::read(tr, s.tags);
    }
    rewind(s.f);

    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type;
    // HE-AAC is off unless asked for, once per container that carries AAC here, and without
    // it an HE-AAC file plays its AAC-LC core: half the sample rate, and mono where the
    // stereo was carried as PS. Same reasoning and cost as in modules/radio; the SBR path's
    // stack stays under what FLAC already takes on this task.
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_m4a_dec_cfg_t m4a = {};          // track_idx 0: the first audio track, as before
    esp_ts_dec_cfg_t  ts  = {};
    aac.aac_plus_enable = m4a.aac_plus_enable = ts.aac_plus_enable = true;
    switch (type) {
    case ESP_AUDIO_SIMPLE_DEC_TYPE_AAC: cfg.dec_cfg = &aac; cfg.cfg_size = sizeof(aac); break;
    case ESP_AUDIO_SIMPLE_DEC_TYPE_M4A: cfg.dec_cfg = &m4a; cfg.cfg_size = sizeof(m4a); break;
    case ESP_AUDIO_SIMPLE_DEC_TYPE_TS:  cfg.dec_cfg = &ts;  cfg.cfg_size = sizeof(ts);  break;
    default: break;
    }
    if (esp_audio_simple_dec_open(&cfg, &s.dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "%s: decoder open failed", path);
        fclose(s.f);
        s.f = nullptr;
        return false;
    }
    snprintf(s.path, sizeof(s.path), "%s", path);
    s.fade_in_left = PlayerTaskState::EDGE;
    s.cover_todo   = true;
    s.resync       = 0;
    ESP_LOGI(TAG, "open %s", path);
    return true;
}

/// Open entry `i` of the queue and make it the cursor.
bool open_index(int i, bool flush_ring) {
    auto& q = player_core::playlist();
    char path[PATH_MAX_LEN];
    if (i < 0 || !q.path_at(i, path, sizeof(path))) return false;
    q.set_cursor(i);
    return open_track(path, flush_ring);
}

void fail_track() {
    close_file();
    s.tail_len = 0;
    s.pending_seek = -1;
    s.state = 3;
    auto& ring = audio_core::pcm_ring();
    if (ring.owner() == audio_core::PcmRing::OWNER_FILE && ring.generation() == s.ring_generation) {
        ring.set_eos(audio_core::PcmRing::OWNER_FILE, s.ring_generation, true);
        ring.release(audio_core::PcmRing::OWNER_FILE);
    }
}

void open_command(int index) {
    if (index < 0) return;                  // no next/previous entry is a harmless no-op
    if (open_index(index, true)) s.state = 1;
    else fail_track();                     // open_track already disposed of the old FILE*
}

void finish_cancelled_write() {
    if (uxQueueMessagesWaiting(s.q)) return;
    if (s.request_generation.load(std::memory_order_acquire) & player_core::COMMAND_PENDING) {
        sleep_at_least_a_tick(1);          // the lower-priority sender must finish enqueuing
        return;
    }
    close_file();
    s.tail_len = 0;
    s.pending_seek = -1;
    s.state = 0;
    audio_core::pcm_ring().release(audio_core::PcmRing::OWNER_FILE);
}

/// vTaskDelay() rounded UP to a whole tick.
///
/// pdMS_TO_TICKS() truncates, so any delay shorter than one tick becomes ZERO — which it
/// was for every caller here at the old 100 Hz tick, and still is for sub-millisecond ones
/// now. vTaskDelay(0) is not a sleep at all: it yields only to tasks of equal or higher
/// priority, which means a producer sitting above the main loop on the same core hands the
/// CPU straight back to itself and the main loop never runs again. Any delay whose purpose
/// is to let something LOWER-priority run has to block for a real tick, hence the clamp.
///
/// At the 1 kHz tick this build now runs, a tick is 1 ms rather than 10, so these sleeps are
/// a tenth of what they were. The clamp still holds and the stated semantics are unchanged,
/// but a caller that wanted to hand the main loop a whole scheduling quantum now hands it a
/// tenth of one — and this task runs at priority 5, above main. Worth watching in
/// system.main_loop_max_ms.
/// How long this task may hold the CPU before it must give the main loop a turn, and how
/// often it reports what it is achieving.
///
/// Sleeping only when there is nothing to do is not enough. This task outranks the main loop
/// and shares its core, so a decoder that simply cannot get ahead of real time never reaches
/// the "ring is full" branch, never blocks, and runs forever — the main loop stops feeding
/// the task watchdog and the board reboots. That is not a performance problem showing up as
/// a dropout, it is a performance problem showing up as a reboot, which is a far worse way
/// to find out about it.
///
/// With a budget, a decoder that cannot keep up produces underruns instead: audible, counted
/// and survivable. 30 ms against one 10 ms tick still leaves this task three quarters of the
/// core, which is far more than decoding should ever need.
constexpr int64_t WORK_BUDGET_US = 30000;
constexpr int64_t REPORT_US      = 1000000;

/// Slept microseconds since the last report, so the CPU figure can subtract them.
int64_t g_slept_us = 0;

void sleep_at_least_a_tick(uint32_t ms) {
    const TickType_t t = pdMS_TO_TICKS(ms);
    const int64_t t0 = esp_timer_get_time();
    vTaskDelay(t ? t : 1);
    g_slept_us += esp_timer_get_time() - t0;
}

/// What one pump() call managed to do.
///
/// The distinction matters more than it looks. This task runs at priority 5 on core 0 and
/// the main loop runs below it on the same core, so FreeRTOS will never preempt it for
/// `main` — only a voluntary block gives the main loop the CPU back. A pump that returns
/// "nothing to do" and is called again immediately is therefore not a wasted cycle, it is a
/// hang: the main loop stops running, stops feeding the task watchdog, and the board reboots
/// about five seconds later. IDLE exists to force the caller to sleep.
enum class Pump { WORKED, IDLE, DONE, CANCELLED, FAILED };

/// Let the previous track finish before asking for a different clock.
///
/// close_file() deliberately leaves the tail of the track that just ended in the ring — up
/// to half a second that has not been heard — so the next track follows it without a gap.
/// That is right when the next track is at the same rate and wrong when it is not: the
/// consumer retunes the clock for the new rate and the tail plays at it, 8.8 % sharp for
/// 44.1 kHz material followed by 48. So when the rate differs, the ring is left to empty
/// first. eos tells the consumer that an empty ring is the end of the music and not a decoder
/// that fell behind, so it counts no underruns; the caller clears it again right after the
/// question is asked, so the consumer primes a cushion for the new track as it does after
/// any retune. The same rate as the previous track gets the same answer as before and skips
/// the wait, which keeps a whole album at one rate gapless whether or not the clock could be
/// moved to it.
///
/// Bounded: a ring that will not empty — paused, or a consumer that is not running — is
/// flushed instead, which is what a manual track change does anyway. Half a second of a
/// track that was already over is the most that can be lost, and only on that path.
void drain_before_rate_change(audio_core::PcmRing& ring, uint32_t src_rate) {
    if (src_rate == ring.out_rate() || src_rate == s.prev_src_rate) return;
    // The clock is settled for this session, so there is no new clock for the tail to be
    // replayed at and nothing to wait for. This is what makes a mixed-rate playlist gapless:
    // the next track follows the previous one straight through the converter, exactly as a
    // same-rate track follows it straight through.
    if (ring.clock_locked()) return;
    if (ring.available() == 0) return;
    ring.set_eos(audio_core::PcmRing::OWNER_FILE, s.ring_generation, true);
    for (int i = 0; i < 150 && ring.available() > 0; i++) sleep_at_least_a_tick(10);
    if (ring.available() == 0) return;             // eos is cleared by the caller, after asking
    ring.set_eos(audio_core::PcmRing::OWNER_FILE, s.ring_generation, false);
    if (!ring.request_flush(audio_core::PcmRing::OWNER_FILE, s.ring_generation)) return;
    for (int i = 0; i < audio_core::FLUSH_WAIT_NAPS && ring.flush_pending(); i++) sleep_at_least_a_tick(5);
    if (ring.flush_pending()) ring.reset(audio_core::PcmRing::OWNER_FILE, s.ring_generation);
    s.ring_generation = ring.generation();
}

/// Feed the ring at `out`: straight through when it is the track's own rate, converted when
/// not. False when there is no memory for the converter.
bool use_clock(uint32_t out) {
    s.clock = out;
    s.direct = (out == s.src_rate);
    if (!s.direct && !s.asrc.ensure(s.src_rate, out)) return false;
    s.ratio = (double)out / (double)s.src_rate;
    return true;
}

/// Push whatever the ring will take.
Pump pump() {
    auto& ring = audio_core::pcm_ring();
    if (!s.f || !s.dec) return Pump::FAILED;
    if (!s.selected.load() || ring.owner() != audio_core::PcmRing::OWNER_FILE ||
        ring.generation() != s.ring_generation ||
        player_core::command_cancelled(s.request_generation, s.command_generation))
        return Pump::CANCELLED;
    // Leave the ring alone while it is nearly full: decoding ahead only costs latency on a
    // seek, and the card is happier reading in bursts than continuously. This is the common
    // case during steady playback — the decoder is far faster than real time — so it is also
    // the one that must sleep.
    if (ring.space() < CHUNK * 2) return Pump::IDLE;

    // ── top up the compressed buffer ──
    if (!s.file_eof && s.raw_len < RAW_BYTES / 2) {
        const size_t got = fread(s.raw + s.raw_len, 1, RAW_BYTES - s.raw_len, s.f);
        s.raw_len += got;
        if (got == 0) s.file_eof = true;
    }
    if (s.raw_len == 0 && s.file_eof) return Pump::DONE;

    // ── decode one call's worth ──
    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = s.raw;
    raw.len    = (uint32_t)s.raw_len;
    raw.eos    = s.file_eof;      // FLAC's parser needs this to flush its last frame
    esp_audio_simple_dec_out_t out = {};
    out.buffer = s.pcm;
    out.len    = (uint32_t)s.pcm_cap;

    const esp_audio_err_t err = esp_audio_simple_dec_process(s.dec, &raw, &out);
    if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        // Straight after a seek the decoder is looking at the middle of a frame, and saying
        // so is the correct thing for it to do. Give it a few calls to find the next header
        // before treating an error as the end of the track; drop a chunk each time so it
        // has new bytes to look at rather than the same ones.
        if (s.resync > 0) {
            s.resync--;
            const size_t drop = s.raw_len < 512 ? s.raw_len : 512;
            memmove(s.raw, s.raw + drop, s.raw_len - drop);
            s.raw_len -= drop;
            return Pump::WORKED;
        }
        ESP_LOGW(TAG, "decode error %d", (int)err);
        return Pump::DONE;
    }
    if (out.decoded_size) s.resync = 0;      // back in step
    if (raw.consumed) {
        memmove(s.raw, s.raw + raw.consumed, s.raw_len - raw.consumed);
        s.raw_len -= raw.consumed;
    }
    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        // The frame's PCM does not fit, and the decoder keeps it until it does: 24-bit FLAC
        // at 96 kHz or in 5.1 runs past 32 KB. Read as "nothing decoded yet" it was asked
        // again with the same buffer for ever, in silence. Grow as asked and try again, or
        // end the track.
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
        return Pump::DONE;
    }
    if (!raw.consumed && !out.decoded_size && s.file_eof) {
        return Pump::DONE;                // nothing consumed, nothing produced, no more input
    }
    // Parser still gathering a frame. Consuming input is progress; consuming nothing is not,
    // and calling straight back in would spin.
    if (!out.decoded_size) return raw.consumed ? Pump::WORKED : Pump::IDLE;

    // ── learn the format from the first decoded frame ──
    if (!s.src_rate) {
        esp_audio_simple_dec_info_t info = {};
        if (esp_audio_simple_dec_get_info(s.dec, &info) == ESP_AUDIO_ERR_OK && info.sample_rate) {
            s.src_rate = info.sample_rate;
            s.src_bits = info.bits_per_sample;
            s.src_ch   = info.channel ? info.channel : 2;
            s.ratio    = (double)OUT_RATE / (double)s.src_rate;
            snprintf(s.fmt, sizeof(s.fmt), "%u/%.1f kHz %u ch",
                     (unsigned)s.src_bits, (double)s.src_rate / 1000.0, (unsigned)s.src_ch);
            // Ask for the source's own rate, then WAIT to hear whether it was granted.
            // The output follows the source, so 44.1 kHz material is normally played at
            // 44.1 kHz with nothing converted — but not every rate can be made from the
            // audio PLL, and deciding not to convert before the answer comes back leaves a
            // stream of one rate feeding a clock running at another. The wait costs a
            // fraction of the cushion the consumer is priming anyway.
            auto& ring = audio_core::pcm_ring();
            drain_before_rate_change(ring, s.src_rate);
            ring.ask_rate(s.src_rate);
            // Asked first, cleared second: with the question already open the consumer
            // reads an empty ring as the wait it is, not as an underrun, and by the time it
            // has retuned and looks for a cushion, eos has long been false again.
            ring.set_eos(audio_core::PcmRing::OWNER_FILE, s.ring_generation, false);
            uint32_t out = 0;
            for (int i = 0; i < 60 && !(out = ring.out_rate()); i++) {
                sleep_at_least_a_tick(5);
            }
            if (!out) out = audio_core::PIPE_OUT_RATE;   // no answer yet; start on the fallback
            if (!use_clock(out)) return Pump::DONE;
            // How long the track is. The decoder reports a bitrate for compressed formats;
            // for PCM it reports none, and the rate times the frame size IS the bitrate.
            // File size over bitrate counts tags and headers as audio, which for a 3-minute
            // MP3 with artwork is a second or two — visible only if you are looking for it,
            // and far better than a progress bar with no end.
            uint32_t bps_rate = info.bitrate;
            // The field is documented as bits per second, and for MP3 it arrives in
            // kilobits: a 200 kbit/s track reported 201, which turned a four-minute song
            // into seventy-seven hours. Nothing this player decodes runs below 10 kbit/s,
            // so a number that small can only be the other unit.
            if (bps_rate && bps_rate < 10000) bps_rate *= 1000;
            if (!bps_rate) bps_rate = s.src_rate * s.src_ch * s.src_bits;
            if (bps_rate >= 8 && s.file_bytes) {
                s.dur_s.store((int32_t)(s.file_bytes * 8 / bps_rate), std::memory_order_relaxed);
            }
            if (s.wav_data.valid()) {
                s.dur_s.store((int32_t)(s.wav_data.bytes / s.wav_data.frame_bytes /
                                       s.wav_data.rate_hz), std::memory_order_relaxed);
            }
            ESP_LOGI(TAG, "format: %s, ratio %.4f%s, %ld s", s.fmt, s.ratio,
                     s.direct ? " (no resampling)" : " (fixed-ratio resampler)",
                     (long)s.dur_s.load(std::memory_order_relaxed));
        } else {
            return Pump::WORKED;          // wait for the decoder to know; input did move
        }
    }

    // ── PCM → float → 48 kHz → ring ──
    size_t done = 0;
    const size_t bps = (size_t)(s.src_bits / 8) * s.src_ch;
    const size_t total_frames = bps ? out.decoded_size / bps : 0;

    // The answer the wait above gave up on, or a later one: convert for the clock the output
    // is really at, not the fallback the track started on (audio_core::clock_moved()).
    const uint32_t answer = ring.out_rate();
    if (audio_core::clock_moved(s.clock, answer)) {
        ESP_LOGW(TAG, "output at %lu Hz, not the %lu assumed; converting for it now",
                 (unsigned long)answer, (unsigned long)s.clock);
        if (!use_clock(answer)) return Pump::DONE;
    }

    // How much input the converter may be given at a time. It produces s.ratio frames
    // for every frame it takes — 2.18 of them going from 22.05 kHz to 48 — and rbuf holds
    // a fixed CHUNK * 2. Handed more than that fits, the converter fills the buffer, stops,
    // and reports how much of the input it actually used; the caller advanced past the
    // whole block regardless, so one frame in twelve was dropped on the floor without a
    // word. Cutting the input to what the output can hold is the same work in more calls.
    const size_t in_max = audio_core::resample_input_limit(
        CHUNK, CHUNK * 2, s.direct ? 1.0 : s.ratio);

    while (done < total_frames) {
        const size_t n = pcm_to_f32(s.pcm + done * bps, (total_frames - done) * bps,
                                    s.src_bits, s.src_ch, s.fbuf, in_max);
        if (!n) break;
        done += n;

        const float* src = s.fbuf;
        size_t src_frames = n;
        if (!s.direct) src = s.asrc.process(s.fbuf, n, s.rbuf, CHUNK * 2, src_frames);
        if (!push_frames(const_cast<float*>(src), src_frames)) return Pump::CANCELLED;
        s.frames_out += src_frames;
        // Produced is half a second ahead of heard — the ring is the buffer that makes
        // playback survive a slow card, and counting its contents as already played is what
        // made the position run ahead of the music and the bar reach the end early.
        const uint64_t queued = audio_core::pcm_ring().available() + s.tail_len;
        const uint64_t heard  = s.frames_out > queued ? s.frames_out - queued : 0;
        s.pos_s.store((int32_t)(heard / fs()), std::memory_order_relaxed);
    }
    return Pump::WORKED;
}

void player_task(void*) {
    CmdMsg m;
    int64_t yield_deadline = 0;    // hold the CPU no longer than this
    int64_t report_at      = 0;
    int64_t busy_us        = 0;
    uint64_t report_base   = 0;
    int      silent_run    = 0;    // tracks in a row that ended without a frame (Pump::DONE)
    for (;;) {
        modesp::liveness::tick(s.beat);
        const bool got = (xQueueReceive(s.q, &m, pdMS_TO_TICKS(s.state == 1 ? 0 : 50)) == pdTRUE);
        if (!s.run) break;
        if (got) {
            if (interrupts_pcm(m.type)) {
                s.command_generation = m.generation;
                silent_run = 0;
            }
            switch (m.type) {
            case Cmd::PLAY:
                cue_path(m.path);
                if (open_track(m.path, true)) s.state = 1;
                else fail_track();
                break;
            // Index the card and stop: what the player does at startup. The path, when
            // there is one, is the track that was playing when the power went off, and the
            // cursor lands on it — ready to continue, without starting to make noise.
            case Cmd::QUEUE:
                index_card(m.path[0] ? m.path : nullptr);
                break;
            case Cmd::SEEK:
                if (s.state == 1 && s.dur_s.load(std::memory_order_relaxed) <= 0) s.pending_seek = m.value;
                else seek_to(m.value);
                break;
            // Pause the CONSUMER, not the decoder. Stopping the decoder leaves two seconds
            // already queued, which play out before anything appears to happen — and then
            // end mid-waveform. Pausing the ring is immediate and keeps the queue, so resume
            // is immediate too.
            case Cmd::PAUSE:  if (s.state == 1) { s.state = 2; audio_core::pcm_ring().set_paused(true); } break;
            case Cmd::RESUME: if (s.state == 2) { s.state = 1; audio_core::pcm_ring().set_paused(false); } break;
            case Cmd::TOGGLE:
                if (s.state == 1)      { s.state = 2; audio_core::pcm_ring().set_paused(true); }
                else if (s.state == 2) { s.state = 1; audio_core::pcm_ring().set_paused(false); }
                break;
            case Cmd::STOP:
                close_track();
                s.pending_seek = -1;
                s.state = 0;
                audio_core::pcm_ring().set_eos(audio_core::PcmRing::OWNER_FILE, s.ring_generation, true);
                audio_core::pcm_ring().release(audio_core::PcmRing::OWNER_FILE);
                break;
            case Cmd::NEXT:
                open_command(player_core::playlist().next(s.shuffle.load(), s.repeat.load()));
                break;
            case Cmd::PREV:
                // The convention every player uses: "previous" early in a track means the
                // one before, later in a track means the start of this one. Three seconds is
                // where the two readings stop competing.
                if (s.frames_out > 3 * fs()) {
                    open_command(player_core::playlist().cursor());
                } else {
                    open_command(player_core::playlist().previous(s.shuffle));
                }
                break;
            }
        }
        if (!s.selected.load(std::memory_order_acquire) && (s.state == 1 || s.state == 2)) {
            close_file();
            s.tail_len = 0;
            s.pending_seek = -1;
            s.state = 0;
            audio_core::pcm_ring().release(audio_core::PcmRing::OWNER_FILE);
        }
        if (s.pending_seek >= 0 && s.dur_s.load(std::memory_order_relaxed) > 0) {
            const int32_t at = s.pending_seek;
            s.pending_seek = -1;
            seek_to(at);
        }
        if (s.state == 1) {
            const int64_t t0 = esp_timer_get_time();
            if (!yield_deadline) {                 // first pass of a track
                yield_deadline = t0 + WORK_BUDGET_US;
                report_at      = t0 + REPORT_US;
                busy_us        = 0;
                report_base    = s.frames_out;
            }
            g_slept_us = 0;          // count only what pump() itself sleeps
            const Pump result = pump();
            const int64_t now = esp_timer_get_time();
            // pump() can block inside, waiting for room in the ring. Timing the call from
            // the outside counts that sleep as work and reports a decoder that looks four
            // times more expensive than it is — a number I then have to argue with instead
            // of use.
            busy_us += (now - t0) - g_slept_us;

            // Frames produced against wall clock, not against CPU time: "1.00x" means the
            // decoder is exactly keeping up with playback and has no margin left for a slow
            // card read. That is the number that decides whether a track plays or stutters,
            // and it was invisible until now.
            if (now >= report_at) {
                // frames_out restarts at zero with each track, so a window containing a
                // switch would subtract a larger number from a smaller one and print a
                // figure with fifteen digits — at precisely the moment worth reading.
                const uint64_t now_frames = s.frames_out;
                const double made = (now_frames >= report_base)
                                  ? (double)(now_frames - report_base) / (double)fs()
                                  : (double)now_frames / (double)fs();
                // Ring occupancy is the number that actually predicts a dropout. "1.01x" is
                // what a HEALTHY producer reports once the ring is full and the consumer
                // sets the pace, and it is also what a producer that is only just keeping up
                // reports while the ring hovers near empty. The two sound completely
                // different and read identically without this.
                auto& rg = audio_core::pcm_ring();
                const int fill = rg.capacity()
                               ? (int)(100 - 100 * rg.space() / rg.capacity()) : 0;
                ESP_LOGI(TAG, "decode %.2fx real time, %.0f%% cpu, ring %d%%",
                         made, 100.0 * (double)busy_us / (double)REPORT_US, fill);
                report_at   = now + REPORT_US;
                busy_us     = 0;
                report_base = s.frames_out;
            }

            switch (result) {
            case Pump::FAILED:
                fail_track();
                break;
            case Pump::CANCELLED:
                // A queued seek/track command needs the old position (notably PREV's
                // three-second rule). Keep it until that command is consumed next turn.
                finish_cancelled_write();
                break;
            case Pump::WORKED:
                // Voluntary, because nothing else can take the CPU away from this task.
                if (now >= yield_deadline) {
                    sleep_at_least_a_tick(1);
                    yield_deadline = esp_timer_get_time() + WORK_BUDGET_US;
                }
                break;
            case Pump::IDLE:
                // The ring is full: the one moment in a track when there is time to spare,
                // and the only moment worth spending on a picture.
                load_cover();
                // Not an optimisation — see the comment on Pump. Without this sleep the loop
                // never yields to the main task below it and the watchdog reboots the board
                // a few seconds after the ring first fills, which is to say a few seconds
                // after playback starts working.
                sleep_at_least_a_tick(5);
                yield_deadline = esp_timer_get_time() + WORK_BUDGET_US;
                break;
            case Pump::DONE: {
                ESP_LOGI(TAG, "end of %s", s.path);
                if (!release_tail()) { finish_cancelled_write(); break; }
                // Not one frame came out of it: an empty file, or one the decoder cannot
                // read. It must not be what plays next — repeat one, or repeat all over a
                // single track, reopened it at once with no card read to wait on, and the
                // task watchdog rebooted the panel — and a card of nothing but such files
                // must not go round for ever, playing silence.
                const bool silent = s.frames_out == 0;
                silent_run = silent ? silent_run + 1 : 0;
                close_file();                  // the ring keeps the tail; it has not played
                auto& q = player_core::playlist();
                int nxt = q.advance(s.shuffle.load(), s.repeat.load());
                if (silent && nxt == q.cursor()) nxt = q.next(s.shuffle.load(), s.repeat.load());
                if (silent && silent_run >= q.count()) {
                    ESP_LOGW(TAG, "nothing played in the last %d tracks: stopped", silent_run);
                    nxt = -1;
                }
                if (nxt >= 0 && open_index(nxt, false)) {
                    s.state = 1;
                } else {
                    // Nothing follows: tell the audio core the stream really has ended, so a
                    // draining ring reads as the end of the music and not as an underrun.
                    audio_core::pcm_ring().set_eos(audio_core::PcmRing::OWNER_FILE, s.ring_generation, true);
                    s.state = 0;
                }
                // A tick between one track and the next, whatever happened: the next pass may
                // end at once again, and only a voluntary block lets main run (see Pump).
                sleep_at_least_a_tick(1);
                yield_deadline = 0;
                break;
            }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    s.exited = true;
    vTaskDelete(nullptr);
}

} // namespace

PlayerModule::PlayerModule()
    : BaseModule("player", modesp::ModulePriority::LOW)
{}

bool PlayerModule::on_init() {
    state_set("player.state", "idle");
    state_set("player.track", "");
    state_set("player.title", "");
    state_set("player.artist", "");
    state_set("player.album", "");
    state_set("player.format", "");
    state_set("player.pos_s", (int32_t)0);
    state_set("player.dur_s", (int32_t)0);
    state_set("player.index", (int32_t)0);
    state_set("player.count", (int32_t)0);
    // What PersistService restored, not the defaults: both keys are persisted, and writing
    // them here undid repeat and shuffle at every boot. on_update range-checks the repeat.
    state_set("player.repeat", read_int("player.repeat", 0));
    state_set("player.shuffle", read_bool("player.shuffle", false));
    state_set("player.seek_s", (int32_t)-1);

    if (!player_core::playlist().init()) {
        ESP_LOGE(TAG, "playlist allocation failed — local playback disabled");
        return true;
    }
    if (!audio_core::pcm_ring().init(RING_FRAMES)) {
        ESP_LOGE(TAG, "PCM ring allocation failed — local playback disabled");
        return true;
    }
    // Two registries, and both are needed. esp_audio_simple_dec_register_default() only
    // registers the CONTAINER parsers — WAV, M4A, TS, OGG. The elementary streams, MP3 and
    // FLAC among them, live in the esp_audio_dec registry, which the simple layer reaches
    // through its ES parser. Registering only the first one left the two formats this player
    // exists for reporting "decoder not built into this firmware" while WAV worked, which
    // reads like a build option and is not one.
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder registration failed — local playback disabled");
        return true;
    }

    s.raw  = (uint8_t*)heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.pcm  = (uint8_t*)heap_caps_malloc(PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.fbuf = (float*)heap_caps_malloc(CHUNK * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s.rbuf = (float*)heap_caps_malloc(CHUNK * 2 * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Built per track in pump(), once the decoder reports the source rate.
    if (!s.raw || !s.pcm || !s.fbuf || !s.rbuf) {
        ESP_LOGE(TAG, "buffer allocation failed — local playback disabled");
        return true;
    }

    // Almost all MP3 and FLAC is 44.1 kHz, so build that bank now. A hundred
    // milliseconds during startup is invisible; the same hundred milliseconds when the first
    // track starts is a crackle.
    // Pre-build the one bank a card of music is most likely to need, so the first track of
    // the evening does not pay for it. 44.1 to 48 is 160 filters; the pair that used to be
    // pre-built here is the pair that is now least likely to be used, because a rate below
    // the DAC's floor goes to 44.1 rather than to 48 (see fallback_rate in pcm_ring.h).
    s.asrc.ensure(44100, OUT_RATE);

    s.q = xQueueCreate(4, sizeof(CmdMsg));
    // The artwork worker (CoverJob): a queue of one, a PSRAM stack, core 0. Idle priority
    // rather than 1, because the software decoder runs at its caller's priority and never
    // blocks: at 1 it would keep IDLE0 from running for the whole decode, stretched further
    // by every task it then shares core 0 with, against a 5 s task watchdog that panics. At
    // 0 IDLE0 still gets its turn every tick, and everything else - this task at 5, the UI
    // and the main loop at 1 - comes first. Once, however often on_init() runs.
    if (!cover_q) {
        cover_q = xQueueCreate(1, sizeof(CoverJob));
        if (!cover_q ||
            xTaskCreatePinnedToCoreWithCaps(cover_worker, "player_cover", COVER_STACK, nullptr,
                                            tskIDLE_PRIORITY, nullptr, 0,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGW(TAG, "no artwork worker: covers are decoded on the player task");
            if (cover_q) vQueueDelete(cover_q);
            cover_q = nullptr;
        } else {
            // 30 s: a decode at idle priority takes what the rest of core 0 leaves it.
            modesp::liveness::watch("player_cover", &cover_beat, 30000);
        }
    }
    // Core 0: the audio task owns core 1, and decoding must never compete with it. Priority
    // above the main loop but far below audio — a late decode costs buffer, a late audio
    // block costs a click.
    // 8 KB rather than 6: the card is walked recursively at startup. Covers are decoded on
    // the artwork worker above, and a software decode on the cover component's own stack:
    // a PNG needs more than this whole task has (SOFT_STACK in components/cover/src/cover.cpp).
    xTaskCreatePinnedToCore(player_task, "player", 8 * 1024, nullptr, 5, &s.task, 0);
    // 30 s, not the 60 s this module's LOW priority would imply: the loop turns over every
    // 50 ms when idle, and the longest thing it legitimately does in one turn is walk the
    // whole card at startup.
    if (s.task) modesp::liveness::watch("player", &s.beat, 30000, this);
    ESP_LOGI(TAG, "ready: ring %u frames (%.1f s), MP3/FLAC/WAV",
             (unsigned)RING_FRAMES, (double)RING_FRAMES / OUT_RATE);
    return true;
}

void PlayerModule::on_update(uint32_t dt_ms) {
    if (!s.q) return;

    // Once, on the first pass: index the card, and put the cursor back on the track that
    // was playing when the power went off. Nothing is played — turning a player on should
    // not start making noise, but it should be one button away from continuing.
    if (!restored_) {
        restored_ = true;
        // Resolve the resume point once, on the first pass: /userdata when the store is
        // up (UserStore::begin() ran during config init), /data otherwise.
        modesp::UserStore::write_path("last_track", s_last_track_path,
                                      sizeof(s_last_track_path));
        char rel[PATH_MAX_LEN] = "";
        if (FILE* f = fopen(s_last_track_path, "r")) {
            if (fgets(rel, sizeof(rel), f)) {
                char* nl = strchr(rel, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
        }
        snprintf(last_dir_, sizeof(last_dir_), "%.*s", (int)sizeof(last_dir_) - 1, rel);
        CmdMsg m{};
        m.type = Cmd::QUEUE;
        snprintf(m.path, sizeof(m.path), "%.*s", (int)sizeof(m.path) - 1, rel);
        queue_command(m);
    }

    // A position on the progress bar, written by the screen or by anything else that can
    // reach the state. −1 is "nothing asked for", which is also what this puts back.
    {
        const int32_t seek = read_int("player.seek_s", -1);
        if (seek >= 0) {
            state_set("player.seek_s", (int32_t)-1);
            CmdMsg m{};
            m.type  = Cmd::SEEK;
            m.value = seek;
            queue_command(m);
        }
    }

    // The output belongs to whichever source is selected. When it is no longer this one —
    // the radio was tuned, or somebody chose AUX — stop rather than keep decoding into a
    // ring nobody reads, and give the ring up so the other producer can have it.
    const bool ours = read_int("audio.source", 0) == AUDIO_SRC_FILE;
    s.selected.store(ours, std::memory_order_release);
    const bool chosen = ours && !was_ours_ && seen_source_;
    was_ours_ = ours;
    seen_source_ = true;
    if (!ours && playing_) {
        // STOP is what releases the ring, and it also closes the track and forgets where it
        // was. So the position is remembered HERE, where the decision to give the output up
        // is made, rather than asked of a player that has already been told to forget.
        // The playlist cursor, not player.track: that key holds what was last ASKED for, and
        // after an auto-advance the card is two tracks further on. The cursor is what
        // player.dir is written from a few lines below, for the same reason.
        {
            auto& q = player_core::playlist();
            q.rel_at(q.cursor(), yield_track_, sizeof(yield_track_));
        }
        yield_pos_ = read_int("player.pos_s", 0);
        yielded_   = yield_track_[0] != '\0';
        playing_   = false;
        CmdMsg m{};
        m.type = Cmd::STOP;
        queue_command(m);
    } else if (ours && yielded_) {
        // Chosen again, and nothing else stopped it: the user never pressed stop, the output
        // was lent out and has now come back. Put the track back on rather than making them
        // find it a second time. The seek goes through the same key the progress bar writes,
        // so it is queued behind this PLAY on the next pass and arrives after the file is
        // open — which is the only ordering that works.
        yielded_ = false;
        playing_ = true;
        // Static, not a local: a CmdMsg carries a 257-byte path, this runs on the main task's
        // 8 KB stack, and each such local in a branch of on_update() is another 270 bytes of
        // the margin the service page grades (it went red at 808 B). Only the main task runs
        // this, and xQueueSend copies the message out before returning.
        static CmdMsg m;
        m = CmdMsg{};
        m.type = Cmd::PLAY;
        // PLAY opens what it is given with fopen, so it needs the whole path; the remembered
        // track is relative to the card, which is what survives a rescan. Passing it as it
        // was said "cannot open" and the card came back silent.
        auto& q = player_core::playlist();
        const int at = q.find(yield_track_);
        if (at >= 0 && q.path_at(at, m.path, sizeof(m.path))) {
            queue_command(m);
            if (yield_pos_ > 1) state_set("player.seek_s", yield_pos_);
        } else {
            playing_ = false;                     // the track is gone: nothing to put back
        }
    } else if (chosen && !playing_) {
        // Chosen with nothing lent out to give back — on the Source page, over MQTT, from the
        // web — and choosing the card is choosing to listen to it: the track the cursor is on,
        // from its start. Not at power-on: the source found on the first pass is not a choice,
        // and turning the panel on still makes no noise (restored_, above). Without this, the
        // card chosen on the Source page would sit silent until a track was picked.
        auto& q = player_core::playlist();
        static CmdMsg m;                          // static for the reason given above
        m = CmdMsg{};
        if (q.count() > 0 && q.path_at(q.cursor(), m.path, sizeof(m.path))) {
            m.type = Cmd::PLAY;
            playing_ = true;
            queue_command(m);
        }
    }

    // A track is requested by writing its path; the command then says what to do with it.
    //
    // A loop, not an `if`: player.cmd is a queue now (BaseModule::post_command), so two
    // presses inside one tick are two commands rather than one. As a shared state key the
    // second overwrote the first before this ever ran.
    char want[PATH_MAX_LEN] = "";
    read_string("player.track", want, sizeof(want));
    modesp::BaseModule::Command pc;
    while (next_command(pc)) {
        const int32_t cmd = pc.value;
        if (cmd == 0) continue;
        CmdMsg m{};
        switch (cmd) {
        case 1:
            if (want[0]) { m.type = Cmd::PLAY; snprintf(m.path, sizeof(m.path), "%s", want); }
            else         { m.type = Cmd::RESUME; }
            break;
        case 2: m.type = Cmd::PAUSE;  break;
        case 3: m.type = Cmd::STOP;   break;
        case 4: m.type = Cmd::NEXT;   break;
        case 5: m.type = Cmd::PREV;   break;
        case 6: m.type = Cmd::TOGGLE; break;
        default: continue;
        }
        if (m.type == Cmd::PLAY || m.type == Cmd::RESUME) {
            // Decoding into the ring is only half the job. The DSP keeps playing whichever
            // source is selected, so starting a track has to take the output as well —
            // without this the file decodes perfectly into a ring nobody reads, and the
            // amplifier stays on the previous source. That is exactly what "playback does
            // not start" looked like.
            state_set("audio.source", AUDIO_SRC_FILE);
            s.selected.store(true, std::memory_order_release);
            playing_ = true;
            yielded_ = false;      // an explicit press outranks a remembered lend
        }
        queue_command(m);
    }

    // Settings, not commands: read every pass so a change takes effect on the next track
    // boundary rather than on the next press.
    int32_t rep = read_int("player.repeat", 0);
    if (rep < 0 || rep > 2) rep = 0;
    s.repeat.store((player_core::Repeat)(uint8_t)rep);
    s.shuffle.store(read_bool("player.shuffle", false));

    report_ms_ += dt_ms;
    if (report_ms_ < 500) return;
    report_ms_ = 0;

    // Remember the folder across a power cycle. Written HERE, from the main loop: a
    // LittleFS write is tens of milliseconds and the player task cannot afford them — it
    // would be eating the ring's margin to record something nobody needs this second.
    // Compared first, so a card played end to end writes flash once, not once per track.
    {
        // Which track, so the player comes back to it rather than to the top of the card.
        char rel[player_core::Playlist::MAX_REL] = "";
        auto& q = player_core::playlist();
        q.rel_at(q.cursor(), rel, sizeof(rel));
        state_set("player.dir", rel);
        if (rel[0] && strcmp(rel, last_dir_) != 0) {
            snprintf(last_dir_, sizeof(last_dir_), "%.*s", (int)sizeof(last_dir_) - 1, rel);
            // write -> read back -> rename (safe_file). A truncated resume
            // point is a path to a file that does not exist, and the queue is rebuilt from
            // it at boot — so the failure mode is "the card comes up empty", not "the
            // cursor is in the wrong place".
            if (!safe_file::save(s_last_track_path, last_dir_, strlen(last_dir_))) {
                ESP_LOGW(TAG, "cannot write %s", s_last_track_path);
            }
        }
    }

    static const char* const NAMES[] = {"idle", "playing", "paused", "error"};
    const int st = s.state < 0 || s.state > 3 ? 0 : s.state;
    state_set("player.state", NAMES[st]);
    state_set("player.format", s.fmt);
    state_set("player.pos_s", s.pos_s.load(std::memory_order_relaxed));
    state_set("player.dur_s", s.dur_s.load(std::memory_order_relaxed));
    // The tag if the file has one, the file name if it does not: better an honest file
    // name than an empty line where a track title belongs.
    const char* slash = strrchr(s.path, '/');
    state_set("player.title", s.tags.title[0] ? s.tags.title : (slash ? slash + 1 : s.path));
    state_set("player.artist", s.tags.artist);
    state_set("player.album", s.tags.album);
    state_set("player.index", (int32_t)player_core::playlist().cursor());
    state_set("player.count", (int32_t)player_core::playlist().count());
}

void PlayerModule::on_stop() {
    modesp::liveness::unwatch("player");
    s.run = false;
    // Joined, not napped past: the task can sit seconds in a soft cover decode, and
    // load_cover() goes back to s.f when that returns — close_track() after a fixed 60 ms
    // left it seeking a FILE that was closed under it. A task still busy after 500 ms keeps
    // its file and decoder; the board is going down, and they go with it.
    if (s.task) {
        for (int waited = 0; !s.exited && waited < 500; waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!s.exited) {
            ESP_LOGW(TAG, "stop: task still busy after 500 ms, track left open");
            return;
        }
    }
    close_track();
}
