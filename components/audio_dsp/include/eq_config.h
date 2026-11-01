/**
 * @file eq_config.h
 * @brief Read and write /data/eq.json — the DSP configuration, sectioned by output.
 *
 * Shared by the audio core (which runs the filters) and the UI (which edits them), for the
 * same reason the filter maths is shared: one parser, so the file the screen writes is by
 * construction the file the audio core reads.
 *
 * The whole file is modelled, not just the section in use. eq_save() regenerates it from
 * this struct, so anything the model does not carry would be silently lost on the first
 * edit — that is why sections, crossover and mode are all here even though only section 0
 * and mode "2.0" do anything today (docs/CONCEPT.md, "Outputs").
 */

#pragma once

#include <atomic>

#include "dsp_chain.h"
#include <cstddef>

namespace audio_core {

static constexpr int MAX_SECTIONS = 2;   ///< two DACs; see docs/CONCEPT.md, "Outputs"

/// The shape of eq.json this build understands. Written by eq_serialize(), checked by
/// eq_parse(): a file from the future is REFUSED rather than read with today's rules.
///
/// A missing "version" means 1 — the field was added before anything shipped, and a file
/// without it is either hand-written or from the very first layout; both parse correctly
/// under the current rules, so absence is not an error. A HIGHER number is: the fields this
/// parser does not know about would be dropped, the file would then be rewritten from what
/// was understood on the first EQ edit, and the rest would be gone for good. Refusing leaves
/// a flat EQ and a log line, which is recoverable.
static constexpr int EQ_SCHEMA_VERSION = 2;

/// One output section: a DAC pair. What it means follows EqConfig::mode — a way of one
/// speaker in biamp/2.1, an independent pair in 4.0.
struct EqSection {
    char   id[12]     = "";
    char   label[28]  = "";
    float  gain_db    = 0.0f;
    float  delay_ms   = 0.0f;    ///< time alignment; only meaningful inside one clock domain
    bool   invert     = false;
    EqBand bands[MAX_BANDS];
    int    count      = 0;
};

struct EqConfig {
    char      mode[8]      = "2.0";   ///< 2.0 | biamp | 4.0 | 2.1
    float     crossover_hz = 2500.0f; ///< unused in 2.0 and 4.0
    EqSection sec[MAX_SECTIONS];
    int       sections     = 0;
    /// The sound the file was saved as: its "preset", an id (eq_presets.h). A hint and no more —
    /// the bands are what plays (EqStore::load). Empty in a file an older build wrote, and
    /// for an id that is not plain lower-case ASCII.
    char      preset[24]   = "";
};

/// Carries an edited band set to the audio task, in memory.
///
/// The audio task used to re-read /data/eq.json whenever something changed. That is a
/// filesystem read on the real-time path, which the standard advice for audio threads rules
/// out along with locks and allocation — and on this chip a flash WRITE elsewhere disables
/// the cache, so such a read can stall for as long as that write takes. It also meant a band
/// could not be heard until the file had been written, tying how fast the EQ responds to how
/// often flash is erased.
///
/// The documented shape for this is a lock-free single-producer/single-consumer handoff
/// where the reader never waits and falls back to the values it already has. A seqlock gives
/// that: the writer bumps a counter around the copy, and a reader that catches a set
/// half-written retries rather than designing biquads from two different edits.
///
/// std::atomic with release/acquire rather than the volatile-plus-barrier of SpectrumBus:
/// this one crosses cores — the main loop writes it, the audio task on the other core reads
/// it — and that is what the ordering guarantees are for.
class EqBus {
public:
    void publish(const EqBand* bands, int count);
    /// False when nothing has been published yet, or when the writer kept winning. The
    /// caller keeps the bands it already has, which is the correct fallback on this path.
    bool read(EqBand* bands, int& count) const;

private:
    mutable std::atomic<uint32_t> seq_{0};
    EqBand bands_[MAX_BANDS];
    int    count_ = 0;
};

EqBus& eq_bus();

/// Parse `path`. Returns false if the file is missing or unparseable, leaving `cfg` at its
/// defaults (a flat EQ), which is the safe state for an audio path.
bool eq_load(const char* path, EqConfig& cfg);

/// Rewrite `path` from `cfg`, whole. Writes to a temporary and renames, so a power cut
/// during the write leaves the previous configuration intact rather than a truncated file.
bool eq_save(const char* path, const EqConfig& cfg);

/// Copy `src` over `dst` as it is, byte for byte, through the same write-and-rename — the copy
/// an older build's file is kept as (eq_store.h). False when `src` is missing, does not fit
/// the read buffer whole, or `dst` could not be written; `dst` is then as it was. eq_save()'s
/// buffer and its stack: one caller at a time, the EQ's owner.
bool eq_copy_file(const char* src, const char* dst);

/// Serialise into `buf` without touching the filesystem — used by eq_save() and by tests.
/// Returns the number of bytes written, or 0 if it would not fit.
size_t eq_serialize(const EqConfig& cfg, char* buf, size_t buf_sz);

/// Parse from memory. Same contract as eq_load().
bool eq_parse(const char* json, size_t len, EqConfig& cfg);

/// A band type as eq.json spells it ("peak", "lowshelf", …) — also the web's word for it.
const char* eq_type_name(BandType t);
/// The type a name stands for; false for a name that is none, leaving `out` alone.
bool eq_type_from_name(const char* name, size_t len, BandType& out);

} // namespace audio_core
