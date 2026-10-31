/**
 * @file uac_tone.h
 * @brief One clean tone, followed through the recurrence a sampled sinusoid obeys — the U0
 *        stream check (usb_uac.h). Pure C++: tests/host/test_uac_tone.cpp runs it on the PC.
 *
 * x[n] = 2c·x[n−1] − x[n−2], c = cos(2πf/fs). Each second fits c by least squares; the next second
 * checks every sample against that fit. A lost or repeated packet is a phase jump, and a phase
 * jump leaves a residual of the order of the tone itself — far above 16-bit quantisation, so a
 * threshold of 5 % of the tone's peak tells the two apart. Breaks are counted only across two
 * seconds that were both the same clean tone: music, or a tone being retuned, counts nothing.
 *
 * It runs on every sample the computer sends, in the producer task above the audio task on
 * core 1, and that core has no double-precision unit: the sums are taken in float over blocks of
 * BLOCK samples and only those are added up in double. Float holds a block because the fit is
 * taken on the second difference e = x − 2x1 + x2 = −2(1 − c)·x1, whose sums are as small as the
 * residual being judged. Fitted on x + x2 = 2c·x1 instead, the residual is the difference of two
 * sums (1 − c)⁻² times larger — fourteen thousand for 1 kHz at 48 kHz — and float loses it.
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace usb_uac {

struct Tone {
    static constexpr uint32_t BLOCK = 64;       ///< samples summed in float before the doubles
    double   s2 = 0, p = 0, q = 0, sig = 0;    ///< Σx1², Σe·x1, Σe², Σx² over the second
    float    b2 = 0, bp = 0, bq = 0, bsig = 0; ///< the same over the block not yet added in
    uint32_t in_block = 0;
    float    x1 = 0, x2 = 0;                   ///< the two previous samples
    uint32_t seen = 0;          ///< samples since restart; the recurrence needs two
    float    c_prev = 0;        ///< last second's fit
    float    amp_prev = 0;      ///< last second's peak
    bool     prev_ok = false;   ///< last second was one clean tone
    bool     in_break = false;
    uint32_t breaks = 0;        ///< this second's

    void restart() { *this = Tone{}; }

    void push(float x) {
        if (seen >= 2) {
            bool bad = false;
            if (prev_ok) {
                const float r = x - (2.f * c_prev * x1 - x2);
                bad = std::fabs(r) > 0.05f * amp_prev;
                if (bad && !in_break) breaks++;
                in_break = bad;
            }
            // A break stays out of the fit: otherwise the samples that make one would make
            // their own second look like no clean tone, and the break would not be counted.
            if (!bad) {
                const float e = x - 2.f * x1 + x2;
                b2 += x1 * x1;
                bp += e * x1;
                bq += e * e;
                bsig += x * x;
                if (++in_block == BLOCK) fold();
            }
        } else {
            seen++;
        }
        x2 = x1;
        x1 = x;
    }

    /// Add the block's float sums into the second's.
    void fold() {
        s2 += b2;
        p += bp;
        q += bq;
        sig += bsig;
        b2 = bp = bq = bsig = 0;
        in_block = 0;
    }

    /// Close a second of `fs` samples whose peak was `peak` (full scale = 1). Returns the tone's
    /// frequency, or 0 if the second was not one clean tone; adds the second's breaks to `total`
    /// only when this second and the one before were the same clean tone.
    float close(uint32_t fs, float peak, uint32_t& total) {
        fold();
        float hz = 0;
        float c = 0;
        bool ok = false;
        if (s2 > 0 && sig > 0 && peak > 0.01f) {
            c = float(1.0 + p / (2.0 * s2));
            // Σr² at the best-fit c is q − p²/s2, against Σe² = q: what the fit had to explain
            // beyond c = 1. Against Σx² any smooth low-frequency signal would pass for one tone
            // (two tones at 1000 and 1531 Hz leave 1e-4 of Σx², but 0.14 of the second
            // difference). A 1 kHz test tone at 16 bits leaves ~1e-5; below ~300 Hz quantisation
            // alone fails.
            const double resid = q - p * p / s2;
            ok = c > -1.f && c < 1.f && q > 0 && resid / q < 0.01;
            if (ok) hz = std::acos(c) * float(fs) / (2.f * float(M_PI));
        }
        if (ok && prev_ok && std::fabs(c - c_prev) < 1e-4f) total += breaks;
        prev_ok  = ok;
        c_prev   = c;
        amp_prev = peak;
        s2 = p = q = sig = 0;
        breaks = 0;
        return hz;
    }
};

}  // namespace usb_uac
