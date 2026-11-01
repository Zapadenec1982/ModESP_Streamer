/**
 * @file test_port_finder.cpp
 * @brief HOST tests for the port finder (components/audio_dsp/port_finder.h): ToneMeter reads one
 *        sine through hum, and find_port() finds fb in near-field sweeps of modelled vented boxes —
 *        run end to end, as the panel runs it: a tone at each grid frequency, the microphone signal
 *        built from the box's response plus 50 Hz hum and noise, 0.4 s Hann windows, a baseline with
 *        nothing playing — and finds nothing in a closed box or in silence.
 */

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>
#include "doctest.h"
#include "port_finder.h"

using namespace audio_core;
using cd = std::complex<double>;

namespace {

/// A vented box (R. H. Small, "Vented-Box Loudspeaker Systems", JAES 1973): Vas/Vb, fb/fs, the
/// driver's total Q and the box's losses.
struct Box { double alpha, h, qt, ql; };
const Box B4    {1.41, 1.00, 0.383, 7.0};
const Box QB3   {2.50, 1.30, 0.300, 7.0};
const Box BOOMY {0.90, 0.90, 0.500, 7.0};

/// What a microphone at the cone hears: the cone's acceleration, normalised to the passband, plus
/// `leak` of the port's own output (a rear port 5 % of the way, a front one more).
cd nearfield(double f, double fb, const Box& b, double leak) {
    const double fs = fb / b.h, Ts = 1.0 / (2 * M_PI * fs), Tb = 1.0 / (2 * M_PI * fb);
    const cd s(0.0, 2 * M_PI * f);
    const cd D = s * s * s * s * Ts * Ts * Tb * Tb + s * s * s * (Ts * Ts * Tb / b.ql + Ts * Tb * Tb / b.qt)
               + s * s * ((b.alpha + 1) * Ts * Ts + Ts * Tb / (b.ql * b.qt) + Tb * Tb) + s * (Ts / b.ql + Tb / b.qt) + 1.0;
    const cd Xc = (s * s * Tb * Tb + s * Tb / b.ql + 1.0) / D;
    const cd G = s * s * s * s * Ts * Ts * Tb * Tb / D;
    const cd Pc = s * s * Ts * Ts * Xc;
    return Pc + leak * (G - Pc);
}

/// A closed box's near field: a 2nd-order high-pass at fc.
cd closed(double f, double fc) {
    const cd s(0.0, f / fc);
    return s * s / (s * s + s / 0.707 + 1.0);
}

/// The microphone's own low end: a 2nd-order high-pass, or nothing.
cd mic_hp(double f, double fc) {
    if (fc <= 0) return 1.0;
    const cd s(0.0, f / fc);
    return s * s / (s * s + s / 0.707 + 1.0);
}

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }
double gauss(uint32_t& s) {                              // Box–Muller
    const double u1 = ((lcg(s) >> 8) + 1.0) / 16777217.0, u2 = (lcg(s) >> 8) / 16777216.0;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2 * M_PI * u2);
}

/// The panel's measurement, simulated: tones of the given complex responses at the grid's
/// frequencies, then find_port().
struct Sweep {
    double fs = 48000.0;
    uint32_t n = 19200;            // 0.4 s
    double hum = 0.0, noise = 0.0; // amplitude of 50 Hz pickup, rms of white noise
    uint32_t seed = 1;
    double t = 0.0;                // running time: hum keeps its own phase across windows

    float window(double hz, cd tone) {
        ToneMeter m;
        m.start((float)hz, (float)fs, n);
        std::vector<float> x(n);
        const double a = std::abs(tone), ph = std::arg(tone);
        for (uint32_t i = 0; i < n; i++) {
            const double ti = t + i / fs;
            x[i] = (float)(a * std::sin(2 * M_PI * hz * ti + ph) + hum * std::sin(2 * M_PI * 50.0 * ti + 0.3) + noise * gauss(seed));
        }
        m.push(x.data(), x.size());
        t += (n + 12000) / fs;    // and 0.25 s of settling before the next
        return m.amplitude();
    }

    PortFit run(const std::vector<cd>& resp, const std::vector<int>& grid) {
        double peak = 0.0;
        for (const cd& r : resp) peak = std::max(peak, std::abs(r));
        std::vector<float> hz(grid.size()), level(grid.size()), floor(grid.size());
        for (size_t k = 0; k < grid.size(); k++) {
            hz[k] = (float)grid[k];
            double p = 0.0;                                   // the baseline: six silent windows
            for (int w = 0; w < 6; w++) { const double v = window(grid[k], 0.0); p += v * v; }
            floor[k] = (float)std::sqrt(p / 6.0);
        }
        for (size_t k = 0; k < grid.size(); k++)              // the auto-level puts the loudest at −20 dBFS
            level[k] = window(grid[k], peak > 0 ? resp[k] * (0.1 / peak) : cd(0.0));
        return find_port(hz.data(), level.data(), floor.data(), (int)grid.size());
    }
};

