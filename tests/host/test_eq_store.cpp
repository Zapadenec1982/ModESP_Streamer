/**
 * @file test_eq_store.cpp
 * @brief The EQ document both editors share (components/audio_dsp: eq_presets, eq_store,
 *        eq_web) — the «Власний» rules, refused stale edits, when and how the files are
 *        written (the eight sounds, the name in the file, the copies kept), and what GET
 *        and POST /api/eq carry.
 */

#include "doctest.h"
#include "eq_web.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace audio_core;

namespace {

const std::string kTmp = MODESP_TEST_TMP;
std::string path_of(const char* name) { return kTmp + "/eqstore_" + name; }
/// The owner's two files, as tools/user_data.py backed them up from the panel.
const std::string kOwner = std::string(MODESP_ROOT) + "/tests/host/fixtures/eq_owner/";

/// A file, and everything the store or safe_file keeps beside it.
void forget(const std::string& p) {
    for (const char* sfx : {"", ".v2", ".bad", ".new", ".v2.new"}) std::remove((p + sfx).c_str());
}

bool exists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

std::string slurp(const std::string& p) {
    std::string s;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return s;
    char b[1024];
    size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

void spit(const std::string& p, const std::string& bytes) {
    FILE* f = std::fopen(p.c_str(), "wb");
    REQUIRE(f);
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

int find_preset(const char* id) {
    for (int p = 0; p < eq_preset_count(); p++) if (!strcmp(eq_preset(p).id, id)) return p;
    return -1;
}

/// Every field, compared exactly: what "bit for bit" means for a document.
bool same_config(const EqConfig& a, const EqConfig& b) {
    if (strcmp(a.mode, b.mode) || a.crossover_hz != b.crossover_hz || a.sections != b.sections) return false;
    for (int s = 0; s < a.sections; s++) {
        const EqSection& x = a.sec[s];
        const EqSection& y = b.sec[s];
        if (strcmp(x.id, y.id) || strcmp(x.label, y.label) || x.gain_db != y.gain_db ||
            x.delay_ms != y.delay_ms || x.invert != y.invert || x.count != y.count) return false;
        for (int i = 0; i < x.count; i++) {
            const EqBand& p = x.bands[i];
            const EqBand& q = y.bands[i];
            if (p.type != q.type || p.freq_hz != q.freq_hz || p.gain_db != q.gain_db || p.q != q.q ||
                p.enabled != q.enabled) return false;
        }
    }
    return true;
}

/// A set of `n` peaks of the user's own making: nothing like any sound in the table.
EqConfig own_set(int n, float base_db) {
    EqConfig c;
    eq_default_config(c);
    c.sec[0].count = n;
    for (int i = 0; i < n; i++) {
        c.sec[0].bands[i].gain_db = base_db + 0.5f * (float)i;
        c.sec[0].bands[i].enabled = true;
    }
    return c;
}

/// `from` with sound `p` on, as an editor puts it.
EqConfig with_sound(const EqConfig& from, int p) {
    EqConfig c = from;
    eq_build_preset(p, from.sec[0], c.sec[0]);
    return c;
}

/// What an older build writes: the same file without "preset" — it did not know the field.
void save_as_older_build(const std::string& path, EqConfig c) {
    c.preset[0] = '\0';
    REQUIRE(eq_save(path.c_str(), c));
}

/// A store loaded from two files that are not there: the flat ten-band set.
struct Fresh {
    std::string eq = path_of("eq.json"), custom = path_of("eq_custom.json");
    EqStore st;
    Fresh() { forget(eq); forget(custom); st.load(eq.c_str(), custom.c_str()); }
    ~Fresh() { forget(eq); forget(custom); }
};

/// The two paths, empty, for a test that writes them itself and boots a store on them.
struct Pair {
    std::string eq = path_of("eq.json"), custom = path_of("eq_custom.json");
    Pair() { forget(eq); forget(custom); }
    ~Pair() { forget(eq); forget(custom); }
    /// A panel booting on them: a new store, and what it holds.
    uint8_t boot(EqStore& st, EqDoc& d) const {
        st.load(eq.c_str(), custom.c_str());
        st.get(d);
        return st.load_notes();
    }
};

}  // namespace

TEST_CASE("presets: eight sounds offered, then the kept ones; ids plain; every one recognised from its bands") {
    REQUIRE(eq_preset_count() == 23);
    REQUIRE(EQ_PRESET_SHOWN == 8);
    const char* offered[] = {"custom", "flat", "pop", "rock", "jazz", "classical", "electronic", "hiphop"};
    for (int p = 0; p < EQ_PRESET_SHOWN; p++) CHECK(std::string(eq_preset(p).id) == offered[p]);
    CHECK(std::string(eq_preset(EQ_PRESET_CUSTOM).id) == "custom");
    std::set<std::string> ids;
    for (int p = 0; p < eq_preset_count(); p++) {
        const std::string id = eq_preset(p).id;
        CHECK(ids.insert(id).second);
        for (char c : id) CHECK(((c >= 'a' && c <= 'z') || c == '_'));   // a key in the web's catalogs
    }
    EqConfig flat;
    eq_default_config(flat);
    for (int p = 1; p < eq_preset_count(); p++) {
        EqSection s;
        eq_build_preset(p, flat.sec[0], s);
        CHECK(s.count == EQ_PRESET_BANDS);
        CHECK(s.bands[0].type == BandType::LOW_SHELF);
        CHECK(s.bands[9].type == BandType::HIGH_SHELF);
        CHECK(eq_detect_preset(s, flat.sec[0]) == p);
    }
    // «Рівно»: the layout with every band off — the EQ does nothing, and Бас, Високі and the
    // loudness stay their own controls
    EqSection level;
    eq_build_preset(find_preset("flat"), flat.sec[0], level);
    for (int i = 0; i < level.count; i++) CHECK_FALSE(level.bands[i].enabled);
    CHECK(eq_detect_preset(flat.sec[0], flat.sec[0]) == EQ_PRESET_CUSTOM);
    CHECK(std::string(eq_preset(-1).id) == "custom");
    CHECK(std::string(eq_preset(99).id) == "custom");
}

TEST_CASE("presets: a kept one is offered while it plays, and an id a sound had still finds it") {
    const int rock = find_preset("rock"), loud = find_preset("loudness");
    REQUIRE(loud >= EQ_PRESET_SHOWN);
    CHECK(eq_offered_count(rock) == EQ_PRESET_SHOWN);
    CHECK(eq_offered_count(EQ_PRESET_CUSTOM) == EQ_PRESET_SHOWN);
    CHECK(eq_offered_count(loud) == EQ_PRESET_SHOWN + 1);
    CHECK(eq_offered(EQ_PRESET_SHOWN, loud) == loud);
    CHECK(eq_offered(3, loud) == rock);
    CHECK(eq_offered(EQ_PRESET_SHOWN, rock) == EQ_PRESET_CUSTOM);   // past the end: harmless

    CHECK(eq_preset_find("techno", 6) == find_preset("electronic"));   // Електроніка was Техно
    CHECK(eq_preset_find("electronic", 10) == find_preset("electronic"));
    CHECK(eq_preset_find("loudness", 8) == loud);
    CHECK(eq_preset_find("rocky", 4) == rock);                        // by length, not to a NUL
    CHECK(eq_preset_find("polka", 5) == -1);
    CHECK(eq_preset_find(nullptr, 0) == -1);
    // Електроніка is the Techno curve, unchanged: a panel left on Техно finds it by its bands
    const float techno[EQ_PRESET_BANDS] = {4.9f, 3.4f, 3.0f, 1.1f, -2.3f, -2.4f, -1.1f, 1.3f, 4.3f, 6.1f};
    for (int i = 0; i < EQ_PRESET_BANDS; i++) CHECK(eq_preset(find_preset("electronic")).g[i] == techno[i]);
}

TEST_CASE("presets: a preset is an attempt — «Власний» comes back, and a hand edit is the new «Власний»") {
    EqConfig cfg, custom;
    eq_default_config(cfg);
    int preset = EQ_PRESET_CUSTOM;
    cfg.sec[0].bands[3].gain_db = 5.0f;
    cfg.sec[0].bands[3].enabled = true;
    eq_hand_edited(cfg, custom, preset);                     // an edit is «Власний» at once
    CHECK(preset == EQ_PRESET_CUSTOM);
    CHECK(eq_same_section(custom.sec[0], cfg.sec[0]));
    const int rock = find_preset("rock");

    CHECK(eq_choose_preset(cfg, custom, preset, rock));
    CHECK(preset == rock);
    CHECK(custom.sec[0].bands[3].gain_db == 5.0f);           // kept on the way out
    CHECK_FALSE(eq_choose_preset(cfg, custom, preset, rock));   // already on

    CHECK(eq_choose_preset(cfg, custom, preset, EQ_PRESET_CUSTOM));
    CHECK(cfg.sec[0].bands[3].gain_db == 5.0f);              // and back
    CHECK_FALSE(eq_choose_preset(cfg, custom, preset, EQ_PRESET_CUSTOM));   // on, and it is the set

    eq_choose_preset(cfg, custom, preset, rock);
    cfg.sec[0].bands[0].gain_db = 1.0f;                      // a finger on the low shelf
    eq_hand_edited(cfg, custom, preset);
    CHECK(preset == EQ_PRESET_CUSTOM);
    CHECK(eq_same_section(custom.sec[0], cfg.sec[0]));

    // What plays under «Власний» is not the set (a sound this build does not have): choosing
    // «Власний» brings the set.
    EqConfig other = cfg;
    other.sec[0].bands[5].gain_db = -4.0f;
    other.sec[0].bands[5].enabled = true;
    CHECK(eq_choose_preset(other, custom, preset, EQ_PRESET_CUSTOM));
    CHECK(eq_same_section(other.sec[0], custom.sec[0]));
}

TEST_CASE("store: no files is the flat set at revision 1, and nothing to write") {
    Fresh f;
    CHECK(f.st.loaded());
    CHECK(f.st.revision() == 1);
    CHECK_FALSE(f.st.dirty());
    EqDoc d;
    CHECK(f.st.get(d) == 1);
    CHECK(d.cfg.sections == 1);
    CHECK(d.cfg.sec[0].count == MAX_BANDS);
    CHECK(d.preset == EQ_PRESET_CUSTOM);
    CHECK(f.st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK_FALSE(exists(f.eq));                              // not dirty: nothing written

    f.st.load("/nonexistent", "/nonexistent");               // once only
    CHECK(f.st.revision() == 1);
}

TEST_CASE("store: an edit from a stale revision is refused, and the current one says which it is") {
    Fresh f;
    EqDoc d;
    f.st.get(d);
    d.cfg.sec[0].bands[2].gain_db = 3.0f;
    const uint32_t r = f.st.put_bands(d.cfg.sec[0].bands, d.cfg.sec[0].count, 1);
    CHECK(r == 2);
    CHECK(f.st.put_bands(d.cfg.sec[0].bands, d.cfg.sec[0].count, 1) == 0);   // made from 1
    CHECK(f.st.choose(find_preset("pop"), 1) == 0);
    CHECK(f.st.choose(99, 2) == 0);                          // not a preset
    CHECK(f.st.choose(EQ_PRESET_CUSTOM, 2) == 2);            // already on: nothing moves
    CHECK(f.st.revision() == 2);
    CHECK(f.st.put_bands(d.cfg.sec[0].bands, MAX_BANDS + 1, 2) == 0);
}

TEST_CASE("store: the web's edit on a preset becomes «Власний», and «Власний» is written only when it moved") {
    Fresh f;
    const int pop = find_preset("pop");
    REQUIRE(f.st.choose(pop, 1) == 2);                      // leaving «Власний»: it is written
    REQUIRE(f.st.dirty());
    REQUIRE(f.st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(exists(f.eq));
    CHECK(exists(f.custom));
    CHECK_FALSE(f.st.dirty());

    // a reboot finds the preset again from the two files
    {
        EqStore again;
        again.load(f.eq.c_str(), f.custom.c_str());
        EqDoc d;
        again.get(d);
        CHECK(d.preset == pop);
    }

    // a preset to another preset: «Власний» did not move, and its file is left alone
    forget(f.custom);
    REQUIRE(f.st.choose(find_preset("rock"), 2) == 3);
    REQUIRE(f.st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK_FALSE(exists(f.custom));
    CHECK_FALSE(exists(f.eq + ".v2"));                       // this build's own file: no copy

    // a hand edit on the preset: the result is «Власний» now
    EqDoc d;
    f.st.get(d);
    d.cfg.sec[0].bands[4].gain_db = -2.0f;
    REQUIRE(f.st.put_bands(d.cfg.sec[0].bands, d.cfg.sec[0].count, 3) == 4);
    EqDoc after;
    f.st.get(after);
    CHECK(after.preset == EQ_PRESET_CUSTOM);
    CHECK(eq_same_section(after.custom.sec[0], after.cfg.sec[0]));
    REQUIRE(f.st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(exists(f.custom));

    // an edit while «Власний» plays: one file is written, eq.json — a LittleFS write is heard
    const std::string custom_before = slurp(f.custom);
    after.cfg.sec[0].bands[4].gain_db = -3.0f;
    REQUIRE(f.st.put_bands(after.cfg.sec[0].bands, after.cfg.sec[0].count, 4) == 5);
    REQUIRE(f.st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(slurp(f.custom) == custom_before);
    EqStore again;
    again.load(f.eq.c_str(), f.custom.c_str());
    EqDoc d2;
    again.get(d2);
    CHECK(d2.preset == EQ_PRESET_CUSTOM);
    CHECK(d2.custom.sec[0].bands[4].gain_db == -3.0f);       // the freshest, from eq.json
}

TEST_CASE("store: the panel's whole document goes in as it is, and the web's keep is taken once") {
    Fresh f;
    EqDoc d;
    f.st.get(d);
    d.preset = find_preset("club");
    eq_build_preset(d.preset, d.custom.sec[0], d.cfg.sec[0]);
    CHECK(f.st.put(d) == 2);
    EqDoc back;
    f.st.get(back);
    CHECK(back.preset == d.preset);
    CHECK(eq_same_section(back.cfg.sec[0], d.cfg.sec[0]));

    CHECK_FALSE(f.st.take_keep());
    f.st.keep();
    CHECK(f.st.take_keep());
    CHECK_FALSE(f.st.take_keep());
    CHECK(f.st.web_touched_at() == 0);
    f.st.web_touched(12345);
    CHECK(f.st.web_touched_at() == 12345);
}

TEST_CASE("files: the owner's eq.json and eq_custom.json - the same bands bit for bit, the same «Власний»") {
    Pair f;
    const std::string eq0 = slurp(kOwner + "eq.json"), custom0 = slurp(kOwner + "eq_custom.json");
    REQUIRE(!eq0.empty());
    REQUIRE(!custom0.empty());
    spit(f.eq, eq0);
    spit(f.custom, custom0);
    // as an older build read them: the same parser, which knew no "preset"
    EqConfig want_cfg, want_custom;
    REQUIRE(eq_parse(eq0.c_str(), eq0.size(), want_cfg));
    REQUIRE(eq_parse(custom0.c_str(), custom0.size(), want_custom));
    CHECK(want_cfg.preset[0] == '\0');

    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == 0);
    const int loud = find_preset("loudness");
    CHECK(d.preset == loud);                     // «Тонкомпенсація», kept: plays under its name
    CHECK(eq_offered_count(d.preset) == EQ_PRESET_SHOWN + 1);
    CHECK(eq_offered(EQ_PRESET_SHOWN, d.preset) == loud);
    CHECK(same_config(d.cfg, want_cfg));         // both sections, every field
    CHECK(same_config(d.custom, want_custom));
    CHECK(d.custom.sec[0].bands[0].type == BandType::HIGH_SHELF);   // their set, as they left it
    CHECK(slurp(f.eq) == eq0);                   // loading writes nothing
    CHECK_FALSE(exists(f.eq + ".v2"));

    // The first write: both files copied as they are, and eq.json says what it is - and is
    // otherwise the same file to the byte.
    REQUIRE(st.put(d) == 2);                      // the panel's document, unchanged
    REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(slurp(f.eq + ".v2") == eq0);
    CHECK(slurp(f.custom + ".v2") == custom0);
    CHECK(slurp(f.custom) == custom0);           // «Власний» did not move: not written
    const std::string eq1 = slurp(f.eq);
    const std::string line = "  \"preset\": \"loudness\",\n";
    REQUIRE(eq1.find(line) != std::string::npos);
    std::string rest = eq1;
    rest.erase(rest.find(line), line.size());
    CHECK(rest == eq0);

    EqStore again;
    EqDoc e;
    CHECK(f.boot(again, e) == 0);
    CHECK(e.preset == loud);
    CHECK(eq_same_section(e.cfg.sec[0], want_cfg.sec[0]));
    CHECK(eq_same_section(e.cfg.sec[1], want_cfg.sec[1]));
    CHECK(same_config(e.custom, want_custom));

    // the kept preset left: the menu has the eight again, and «Власний» is still theirs
    REQUIRE(again.choose(find_preset("rock"), 1) == 2);
    again.get(e);
    CHECK(eq_offered_count(e.preset) == EQ_PRESET_SHOWN);
    REQUIRE(again.choose(EQ_PRESET_CUSTOM, 2) == 3);
    again.get(e);
    CHECK(eq_same_section(e.cfg.sec[0], want_custom.sec[0]));
}

TEST_CASE("files: «Власний» of 1, 3 or 10 bands survives choosing a sound and a restart") {
    for (int n : {1, 3, 10}) {
        CAPTURE(n);
        Pair f;
        const EqConfig own = own_set(n, 1.0f);
        // an older build's pair: Рок playing, the set beside it
        save_as_older_build(f.eq, with_sound(own, find_preset("rock")));
        save_as_older_build(f.custom, own);
        {
            EqStore st;
            EqDoc d;
            CHECK(f.boot(st, d) == 0);
            CHECK(d.preset == find_preset("rock"));
            CHECK(d.custom.sec[0].count == n);     // not dropped for being unlike what plays
            CHECK(eq_same_section(d.custom.sec[0], own.sec[0]));
            REQUIRE(st.choose(find_preset("jazz"), 1) == 2);
            REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
        }
        {
            EqStore st;
            EqDoc d;
            CHECK(f.boot(st, d) == 0);
            CHECK(d.preset == find_preset("jazz"));
            CHECK(eq_same_section(d.custom.sec[0], own.sec[0]));
            REQUIRE(st.choose(EQ_PRESET_CUSTOM, 1) == 2);
            st.get(d);
            CHECK(eq_same_section(d.cfg.sec[0], own.sec[0]));
            REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
        }
        EqStore st;
        EqDoc d;
        CHECK(f.boot(st, d) == 0);
        CHECK(d.preset == EQ_PRESET_CUSTOM);
        CHECK(eq_same_section(d.cfg.sec[0], own.sec[0]));
        CHECK(eq_same_section(d.custom.sec[0], own.sec[0]));
    }
}

TEST_CASE("files: «Власний» playing - its freshest is eq.json, whatever eq_custom.json holds") {
    Pair f;
    EqConfig fresher = own_set(10, 2.0f), older = own_set(10, -1.0f);
    snprintf(fresher.preset, sizeof(fresher.preset), "custom");
    snprintf(older.preset, sizeof(older.preset), "custom");
    REQUIRE(eq_save(f.eq.c_str(), fresher));
    REQUIRE(eq_save(f.custom.c_str(), older));
    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == 0);
    CHECK(d.preset == EQ_PRESET_CUSTOM);
    CHECK(eq_same_section(d.cfg.sec[0], fresher.sec[0]));
    CHECK(eq_same_section(d.custom.sec[0], fresher.sec[0]));
    // and "custom" wins over bands that happen to be a sound's: they were left as «Власний»
    Pair g;
    EqConfig as_rock = with_sound(fresher, find_preset("rock"));
    snprintf(as_rock.preset, sizeof(as_rock.preset), "custom");
    REQUIRE(eq_save(g.eq.c_str(), as_rock));
    EqStore st2;
    CHECK(g.boot(st2, d) == 0);
    CHECK(d.preset == EQ_PRESET_CUSTOM);
}

TEST_CASE("files: the name only names - bands that are not that sound play as saved, and «Власний» stays") {
    // a later build's sound, and a curve this build does not have for a name it knows
    for (const char* name : {"lounge", "jazz"}) {
        CAPTURE(name);
        Pair f;
        EqConfig own = own_set(10, 3.0f);
        snprintf(own.preset, sizeof(own.preset), "custom");
        REQUIRE(eq_save(f.custom.c_str(), own));
        EqConfig other = own_set(10, -2.0f);
        snprintf(other.preset, sizeof(other.preset), "%s", name);
        REQUIRE(eq_save(f.eq.c_str(), other));

        EqStore st;
        EqDoc d;
        CHECK(f.boot(st, d) == 0);
        CHECK(d.preset == EQ_PRESET_CUSTOM);                   // shown as «Власний»
        CHECK(eq_same_section(d.cfg.sec[0], other.sec[0]));    // never rebuilt from the name
        CHECK(eq_same_section(d.custom.sec[0], own.sec[0]));   // and not taken for the set
        // written back with its name, so the build that has the sound still knows it
        REQUIRE(st.put(d) == 2);
        REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
        EqConfig back;
        REQUIRE(eq_load(f.eq.c_str(), back));
        CHECK(std::string(back.preset) == name);
        // another sound chosen: eq_custom.json keeps the set
        REQUIRE(st.choose(find_preset("rock"), 2) == 3);
        REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
        EqConfig mine;
        REQUIRE(eq_load(f.custom.c_str(), mine));
        CHECK(eq_same_section(mine.sec[0], own.sec[0]));
        CHECK(std::string(mine.preset) == "custom");
    }
    // and «Власний» chosen while that sound plays brings the set at once
    Pair f;
    EqConfig own = own_set(10, 3.0f);
    snprintf(own.preset, sizeof(own.preset), "custom");
    REQUIRE(eq_save(f.custom.c_str(), own));
    EqConfig other = own_set(10, -2.0f);
    snprintf(other.preset, sizeof(other.preset), "lounge");
    REQUIRE(eq_save(f.eq.c_str(), other));
    EqStore st;
    EqDoc d;
    f.boot(st, d);
    REQUIRE(st.choose(EQ_PRESET_CUSTOM, 1) == 2);
    st.get(d);
    CHECK(eq_same_section(d.cfg.sec[0], own.sec[0]));
}

TEST_CASE("files: update, a new sound, a rollback, a preset there, an update again - nothing is lost") {
    Pair f;
    // An older build: «Свій» playing, in eq.json; eq_custom.json is an older set.
    const EqConfig mine = own_set(10, 1.5f), stale = own_set(10, -3.0f);
    save_as_older_build(f.eq, mine);
    save_as_older_build(f.custom, stale);
    const std::string eq0 = slurp(f.eq), custom0 = slurp(f.custom);
    const int jazz = find_preset("jazz"), rock = find_preset("rock");
    {   // the update; Джаз
        EqStore st;
        EqDoc d;
        CHECK(f.boot(st, d) == 0);
        CHECK(d.preset == EQ_PRESET_CUSTOM);
        CHECK(eq_same_section(d.custom.sec[0], mine.sec[0]));
        REQUIRE(st.choose(jazz, 1) == 2);
        REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    }
    CHECK(slurp(f.eq + ".v2") == eq0);
    CHECK(slurp(f.custom + ".v2") == custom0);
    // The rollback. That build has no Джаз: it takes it, playing, for «Свій», and choosing Рок
    // writes it to eq_custom.json and Рок to eq.json - as that build writes them, no "preset".
    {
        EqConfig heard;
        REQUIRE(eq_load(f.eq.c_str(), heard));
        save_as_older_build(f.custom, heard);
        save_as_older_build(f.eq, with_sound(heard, rock));
    }
    {   // the update again
        EqStore st;
        EqDoc d;
        CHECK(f.boot(st, d) == EqStore::CUSTOM_FROM_COPY);
        CHECK(d.preset == rock);                          // what played
        CHECK(eq_same_section(d.custom.sec[0], mine.sec[0]));   // and «Власний» is theirs again
        REQUIRE(st.choose(find_preset("pop"), 1) == 2);
        REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    }
    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == 0);                           // written back, marked: settled
    CHECK(d.preset == find_preset("pop"));
    CHECK(eq_same_section(d.custom.sec[0], mine.sec[0]));
    CHECK(slurp(f.eq + ".v2") == eq0);                   // the copies were never written over
    CHECK(slurp(f.custom + ".v2") == custom0);
}

TEST_CASE("files: on the rollback the set was edited, not left - the edit is «Власний» after the update") {
    Pair f;
    const EqConfig mine = own_set(10, 1.5f);
    save_as_older_build(f.eq, mine);
    save_as_older_build(f.custom, mine);
    {
        EqStore st;
        EqDoc d;
        f.boot(st, d);
        REQUIRE(st.choose(find_preset("hiphop"), 1) == 2);
        REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    }
    // that build calls Хіп-хоп «Свій», and an edit there is written to eq.json alone
    EqConfig edited;
    REQUIRE(eq_load(f.eq.c_str(), edited));
    edited.sec[0].bands[4].gain_db = 2.5f;
    save_as_older_build(f.eq, edited);
    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == 0);
    CHECK(d.preset == EQ_PRESET_CUSTOM);
    CHECK(eq_same_section(d.cfg.sec[0], edited.sec[0]));        // the bands as they were left
    CHECK(eq_same_section(d.custom.sec[0], edited.sec[0]));     // what that build showed as «Свій»
}

TEST_CASE("files: a save cut off between the two writes leaves «Власний» on disk") {
    // An older build's pair: «Свій» playing, kept in eq.json only; eq_custom.json is older.
    Pair f;
    const EqConfig mine = own_set(10, 1.5f), stale = own_set(10, -3.0f);
    save_as_older_build(f.eq, mine);
    save_as_older_build(f.custom, stale);
    const std::string nowhere = kTmp + "/eqstore_no_such_dir/eq.json";   // a write that fails
    {
        EqStore st;
        EqDoc d;
        f.boot(st, d);
        REQUIRE(st.choose(find_preset("rock"), 1) == 2);
        CHECK_FALSE(st.save(nowhere.c_str(), f.custom.c_str()));   // eq_custom.json only
    }
    {
        EqStore st;
        EqDoc d;
        CHECK(f.boot(st, d) == 0);
        CHECK(d.preset == EQ_PRESET_CUSTOM);                       // what played before
        CHECK(eq_same_section(d.cfg.sec[0], mine.sec[0]));
        CHECK(eq_same_section(d.custom.sec[0], mine.sec[0]));
        EqConfig on_disk;
        REQUIRE(eq_load(f.custom.c_str(), on_disk));
        CHECK(eq_same_section(on_disk.sec[0], mine.sec[0]));        // the set is on disk now
    }
    // A hand edit on a preset, cut off the same way: the preset plays, the edit is «Власний».
    {
        EqStore st;
        EqDoc d;
        f.boot(st, d);
        REQUIRE(st.choose(find_preset("pop"), 1) == 2);
        REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    }
    EqConfig edit;
    {
        EqStore st;
        EqDoc d;
        f.boot(st, d);
        edit = d.cfg;
        edit.sec[0].bands[2].gain_db = -1.5f;
        REQUIRE(st.put_bands(edit.sec[0].bands, edit.sec[0].count, 1) == 2);
        CHECK_FALSE(st.save(nowhere.c_str(), f.custom.c_str()));
    }
    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == 0);
    CHECK(d.preset == find_preset("pop"));
    CHECK(eq_same_section(d.custom.sec[0], edit.sec[0]));
}

TEST_CASE("files: one this build cannot read is left as it is, then moved aside, never lost") {
    Pair f;
    const std::string later = "{\"version\": 3, \"outputs\": [{\"bands\": []}]}\n";   // a later schema
    const std::string broken = "{\"version\": 2, \"outputs\": [";
    spit(f.eq, later);
    spit(f.custom, broken);
    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == (EqStore::EQ_UNREADABLE | EqStore::CUSTOM_UNREADABLE));
    CHECK(d.cfg.sec[0].count == MAX_BANDS);       // the flat set
    CHECK(d.preset == EQ_PRESET_CUSTOM);
    CHECK(slurp(f.eq) == later);                  // untouched until somebody edits
    CHECK(slurp(f.custom) == broken);

    EqConfig c = d.cfg;
    c.sec[0].bands[1].gain_db = 2.0f;
    c.sec[0].bands[1].enabled = true;
    REQUIRE(st.put_bands(c.sec[0].bands, c.sec[0].count, 1) == 2);
    REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(slurp(f.eq + ".bad") == later);         // written over: moved aside first
    CHECK_FALSE(exists(f.eq + ".v2"));            // moved, not copied
    EqConfig back;
    REQUIRE(eq_load(f.eq.c_str(), back));
    CHECK(eq_same_section(back.sec[0], c.sec[0]));
    CHECK(slurp(f.custom) == broken);             // «Власний» plays, eq.json holds it: not written over

    // the next save writes over this build's own file: nothing more is moved
    c.sec[0].bands[1].gain_db = 3.0f;
    REQUIRE(st.put_bands(c.sec[0].bands, c.sec[0].count, 2) == 3);
    REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(slurp(f.eq + ".bad") == later);

    // «Власний» stops playing: eq_custom.json is written, and the file it could not read
    // goes aside first
    REQUIRE(st.choose(find_preset("rock"), 3) == 4);
    REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    CHECK(slurp(f.custom + ".bad") == broken);
    CHECK_FALSE(exists(f.custom + ".v2"));
    EqConfig mine;
    REQUIRE(eq_load(f.custom.c_str(), mine));
    CHECK(eq_same_section(mine.sec[0], c.sec[0]));
}

TEST_CASE("web: GET's bands come back through POST unchanged") {
    Fresh f;
    REQUIRE(f.st.choose(find_preset("loudness"), 1) == 2);
    EqDoc d;
    const uint32_t rev = f.st.get(d);
    char buf[2048];
    const size_t n = eq_web_json(d, rev, 48000.0f, buf, sizeof(buf));
    REQUIRE(n > 0);
    const std::string s(buf, n);
    CHECK(s.rfind("{\"rev\":2,\"fs\":48000,\"preset\":\"loudness\",", 0) == 0);
    // the eight, and the kept one that plays
    CHECK(s.find("\"presets\":[\"custom\",\"flat\",\"pop\",\"rock\",\"jazz\",\"classical\","
                 "\"electronic\",\"hiphop\",\"loudness\"],") != std::string::npos);
    CHECK(s.find("\"f\":62.5,") != std::string::npos);
    CHECK(s.find("\"q\":0.707,") != std::string::npos);
    CHECK(eq_web_json(d, rev, 48000.0f, buf, 64) == 0);      // never half a body

    // GET's band array, sent back as an edit (it ends the object, closing brace and all)
    const std::string post = "{\"rev\":2,\"bands\":" + s.substr(s.find("[{"));
    EqWebEdit e;
    REQUIRE(eq_web_parse(post.c_str(), post.size(), e));
    CHECK(e.kind == EqWebEdit::BANDS);
    CHECK(e.rev == 2);
    REQUIRE(e.count == d.cfg.sec[0].count);
    EqSection got = d.cfg.sec[0];
    for (int i = 0; i < e.count; i++) got.bands[i] = e.bands[i];
    CHECK(eq_same_section(got, d.cfg.sec[0]));
}

TEST_CASE("web: a preset by name, keep, and what is refused") {
    EqWebEdit e;
    const char* ok = R"({"rev":7,"preset":"electronic"})";
    REQUIRE(eq_web_parse(ok, strlen(ok), e));
    CHECK(e.kind == EqWebEdit::PRESET);
    CHECK(e.rev == 7);
    CHECK(e.preset == find_preset("electronic"));
    const char* old_id = R"({"rev":7,"preset":"techno"})";    // a page still open from before
    REQUIRE(eq_web_parse(old_id, strlen(old_id), e));
    CHECK(e.preset == find_preset("electronic"));
    const char* kept = R"({"rev":7,"preset":"soft_rock"})";   // kept, not offered: still a preset
    REQUIRE(eq_web_parse(kept, strlen(kept), e));
    CHECK(e.preset == find_preset("soft_rock"));

    const char* keep = R"({"keep":true})";
    REQUIRE(eq_web_parse(keep, strlen(keep), e));
    CHECK(e.kind == EqWebEdit::KEEP);
    CHECK(e.keep);

    // the page's last edit, sent as it is left: an edit, and kept at once
    const char* last = R"({"rev":9,"bands":[{"type":"peak","f":100,"gain":1,"q":1,"on":true}],"keep":true})";
    REQUIRE(eq_web_parse(last, strlen(last), e));
    CHECK(e.kind == EqWebEdit::BANDS);
    CHECK(e.keep);
    REQUIRE(eq_web_parse(ok, strlen(ok), e));
    CHECK_FALSE(e.keep);

    const char* bad[] = {
        R"({"preset":"rock"})",                                   // no revision
        R"({"rev":0,"preset":"rock"})",                           // 0 is never a revision
        R"({"rev":3,"preset":"polka"})",                          // no such preset
        R"({"rev":3})",                                           // no edit
        R"({"rev":3,"preset":"rock","bands":[{"f":100}]})",       // two edits
        R"({"rev":3,"bands":[]})",                                // no bands
        R"({"rev":3,"bands":[{"type":"bell"}]})",                 // no such type
        R"({"rev":3,"bands":[{"f":"loud"}]})",                    // not a number
        R"({"rev":3,"bands":[1,2,3,4,5,6,7,8,9,10,11]})",         // too many
        R"({"rev":3,"bands":[{}],"bands":[{}]})",                 // a repeated key
        R"([1,2])",
        R"({"rev":3,"bands":[{"f":100})",                          // cut off
        R"({"keep":false})",
    };
    for (const char* b : bad) {
        INFO(b);
        CHECK_FALSE(eq_web_parse(b, strlen(b), e));
    }

    // A second "bands" one band past the end. The host has no ASan: a canary after the edit,
    // a NaN in every slot that eq_web_clamp() would rewrite, shows nothing was written there.
    struct { EqWebEdit e; uint32_t canary[16]; } g;
    for (uint32_t& c : g.canary) c = 0xFFFFFFFFu;
    const char* past = R"({"rev":3,"bands":[{},{},{},{},{},{},{},{},{},{}],"bands":[{}]})";
    CHECK_FALSE(eq_web_parse(past, strlen(past), g.e));
    for (uint32_t c : g.canary) CHECK(c == 0xFFFFFFFFu);
}

TEST_CASE("web: numbers out of reach of the panel's page are held to it") {
    const char* body = R"({"rev":4,"bands":[
        {"type":"peak","f":5,"gain":30,"q":40,"on":true},
        {"type":"lowshelf","f":90000,"gain":-30,"q":4,"on":false},
        {"type":"notch","f":1000,"gain":0,"q":0.01,"on":true},
        {"type":"peak","f":nan,"gain":inf,"q":1,"on":true}]})";
    EqWebEdit e;
    REQUIRE(eq_web_parse(body, strlen(body), e));
    REQUIRE(e.count == 4);
    CHECK(e.bands[0].freq_hz == EQ_WEB_F_MIN);
    CHECK(e.bands[0].gain_db == EQ_WEB_GAIN_MAX);
    CHECK(e.bands[0].q == EQ_WEB_Q_MAX);
    CHECK(e.bands[0].enabled);
    CHECK(e.bands[1].freq_hz == EQ_WEB_F_MAX);
    CHECK(e.bands[1].gain_db == -EQ_WEB_GAIN_MAX);
    CHECK(e.bands[1].q == SHELF_Q_MAX);                       // a shelf stops where it would overshoot
    CHECK_FALSE(e.bands[1].enabled);
    CHECK(e.bands[2].q == EQ_WEB_Q_MIN);
    CHECK(e.bands[3].freq_hz == 1000.0f);                     // NaN: back to a harmless value
    CHECK(e.bands[3].gain_db == EQ_WEB_GAIN_MAX);
}

