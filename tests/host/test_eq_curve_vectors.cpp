/**
 * @file test_eq_curve_vectors.cpp
 * @brief The vectors the web's copy of the EQ maths is held to (webui/src/lib/eqmath.js).
 *
 * The web draws the DSP page's curve itself, so its design must be this one: the same
 * curve on the glass and in the browser. This test computes, with the
 * firmware's own functions, each band's response (matched, and bilinear — the fallback the JS
 * also carries) and the summed curve, for bands that reach every type and every branch, at the
 * rates the panel runs, and holds the committed fixture to them. tools/tests/test_webui_eqmath.py
 * then runs the JS against the same file.
 *
 * After a deliberate change to the design, rewrite the fixture and commit it with the change:
 *   MODESP_WRITE_EQ_VECTORS=1 tests/host/build/test_runner -tc="*web's copy*"
 */

#include "doctest.h"
#include "eq_presets.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace audio_core;

namespace {

const char* FIXTURE = MODESP_ROOT "/tests/host/fixtures/eq_curve/vectors.json";

EqBand band(BandType t, float f, float g, float q, bool on = true) {
    EqBand b; b.type = t; b.freq_hz = f; b.gain_db = g; b.q = q; b.enabled = on;
    return b;
}

struct Case { const char* name; double fs; std::vector<EqBand> bands; };

std::vector<EqBand> preset_bands(const char* id) {
    EqConfig flat;
    eq_default_config(flat);
    for (int p = 0; p < eq_preset_count(); p++) {
        if (strcmp(eq_preset(p).id, id) != 0) continue;
        EqSection s;
        eq_build_preset(p, flat.sec[0], s);
        return std::vector<EqBand>(s.bands, s.bands + s.count);
    }
    return {};
}

std::vector<Case> cases() {
    // every type, a shelf past SHELF_Q_MAX, a band that is off, one near the top of the band
    const std::vector<EqBand> every = {
        band(BandType::PEAK, 100, 6, 0.7f),
        band(BandType::LOW_SHELF, 80, 8, 0.707f),
        band(BandType::HIGH_SHELF, 10000, -6, 2.0f),
        band(BandType::LOW_PASS, 15000, 0, 0.707f),
        band(BandType::HIGH_PASS, 30, 0, 0.5f),
        band(BandType::NOTCH, 3000, 0, 4.0f),
        band(BandType::PEAK, 500, 12, 1.0f, false),
        band(BandType::PEAK, 21000, -4, 2.0f),          // past 0.49 fs at 44.1 kHz: a wire there
        band(BandType::PEAK, 1000, -12, 8.0f),
        band(BandType::LOW_SHELF, 20000, 12, 1.0f),
    };
    const std::vector<EqBand> edges = {
        band(BandType::PEAK, 20, 12, 8.0f),
        band(BandType::PEAK, 20000, -12, 0.3f),
        band(BandType::HIGH_SHELF, 20, -12, 0.3f),
        band(BandType::HIGH_SHELF, 18000, 12, 0.3f),
        band(BandType::NOTCH, 19000, 0, 0.3f),
        band(BandType::HIGH_PASS, 20000, 0, 8.0f),
        band(BandType::LOW_PASS, 20, 0, 0.3f),
    };
    EqConfig flat;
    eq_default_config(flat);
    return {
        {"rock_48k", 48000, preset_bands("rock")},
        {"loudness_44k1", 44100, preset_bands("loudness")},
        {"every_type_44k1", 44100, every},
        {"every_type_48k", 48000, every},
        {"every_type_96k", 96000, every},
        {"edges_44k1", 44100, edges},
        {"edges_192k", 192000, edges},
        {"flat_48k", 48000, std::vector<EqBand>(flat.sec[0].bands, flat.sec[0].bands + flat.sec[0].count)},
    };
}

/// 31 points, a third of a decade apart from 20 Hz to 20 kHz — none of them exactly on a notch.
std::vector<double> freqs() {
    std::vector<double> f;
    for (int j = 0; j <= 30; j++) f.push_back(20.0 * pow(10.0, j / 10.0));
    return f;
}

double one_band_db(const EqBand& b, double fs, double f, Design how) {
    SvfDesign d;
    if (!b.enabled || !design_svf_params(b, fs, d, how)) return 0.0;
    return svf_magnitude_db(d, fs, f);
}

std::string render() {
    std::string o;
    char buf[64];
    auto num = [&](double v) { snprintf(buf, sizeof(buf), "%.12g", v); o += buf; };
    auto list = [&](const std::vector<double>& v) {
        o += "[";
        for (size_t i = 0; i < v.size(); i++) { if (i) o += ","; num(v[i]); }
        o += "]";
    };
    const std::vector<double> fr = freqs();
    o += "{\n  \"_comment\": \"Written by tests/host/test_eq_curve_vectors.cpp - do not edit by hand.\",\n";
    o += "  \"freqs\": ";
    list(fr);
    o += ",\n  \"cases\": [\n";
    const auto cs = cases();
    const float fs_saved = g_fs_hz;
    for (size_t c = 0; c < cs.size(); c++) {
        const Case& k = cs[c];
        o += "    {\"name\": \"" + std::string(k.name) + "\", \"fs\": ";
        num(k.fs);
        o += ",\n     \"bands\": [";
        for (size_t i = 0; i < k.bands.size(); i++) {
            const EqBand& b = k.bands[i];
            snprintf(buf, sizeof(buf), "%s{\"type\":\"%s\",\"f\":%.9g,", i ? ", " : "", eq_type_name(b.type), (double)b.freq_hz);
            o += buf;
            snprintf(buf, sizeof(buf), "\"gain\":%.9g,\"q\":%.9g,\"on\":%s}", (double)b.gain_db, (double)b.q,
                     b.enabled ? "true" : "false");
            o += buf;
        }
        o += "],\n     \"band_db\": [";
        for (size_t i = 0; i < k.bands.size(); i++) {
            std::vector<double> v;
            for (double f : fr) v.push_back(one_band_db(k.bands[i], k.fs, f, Design::MATCHED));
            if (i) o += ", ";
            list(v);
        }
        o += "],\n     \"bilinear_db\": [";
        for (size_t i = 0; i < k.bands.size(); i++) {
            std::vector<double> v;
            for (double f : fr) v.push_back(one_band_db(k.bands[i], k.fs, f, Design::BILINEAR));
            if (i) o += ", ";
            list(v);
        }
        // the panel's own drawing: eq_curve_db at the rate the audio core runs
        g_fs_hz = (float)k.fs;
        std::vector<float> ff(fr.begin(), fr.end()), out(fr.size());
        eq_curve_db(k.bands.data(), (int)k.bands.size(), ff.data(), (int)ff.size(), out.data());
        o += "],\n     \"curve_db\": ";
        list(std::vector<double>(out.begin(), out.end()));
        o += c + 1 < cs.size() ? "},\n" : "}\n";
    }
    g_fs_hz = fs_saved;
    o += "  ]\n}\n";
    return o;
}

std::string slurp(const char* path) {
    std::string out;
    if (FILE* f = fopen(path, "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
        fclose(f);
    }
    return out;
}

bool is_num_start(char c) { return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.'; }

/// Same text, numbers within `tol` of each other: another libm may round the last digit apart.
bool same_within(const std::string& a, const std::string& b, double tol, std::string& where) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_num_start(a[i]) && is_num_start(b[j])) {
            char* ea; char* eb;
            const double x = strtod(a.c_str() + i, &ea), y = strtod(b.c_str() + j, &eb);
            if (ea != a.c_str() + i && eb != b.c_str() + j) {   // a dash in prose is no number
                // Below -48 dB a float curve is rounding noise another libm may round apart
                // (edges_*), and nothing is drawn there.
                const bool noise = x < -48.0 && y < -48.0;
                if (fabs(x - y) > tol && !noise) { where = a.substr(i > 40 ? i - 40 : 0, 80); return false; }
                i = (size_t)(ea - a.c_str());
                j = (size_t)(eb - b.c_str());
                continue;
            }
        }
        if (a[i] != b[j]) { where = a.substr(i > 40 ? i - 40 : 0, 80); return false; }
        i++; j++;
    }
    return i == a.size() && j == b.size();
}

}  // namespace

TEST_CASE("the web's copy of the EQ maths is held to these vectors (fixture is current)") {
    const std::string now = render();
    if (getenv("MODESP_WRITE_EQ_VECTORS")) {
        FILE* f = fopen(FIXTURE, "wb");
        REQUIRE(f != nullptr);
        fwrite(now.data(), 1, now.size(), f);
        fclose(f);
        MESSAGE("rewrote " << FIXTURE);
    }
    const std::string committed = slurp(FIXTURE);
    REQUIRE_MESSAGE(!committed.empty(), "no fixture: run with MODESP_WRITE_EQ_VECTORS=1");
    std::string where;
    // 1e-3 dB: the summed curve is float, as on the panel, and a notch's depth amplifies the
    // last bit of a sinf; a design change moves far more than that.
    CHECK_MESSAGE(same_within(now, committed, 1e-3, where),
                  "the design no longer matches the fixture near: " << where
                  << " - if the change is deliberate, rewrite it (see the file header)");
}
