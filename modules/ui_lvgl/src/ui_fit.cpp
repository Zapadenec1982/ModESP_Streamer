/**
 * @file ui_fit.cpp
 * @brief GET /api/i18n/overflow — the text on the glass that does not fit its place.
 *
 * Level 2 of the length check. German and Polish run
 * longer than the Ukrainian the screens were laid out in; tools/i18n/fit.py measures every
 * string against its Ukrainian, and this looks at the real thing: the LVGL tree as built, in the
 * language that is on, every page at once — they are all built and only shown in turn, so
 * nothing on the glass has to change for the check but the language itself.
 *
 * A label is reported when its text
 *   cut    is wider than a one-line label and is cut with "…" or clipped;
 *   lines  wraps into more lines than a fixed-height label has room for, cut or not;
 *   word   has a word wider than the label, which LVGL then breaks inside the word;
 *   outside  runs past its parent, which clips it;
 *   overlap  sits over a control or another text beside it (an auto-width title running into
 *          a slider, the clock).
 * Text set by a page's refresh shows only for the page that is open, and only in the state it
 * is in; the strings that are not on the glass at the time are fit.py's. Icons are not text
 * (their glyphs sit in the Private Use Area and are never translated), and a label that
 * scrolls its text is meant to be longer than it is.
 *
 * `?all=1` lists every label that fits as well, kind "ok": what tools/i18n/fit.py --accept is
 * told was seen on the glass.
 *
 * Read-only, behind the web UI's login like /api/screenshot (open on the bench build the same
 * way). The walk holds the LVGL lock for its length: a few milliseconds over a thousand objects.
 */

#include "ui_fit.h"
#include "ui_common.h"
#include "i18n.h"
#include "modesp/json_escape.h"
#include "modesp/utf8.h"
#include "modesp/net/http_routes.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "src/widgets/label/lv_label_private.h"   // a DOTS label's own text, before the "..."
#include <cstdarg>
#include <cstdio>
#include <cstring>

static const char* TAG = "UiFit";

