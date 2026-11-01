/**
 * @file screen_dlna.cpp
 * @brief "DLNA" — the media servers on the network, their folders and their tracks.
 *
 * A browser, so it has a hierarchy of its own: the header's ← climbs it a level at a time and
 * leaves the page from the top, where the servers are. A tap on a server or a folder opens
 * it; a tap on a track plays it and goes to the Player; the + at the end of a track's row
 * queues it to play next instead, and the page stays. The servers are looked for when the
 * page opens and none have been found yet — a page that asks to be told to look is a page
 * that looks broken.
 *
 * The browsing itself runs in components/dlna_client on a task of its own; this page draws
 * the snapshot it publishes and posts commands stamped with the snapshot's revision, so a tap
 * on a row that has since changed does nothing rather than the wrong thing.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "dlna_browser.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <new>

namespace {

using i18n::S;
using i18n::tr;

// ‹ › ⟳ a finger's 64 px, 8 apart
constexpr int32_t BAR_H  = 68;
constexpr int32_t BTN    = 64;
constexpr int32_t BTN_X3 = uig::CONT_W - BTN;              // refresh / cancel
constexpr int32_t BTN_X2 = BTN_X3 - BTN - 8;               // next page
constexpr int32_t BTN_X1 = BTN_X2 - BTN - 8;               // previous page
constexpr int32_t TXT_W  = BTN_X1 - 16 - 8;


struct Page {
    UiCtx*      ctx = nullptr;
    m3w::VList  list;
    lv_obj_t*   where  = nullptr;
    lv_obj_t*   status = nullptr;
    m3w::Button prev, next, act;       ///< act: find servers when idle, cancel while busy
    lv_obj_t*   empty = nullptr;
    bool        painted = false;
};

Page g;
// The two snapshots live in PSRAM and outlive a rebuild of the object tree, which would
// otherwise leak them: the one on screen, and the one just taken to compare it with.
dlna_client::View* s_view  = nullptr;
dlna_client::View* s_fresh = nullptr;

dlna_client::View* alloc_view() {
    void* m = heap_caps_malloc(sizeof(dlna_client::View), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return m ? new (m) dlna_client::View{} : nullptr;
}

void post(dlna_client::Command command, int row = 0) {
    if (g.ctx && g.ctx->mod && s_view) {
        g.ctx->mod->lvgl_post(UiLvglModule::Req::DLNA_BROWSE,
                              dlna_client::encode(command, row, s_view->revision));
    }
}

// ── the list ──

void fill(int idx, m3w::VItem& it, void*) {
    if (!s_view || idx >= s_view->count) return;
    const auto& r = s_view->rows[idx];
    it.lead    = s_view->servers ? UI_SYM_HUB : r.folder ? UI_SYM_FOLDER : UI_SYM_MUSIC;
    it.title   = r.title[0] ? r.title : "—";
    it.sub     = r.folder ? nullptr : r.playable ? r.subtitle : tr(S::dlna_unsupported);
    it.trail   = r.folder ? UI_SYM_CHEVRON : r.playable ? UI_SYM_PLAYLIST_ADD : nullptr;
    it.enabled = !s_view->busy && (r.folder || r.playable);
}

void pick(int idx, void*) {
    if (!s_view || idx >= s_view->count || s_view->busy) return;
    const auto& r = s_view->rows[idx];
    post(dlna_client::Command::Open, idx);
    if (!r.folder && r.playable) ui_shell_show_player();   // a track: listening is shown there
}

/// The trailing button: a folder's chevron opens it like the row does; a track's + queues it.
void trail_pick(int idx, void*) {
    if (!s_view || idx >= s_view->count || s_view->busy) return;
    post(s_view->rows[idx].folder ? dlna_client::Command::Open : dlna_client::Command::Queue, idx);
}

void prev_cb(lv_event_t*) { post(dlna_client::Command::PrevPage); }
void next_cb(lv_event_t*) { post(dlna_client::Command::NextPage); }
void act_cb(lv_event_t*) {
    if (!s_view) return;
    post(s_view->busy ? dlna_client::Command::Cancel : dlna_client::Command::Discover);
}

// ── build ──

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    if (!s_view)  s_view  = alloc_view();
    if (!s_fresh) s_fresh = alloc_view();
    if (s_view) *s_view = dlna_client::View{};

    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);

    g.where = m3w::text(root, t.t.body_l, t.c.on_surface, "");
    lv_obj_set_pos(g.where, 16, (BTN - 25 - 2 - 21) / 2);
    lv_obj_set_size(g.where, TXT_W, lv_font_get_line_height(t.t.body_l));
    lv_label_set_long_mode(g.where, LV_LABEL_LONG_DOT);
    g.status = m3w::text(root, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_pos(g.status, 16, (BTN - 25 - 2 - 21) / 2 + 27);
    lv_obj_set_size(g.status, TXT_W, lv_font_get_line_height(t.t.label_s));
    lv_label_set_long_mode(g.status, LV_LABEL_LONG_DOT);

    g.prev = m3w::icon_button(root, BTN_X1, 0, BTN, UI_SYM_CHEVRON_LEFT, t.t.icon_m);
    lv_obj_add_event_cb(g.prev.obj, prev_cb, LV_EVENT_CLICKED, nullptr);
    g.next = m3w::icon_button(root, BTN_X2, 0, BTN, UI_SYM_CHEVRON, t.t.icon_m);
    lv_obj_add_event_cb(g.next.obj, next_cb, LV_EVENT_CLICKED, nullptr);
    g.act = m3w::icon_button(root, BTN_X3, 0, BTN, UI_SYM_REFRESH, t.t.icon_m);
    lv_obj_add_event_cb(g.act.obj, act_cb, LV_EVENT_CLICKED, nullptr);

    g.list.build(root, 0, BAR_H, uig::CONT_W, uig::CONT_H - BAR_H, fill, pick, trail_pick, nullptr);
    g.empty = m3w::text(root, t.t.body_l, t.c.on_surface_var, "");
    lv_obj_set_pos(g.empty, 16, BAR_H + 16);
    uih::show(g.empty, false);
    if (!s_view || !s_fresh) lv_label_set_text(g.status, tr(S::common_no_memory));
    return root;
}

// ── refresh ──

void paint_bar() {
    const auto& v = *s_view;
    // Both may be messages of the catalog (dlna_browser.cpp: the servers' list, every status
    // line); a folder's own name is shown as it is.
    char buf[160];
    i18n::render(buf, sizeof(buf), v.title);
    lv_label_set_text(g.where, buf);
    i18n::render(buf, sizeof(buf), v.status);
    lv_label_set_text(g.status, buf);
    m3w::button_enable(g.prev, v.has_prev && !v.busy);
    m3w::button_enable(g.next, v.has_next && !v.busy);
    // Paging exists inside a server only; at the top the two arrows would be two dead buttons.
    m3w::show(g.prev.obj, !v.servers);
    m3w::show(g.next.obj, !v.servers);
    // Finding servers is the servers' view's action; inside a folder it would throw the user
    // out to the top. Cancel is there whenever something is being fetched.
    if (g.act.icon) lv_label_set_text(g.act.icon, v.busy ? UI_SYM_CLOSE : UI_SYM_REFRESH);
    m3w::show(g.act.obj, v.busy || v.servers);
    const bool none = v.count == 0 && !v.busy;
    lv_label_set_text(g.empty, tr(v.servers ? S::dlna_none : S::dlna_empty));
    uih::show(g.empty, none);
}

void refresh(lv_obj_t*, UiCtx&) {
    if (!s_view || !s_fresh || !dlna_client::snapshot(*s_fresh)) return;
    const auto& f = *s_fresh;
    const auto& v = *s_view;
    const bool new_rev = !g.painted || f.revision != v.revision;
    const bool changed = new_rev || f.busy != v.busy || f.has_prev != v.has_prev ||
                         f.has_next != v.has_next || f.can_back != v.can_back ||
                         strcmp(f.status, v.status) != 0 || strcmp(f.title, v.title) != 0;
    if (!changed) return;
    *s_view = f;
    g.painted = true;
    paint_bar();
    if (!new_rev) { g.list.refill(); return; }     // busy came or went: rows grey or wake
    g.list.set_count(s_view->count);
}

void shown(lv_obj_t*, UiCtx&) {
    if (s_view && dlna_client::snapshot(*s_view)) {
        g.painted = false;                          // redraw from this snapshot on the next pass
        if (s_view->servers && s_view->count == 0 && !s_view->busy) post(dlna_client::Command::Discover);
    }
}

/// The header's ←: up a level while there is one, and out of the page from the top.
bool back(UiCtx&) {
    if (!s_view || !s_view->can_back || s_view->busy) return false;
    post(dlna_client::Command::Back);
    return true;
}

}  // namespace

const UiScreenDef UI_SCREEN_DLNA = {"dlna", UI_SYM_CAST, build, refresh, shown, back};