std::vector<int> grid() {
    int g[96];
    const int n = port_sweep_grid(20, 100, g, 96);
    return std::vector<int>(g, g + n);
}

}  // namespace

TEST_CASE("port finder: the grid is whole hertz, 1/24 octave, without 50 and 60") {
    const auto g = grid();
    REQUIRE(g.size() >= 40);
    CHECK(g.front() == 20);
    CHECK(g.back() <= 100);
    for (size_t k = 1; k < g.size(); k++) {
        CHECK(g[k] > g[k - 1]);
        CHECK(g[k] - g[k - 1] <= std::max(2, (int)std::lround(0.06 * g[k])));
    }
    CHECK(std::find(g.begin(), g.end(), 50) == g.end());
    CHECK(std::find(g.begin(), g.end(), 60) == g.end());
    MESSAGE(g.size() << " points from " << g.front() << " to " << g.back() << " Hz");
}

TEST_CASE("port finder: ToneMeter reads one sine, and keeps hum 10 Hz away out") {
    const double fs = 48000.0;
    const uint32_t n = 19200;
    std::vector<float> x(n);
    for (double hz : {23.0, 47.0, 91.0}) {
        for (uint32_t i = 0; i < n; i++) x[i] = (float)(0.3 * std::sin(2 * M_PI * hz * i / fs + 1.0));
        ToneMeter m;
        m.start((float)hz, (float)fs, n);
        CHECK(m.push(x.data(), x.size()) == n);
        CHECK(m.done());
        CAPTURE(hz);
        CHECK(m.amplitude() == doctest::Approx(0.3).epsilon(0.002));
    }
    // A 0.3 hum at 50 Hz, read at 40 Hz: what leaks through the window's sidelobes.
    for (uint32_t i = 0; i < n; i++) x[i] = (float)(0.3 * std::sin(2 * M_PI * 50.0 * i / fs));
    ToneMeter m;
    m.start(40.0f, (float)fs, n);
    m.push(x.data(), x.size());
    const double leak_db = 20.0 * std::log10(m.amplitude() / 0.3 + 1e-30);
    MESSAGE("hum 10 Hz away: " << leak_db << " dB");
    CHECK(leak_db <= -40.0);
}

TEST_CASE("port finder: fb of modelled vented boxes, the microphone at the cone") {
    const auto g = grid();
    double worst = 0.0;
    struct A { const char* name; Box b; };
    for (const A& al : {A{"B4", B4}, A{"QB3", QB3}, A{"boomy", BOOMY}}) {
        for (double fb : {35.0, 45.0, 55.0, 70.0}) {
            for (double hp : {0.0, 40.0}) {
                std::vector<cd> r;
                for (int f : g) r.push_back(nearfield(f, fb, al.b, 0.0) * mic_hp(f, hp));
                Sweep s;
                s.noise = 1e-4;                       // −80 dB rms re full scale
                s.hum = 3e-4;
                s.seed = (uint32_t)(fb * 10 + hp);
                const PortFit p = s.run(r, g);
                const std::string name = al.name; CAPTURE(name); CAPTURE(fb); CAPTURE(hp); CAPTURE(p.hz); CAPTURE(p.depth_db); CAPTURE(p.below_db); CAPTURE(p.snr_db);
                // A microphone that rolls off below the dip hides the cone's rise there, and
                // OK wants 6 dB of it: such a dip is found where it is but not applied unasked.
                if (hp == 0.0) CHECK(p.status == PortFit::OK);
                else CHECK((p.status == PortFit::OK || p.status == PortFit::WEAK));
                CHECK(std::fabs(p.hz - fb) <= 1.0);
                worst = std::max(worst, std::fabs(p.hz - fb));
            }
        }
    }
    MESSAGE("no port leakage: at worst " << worst << " Hz off fb");
}

TEST_CASE("port finder: a rear port's sound pulls the dip a little low") {
    const auto g = grid();
    double lo = 0.0;
    for (double fb : {40.0, 50.0, 62.0}) {
        std::vector<cd> r;
        for (int f : g) r.push_back(nearfield(f, fb, B4, 0.05));
        Sweep s;
        s.noise = 1e-4;
        const PortFit p = s.run(r, g);
        CAPTURE(fb); CAPTURE(p.hz);
        CHECK(p.status == PortFit::OK);
        CHECK(p.hz <= fb + 1.0);
        CHECK(p.hz >= fb * 0.95);
        lo = std::min(lo, (p.hz - fb) / fb);
    }
    MESSAGE("port at 5 % of the cone's level: down to " << 100.0 * lo << " %");
}