TEST_CASE("web: an imported set of three bands is «Власний», and a restart and a sound chosen meanwhile keep it") {
    // «Імпорт фільтрів» posts a file's bands as they are — three here, not ten — and the store keeps
    // «Власний» of any count
    Pair f;
    EqStore st;
    EqDoc d;
    CHECK(f.boot(st, d) == 0);                               // no files: the flat ten, revision 1
    REQUIRE(st.choose(find_preset("rock"), 1) == 2);          // a genre plays when the file comes in
    const char* body = R"({"rev":2,"bands":[
        {"type":"lowshelf","f":105,"gain":5.5,"q":0.71,"on":true},
        {"type":"peak","f":2519,"gain":3.9,"q":2.45,"on":true},
        {"type":"highshelf","f":10000,"gain":-2,"q":0.71,"on":true}],"keep":true})";
    EqWebEdit e;
    REQUIRE(eq_web_parse(body, strlen(body), e));
    REQUIRE(e.count == 3);
    REQUIRE(st.put_bands(e.bands, e.count, e.rev) == 3);
    st.get(d);
    CHECK(d.preset == EQ_PRESET_CUSTOM);
    CHECK(d.cfg.sec[0].count == 3);
    const EqSection imported = d.cfg.sec[0];
    REQUIRE(st.save(f.eq.c_str(), f.custom.c_str()));
    {
        EqStore again;
        EqDoc a;
        CHECK(f.boot(again, a) == 0);
        CHECK(a.preset == EQ_PRESET_CUSTOM);
        CHECK(eq_same_section(a.cfg.sec[0], imported));        // restarted: the three play
        REQUIRE(again.choose(find_preset("jazz"), 1) == 2);     // a sound tried meanwhile
        REQUIRE(again.save(f.eq.c_str(), f.custom.c_str()));
    }
    EqStore last;
    EqDoc b;
    CHECK(f.boot(last, b) == 0);
    CHECK(b.preset == find_preset("jazz"));
    CHECK(eq_same_section(b.custom.sec[0], imported));         // «Власний» kept, not dropped for being three
    REQUIRE(last.choose(EQ_PRESET_CUSTOM, 1) == 2);
    last.get(b);
    CHECK(b.cfg.sec[0].count == 3);
    CHECK(eq_same_section(b.cfg.sec[0], imported));
}