namespace {

constexpr size_t   OUT_CAP    = 16 * 1024;   // in PSRAM, for the length of one call
constexpr size_t   OUT_CAP_ALL = 64 * 1024;  // ... with every label listed (?all=1)
constexpr int      MAX_ISSUES = 60;
constexpr size_t   TEXT_MAX   = 96;          // bytes of a label's text quoted in the report
constexpr size_t   GIVEN_MAX  = 256;         // bytes of a cut label's text put back together
constexpr uint32_t DOT_BEGIN_INV = 0xFFFFFFFF;   // lv_label.c's LV_LABEL_DOT_BEGIN_INV: no dots
constexpr const char* LANG_CODES[] = {"uk", "en", "de", "pl"};   // as the web names them
static_assert(sizeof(LANG_CODES) / sizeof(LANG_CODES[0]) == (size_t)i18n::Lang::COUNT, "a code per language");

struct Walk {
    char*  out;
    size_t cap;
    bool   all = false;    ///< list the labels that fit too
    size_t n = 0;
    bool   full = false;
    int    labels = 0;
    int    issues = 0;
    int    listed = 0;     ///< "ok" entries, with all
    bool   more = false;
};

void put(Walk& w, const char* f, ...) {
    if (w.full) return;
    va_list ap;
    va_start(ap, f);
    const int k = vsnprintf(w.out + w.n, w.cap - w.n, f, ap);
    va_end(ap);
    if (k < 0 || w.n + (size_t)k >= w.cap) { w.full = true; return; }
    w.n += (size_t)k;
}

/// `txt` cut to TEXT_MAX on a character boundary and escaped for JSON, into `esc`.
void quote(char (&esc)[TEXT_MAX * 6 + 1], const char* txt) {
    char cut[TEXT_MAX + 1];
    modesp::utf8_copy(cut, sizeof(cut), txt, strlen(txt));
    modesp::json_escape(esc, sizeof(esc), cut);
}

void report(Walk& w, const lv_obj_t* o, const char* kind, const char* txt, int32_t need, int32_t have) {
    if (w.issues >= MAX_ISSUES) { w.more = true; return; }
    char esc[TEXT_MAX * 6 + 1];
    quote(esc, txt);
    const lv_font_t* f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    put(w, "%s{\"page\":\"%s\",\"kind\":\"%s\",\"text\":\"%s\",\"line_h\":%ld,\"need\":%ld,\"have\":%ld,"
           "\"x\":%ld,\"y\":%ld}",
        w.issues + w.listed ? "," : "", ui_shell_page_of(o), kind, esc, (long)(f ? lv_font_get_line_height(f) : 0),
        (long)need, (long)have, (long)a.x1, (long)a.y1);
    w.issues++;
}

/// A label that fits, for ?all=1: its page and its text, nothing to measure.
void list_ok(Walk& w, const lv_obj_t* o) {
    char esc[TEXT_MAX * 6 + 1];
    quote(esc, lv_label_get_text(o));
    put(w, "%s{\"page\":\"%s\",\"kind\":\"ok\",\"text\":\"%s\"}",
        w.issues + w.listed ? "," : "", ui_shell_page_of(o), esc);
    w.listed++;
}

/// The widest word of `txt`: a word wider than its label is broken inside it.
int32_t widest_word(const char* txt, const lv_font_t* f, int32_t ls) {
    int32_t best = 0;
    const char* p = txt;
    while (*p) {
        while (*p == ' ' || *p == '\n') p++;
        const char* e = p;
        while (*e && *e != ' ' && *e != '\n') e++;
        if (e > p) {
            const int32_t wd = lv_text_get_width(p, (uint32_t)(e - p), f, ls);
            if (wd > best) best = wd;
        }
        p = e;
    }
    return best;
}

bool overlap(const lv_area_t& a, const lv_area_t& b) {
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

/// An icon: the faces' symbols are in the Private Use Area, U+E000..U+F8FF (EE 80 80..EF A3 BF).
bool icon(const char* txt) {
    const auto* u = reinterpret_cast<const unsigned char*>(txt);
    return u[0] == 0xEE || (u[0] == 0xEF && u[1] >= 0x80 && u[1] <= 0xA3);
}

/// The text the label was given. LVGL writes "..." over the end of a DOTS label's text that
/// does not fit and keeps the bytes it wrote over (lv_label_set_dots); measuring what is left
/// would always fit. Put back together in `buf`; the label itself is not touched.
const char* given_text(lv_obj_t* o, char* buf, size_t cap) {
    const auto* l = reinterpret_cast<const lv_label_t*>(o);
    if (l->dot_begin == DOT_BEGIN_INV) return l->text;
    size_t n = 0;
    auto add = [&](char ch) { if (n + 1 < cap) buf[n++] = ch; };
    for (uint32_t i = 0; i < l->dot_begin && l->text[i]; i++) add(l->text[i]);
    uint32_t k = 0;
    for (; k < sizeof(l->dot) && l->dot[k]; k++) add(l->dot[k]);
    if (k == sizeof(l->dot)) {                     // the text went on past the bytes kept
        for (const char* p = l->text + l->dot_begin + k; *p; p++) add(*p);
    }
    buf[n] = '\0';
    return buf;
}

/// A sibling worth not running into: a control, or another text. A label with no text yet is
/// neither (the Bluetooth page's note, before its first refresh).
bool solid(const lv_obj_t* s) {
    if (lv_obj_check_type(s, &lv_label_class)) {
        const char* t = lv_label_get_text(s);
        return t && *t;
    }
    if (!lv_obj_has_flag(s, LV_OBJ_FLAG_CLICKABLE)) return false;
    // An invisible touch target — nothing drawn and nothing in it, as the Player's seek box
    // around its 4-px bar — lies under the text on purpose: a finger finds it, not an eye. It
    // was the "0:00" overlap the walk reported in en, de and pl.
    return lv_obj_get_style_bg_opa(s, LV_PART_MAIN) > LV_OPA_TRANSP ||
           lv_obj_get_style_border_width(s, LV_PART_MAIN) > 0 ||
           lv_obj_get_child_count(s) > 0;
}

/// True when the label is text, was measured and fits.
bool check_label(Walk& w, lv_obj_t* o) {
    const char* txt = lv_label_get_text(o);
    if (!txt || !*txt || icon(txt)) return false;
    w.labels++;
    const lv_font_t* f = lv_obj_get_style_text_font(o, LV_PART_MAIN);
    if (!f) return false;
    const int32_t ls = lv_obj_get_style_text_letter_space(o, LV_PART_MAIN);
    const int32_t sp = lv_obj_get_style_text_line_space(o, LV_PART_MAIN);
    const bool auto_w = lv_obj_get_style_width(o, LV_PART_MAIN) == LV_SIZE_CONTENT;
    const bool auto_h = lv_obj_get_style_height(o, LV_PART_MAIN) == LV_SIZE_CONTENT;
    const int32_t cw = lv_obj_get_content_width(o);
    const int32_t ch = lv_obj_get_content_height(o);

    const int32_t line_h = lv_font_get_line_height(f);
    if (!auto_w) {
        const lv_label_long_mode_t mode = lv_label_get_long_mode(o);
        if (mode == LV_LABEL_LONG_MODE_WRAP) {
            lv_point_t sz;
            lv_text_get_size(&sz, txt, f, ls, sp, cw, LV_TEXT_FLAG_NONE);
            if (!auto_h && sz.y > ch) { report(w, o, "lines", txt, sz.y, ch); return false; }
            const int32_t word = widest_word(txt, f, ls);
            if (word > cw) { report(w, o, "word", txt, word, cw); return false; }
        } else if (!auto_h && (mode == LV_LABEL_LONG_MODE_DOTS || mode == LV_LABEL_LONG_MODE_CLIP)) {
            // Both wrap at the width and stop at the height: one line high, the text has to
            // fit across; two, it may wrap once (the Source page's cards).
            char given[GIVEN_MAX];
            const char* full = given_text(o, given, sizeof(given));
            lv_point_t sz;
            if (ch < 2 * line_h) {
                lv_text_get_size(&sz, full, f, ls, sp, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
                if (sz.x > cw) { report(w, o, "cut", full, sz.x, cw); return false; }
            } else {
                lv_text_get_size(&sz, full, f, ls, sp, cw, LV_TEXT_FLAG_NONE);
                if (sz.y > ch) { report(w, o, "lines", full, sz.y, ch); return false; }
            }
        }
    }

    lv_obj_t* parent = lv_obj_get_parent(o);
    if (!parent) return true;
    lv_area_t a, pa;
    lv_obj_get_coords(o, &a);
    // Past the parent's edge, where the parent clips it (a scrolling parent shows it by scrolling).
    // Downwards only once the text has wrapped: one line's height is the face's, glyphs and
    // the space over and under them, and a big digit's line runs past its card with nothing
    // cut (the Service page's "0").
    if (!lv_obj_has_flag(parent, LV_OBJ_FLAG_OVERFLOW_VISIBLE) && !lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) {
        lv_obj_get_content_coords(parent, &pa);
        if (a.x2 > pa.x2 || (a.y2 > pa.y2 && lv_area_get_height(&a) > line_h)) {
            report(w, o, "outside", txt, lv_area_get_width(&a), pa.x2 - a.x1 + 1);
            return false;
        }
    }
    // Over a control or another text beside it. Only an auto-width label grows into its
    // neighbours; a fixed one is cut instead, which is "cut" above. A hidden one is the other
    // half of a pair that takes turns (the Bluetooth page's settings and the note in their
    // place), and its neighbours are the ones shown instead of it.
    if (!auto_w || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    const uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t* s = lv_obj_get_child(parent, (int32_t)i);
        if (s == o || lv_obj_has_flag(s, LV_OBJ_FLAG_HIDDEN) || !solid(s)) continue;
        lv_area_t b;
        lv_obj_get_coords(s, &b);
        if (overlap(a, b)) {
            report(w, o, "overlap", txt, lv_area_get_width(&a), b.x1 > a.x1 ? b.x1 - a.x1 : 0);
            return false;
        }
    }
    return true;
}

/// Every label under `o`, hidden pages and dialogs included: they are built, laid out and
/// waiting to be shown.
void walk(Walk& w, lv_obj_t* o) {
    if (lv_obj_check_type(o, &lv_label_class) && check_label(w, o) && w.all) list_ok(w, o);
    const uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) walk(w, lv_obj_get_child(o, (int32_t)i));
}

esp_err_t handle_overflow(httpd_req_t* req) {
#if !CONFIG_UI_SCREENSHOT_OPEN
    if (!modesp::http_routes::authorized(req)) return ESP_OK;
#endif
    char query[16], val[4];
    const bool all = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                     httpd_query_key_value(query, "all", val, sizeof(val)) == ESP_OK && val[0] == '1';
    const size_t cap = all ? OUT_CAP_ALL : OUT_CAP;
    char* out = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    if (!lvgl_port_lock(500)) {
        heap_caps_free(out);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "{\"error\":\"busy\"}", HTTPD_RESP_USE_STRLEN);
    }
    Walk w{out, cap};
    w.all = all;
    const int l = (int)i18n::lang();
    put(w, "{\"lang\":\"%s\",\"issues\":[", LANG_CODES[l]);
    lv_obj_update_layout(lv_screen_active());
    walk(w, lv_screen_active());
    lvgl_port_unlock();
    put(w, "],\"labels\":%d,\"more\":%s}", w.labels, w.more ? "true" : "false");
    esp_err_t err;
    if (w.full) {
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "report too long");
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        err = httpd_resp_send(req, out, (ssize_t)w.n);
    }
    heap_caps_free(out);
    return err;
}

}  // namespace

void ui_fit_register(httpd_handle_t server) {
    httpd_uri_t uri{};
    uri.uri = "/api/i18n/overflow";
    uri.method = HTTP_GET;
    uri.handler = handle_overflow;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) ESP_LOGW(TAG, "/api/i18n/overflow not registered");
}