TEST_CASE("port finder: hum right next to the dip does not move it") {
    const auto g = grid();
    for (double fb : {48.0, 52.0}) {
        for (uint32_t seed : {3u, 4u, 5u}) {
            std::vector<cd> r;
            for (int f : g) r.push_back(nearfield(f, fb, QB3, 0.0));
            Sweep s;
            s.noise = 1e-4;
            s.hum = 1e-3;                             // −40 dB re the loudest tone, at the dip's door
            s.seed = seed;
            s.t = 0.137 * seed;
            const PortFit p = s.run(r, g);
            CAPTURE(fb); CAPTURE(seed); CAPTURE(p.hz); CAPTURE(p.snr_db);
            CHECK(p.status == PortFit::OK);
            CHECK(std::fabs(p.hz - fb) <= 1.5);
        }
    }
}

TEST_CASE("port finder: a closed box has no dip, and silence is not heard") {
    const auto g = grid();
    for (double fc : {50.0, 70.0, 90.0}) {
        for (double hp : {0.0, 40.0}) {
            std::vector<cd> r;
            for (int f : g) r.push_back(closed(f, fc) * mic_hp(f, hp));
            Sweep s;
            s.noise = 1e-4;
            s.hum = 3e-4;
            const PortFit p = s.run(r, g);
            CAPTURE(fc); CAPTURE(hp); CAPTURE(p.hz);
            CHECK(p.status == PortFit::NO_DIP);
        }
    }
    std::vector<cd> none(g.size(), cd(0.0));
    Sweep s;
    s.noise = 1e-3;
    s.hum = 1e-3;
    CHECK(s.run(none, g).status == PortFit::NOT_HEARD);
}

TEST_CASE("port finder: the SC-G101's sweep is not applied unasked") {
    // The panel's own sweep, microphone at the woofer: dBFS at the microphone, tone and noise,
    // at -42.5 dBFS of drive (measure1.log). It found 67.7 Hz with 17 dB above and 4 dB below and
    // wrote it into the port setting; a finger on the dust cap put the still cone at 63 Hz, where
    // the sweep shows a bump instead: a room mode there, fed by the port at its own frequency.
    static const float F[]  = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
                               42, 44, 45, 46, 48, 49, 52, 53, 55, 57, 58, 62, 63, 65, 67, 69, 71, 73, 76, 78, 80, 82,
                               85, 87, 90, 92, 95, 98};
    static const float TONE[] = {-59.3f, -57.3f, -55.9f, -53.5f, -51.9f, -50.7f, -49.5f, -48.8f, -47.5f, -46.2f, -44.9f,
                                 -43.9f, -42.9f, -42.0f, -41.1f, -40.4f, -39.4f, -38.9f, -38.1f, -37.4f, -36.8f, -36.3f,
                                 -35.9f, -35.2f, -34.8f, -34.3f, -33.8f, -33.6f, -33.9f, -34.2f, -34.9f, -35.1f, -34.9f,
                                 -29.0f, -30.0f, -33.2f, -35.3f, -34.9f, -33.4f, -31.5f, -28.6f, -26.7f, -25.1f, -23.7f,
                                 -21.7f, -20.6f, -19.8f, -19.5f, -18.5f, -17.4f};
    static const float NOISE[] = {-89.5f, -89.4f, -88.5f, -85.6f, -81.8f, -79.0f, -77.6f, -77.6f, -78.8f, -80.9f, -83.6f,
                                  -86.1f, -87.1f, -85.4f, -83.3f, -82.1f, -82.4f, -83.9f, -85.7f, -86.3f, -86.5f, -87.1f,
                                  -88.2f, -89.8f, -90.2f, -90.9f, -94.4f, -93.3f, -86.1f, -86.0f, -89.9f, -92.3f, -90.3f,
                                  -84.9f, -84.8f, -86.3f, -92.2f, -96.8f, -99.0f, -98.8f, -100.3f, -98.0f, -97.7f, -99.5f,
                                  -102.2f, -102.3f, -100.1f, -99.4f, -102.1f, -104.3f};
    constexpr int N = (int)(sizeof(F) / sizeof(F[0]));
    static_assert(sizeof(TONE) / sizeof(TONE[0]) == N && sizeof(NOISE) / sizeof(NOISE[0]) == N, "one row each");
    float level[N], noise[N];
    for (int k = 0; k < N; k++) {
        level[k] = std::pow(10.0f, TONE[k] / 20.0f);
        noise[k] = std::pow(10.0f, NOISE[k] / 20.0f);
    }
    const PortFit p = find_port(F, level, noise, N);
    MESSAGE("SC-G101 sweep: status " << (int)p.status << ", " << p.hz << " Hz, " << p.depth_db << " dB above / "
            << p.below_db << " dB below; the cone stood still at 63 Hz");
    CHECK(p.status == PortFit::WEAK);
    CHECK(p.hz > 66.0f);
    CHECK(p.hz < 69.5f);
}
