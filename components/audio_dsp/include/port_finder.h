/**
 * @file port_finder.h
 * @brief A vented speaker's port frequency, from a microphone held at its woofer.
 *
 * A centimetre or two from a woofer's cone a microphone hears the cone and little else, and a
 * vented box's cone stands nearly still at the box's tuning frequency fb: the air in the port
 * does the moving there. So the near-field response has a deep, narrow dip at fb, with the
 * cone's own rise on both sides of it — towards the passband above, and below fb where the cone
 * comes unloaded (D. B. Keele, "Low-Frequency Loudspeaker Assessment by Nearfield Sound-Pressure
 * Measurement", JAES 22(3), 1974).
 *
 * The sweep plays one sine at a time; ToneMeter measures what the microphone heard at that
 * frequency, and the same with nothing playing gives the noise there (mains hum included);
 * find_port() picks the dip. Pure arithmetic, no I/O: host-tested in tests/host/test_port_finder.cpp,
 * driven on the panel by modules/audio (AudioModule::port_meter_loop).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace audio_core {

/// The amplitude of one sine in a stream of samples: a lock-in over a Hann window — the samples
/// times a phasor at the frequency, summed, the window keeping a nearby sine (mains hum, say)
/// from leaking in more than its sidelobes allow. Float, with the phasors renormalised every
/// 256 samples: over a second the phase error stays under a milliradian.
class ToneMeter {
public:
    /// Measure the sine at `hz` in the next `n` samples, `fs` samples a second.
    void start(float hz, float fs, uint32_t n);
    /// Feed samples; consumes at most what the window still wants and returns how many.
    size_t push(const float* x, size_t m);
    bool done() const { return pos_ >= n_; }
    /// The sine's peak amplitude, in the samples' units (valid once done()).
    float amplitude() const;
private:
    float ref_r_ = 1.0f, ref_i_ = 0.0f, step_r_ = 1.0f, step_i_ = 0.0f;   ///< e^(−jωt)
    float win_r_ = 1.0f, win_i_ = 0.0f, wstep_r_ = 1.0f, wstep_i_ = 0.0f; ///< e^(j2πt/T)
    float acc_r_ = 0.0f, acc_i_ = 0.0f, wsum_ = 0.0f;
    uint32_t n_ = 0, pos_ = 0;
};

/// What find_port() concluded.
struct PortFit {
    enum Status : int {
        OK = 0,      ///< a clear dip: trust it
        WEAK,        ///< a dip, but shallow or close to the noise: show it, do not apply it
        NO_DIP,      ///< nothing dips: a closed box, or the microphone is too far from the cone
        NOT_HEARD,   ///< the microphone barely heard the sweep at all
    };
    Status status = NOT_HEARD;
    float hz = 0.0f;         ///< the dip, interpolated between grid points
    float depth_db = 0.0f;   ///< how far the curve climbs from it within the octave above
    float below_db = 0.0f;   ///< and within the octave below (the cone's unloading rise)
    float snr_db = 0.0f;     ///< signal over noise at the dip
    float loud_db = 0.0f;    ///< the sweep's loudest point over the noise there
};

/// The dip in a near-field sweep. `hz` ascending; `level` what the microphone heard at each
/// frequency per unit of drive; `noise` what it heard there with nothing playing, through the
/// same window (both linear amplitudes, same units). Noise power is taken off, then the curve is
/// smoothed over three points (a single noisy point makes no dip). A dip is a local minimum the
/// curve climbs from by 6 dB or more within the octave above (towards the passband) and by
/// 1.5 dB or more within the octave below (the cone's own rise there, which can be small: a
/// closed box, or a roll-off, has none at all). OK — clear enough to apply unasked — takes 12 dB
/// above, 6 dB below and 15 dB over the noise; anything less is WEAK. Port sound reaching the
/// microphone moves the dip: on a modelled rear-ported box (port 5 % of the cone's near-field
/// level) 3-4 % low; through a room mode at fb it can fill the dip and push it up (an SC-G101:
/// 67.7 Hz measured, the cone stood still at 63). A finger on the dust cap settles it.
PortFit find_port(const float* hz, const float* level, const float* noise, int n);

/// The sweep's frequencies: whole hertz (the tone takes integers), about 1/24 octave apart,
/// from `lo` to `hi`, skipping 50 and 60 Hz — mains hum sits exactly there, in phase with
/// nothing, and a tone on top of it would add or cancel instead of summing powers. Returns how
/// many it wrote (at most `max`).
int port_sweep_grid(int lo, int hi, int* out, int max);

}  // namespace audio_core
