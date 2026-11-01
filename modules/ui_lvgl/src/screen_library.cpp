/**
 * @file screen_library.cpp
 * @brief "Бібліотека" — every track on the card, one tap from playing it.
 *
 * The WHOLE card — indexed once, every folder, no browsing — because choosing the card is
 * choosing to listen to your music, not to go looking for it a directory at a time. The page
 * opens on the track the queue is on; a tap plays a track and goes back to the Player, which
 * is where the question the list was opened to answer gets its answer.
 *
 * It was a panel over the Player until 24.09.2026. As a page of its own it has the shell's
 * header — ← and a title — like every other list, instead of a close button of its own.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "playlist.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::P;
using i18n::S;
using i18n::tr;
using modesp::gen::AUDIO_SOURCE_FILE;

constexpr int32_t CAP_H  = 36;                     // the count, above the list

struct Page {
    UiCtx*     ctx = nullptr;
    m3w::VList list;
    lv_obj_t*  cap   = nullptr;
    lv_obj_t*  empty = nullptr;
    uint32_t   seen_gen = 0xFFFFFFFF;
    int        shown_cursor = -2;
    int8_t     shown_playing = -1;
    char       cap_txt[64] = "";
};

Page g;

bool card_playing(const UiLvglModule::Snapshot& s) {
    return s.source == AUDIO_SOURCE_FILE && strcmp(s.play_state, "playing") == 0;
}

/// A file name without its extension: the row is read as a title, and ".mp3" after every one
/// of them is noise. Only a short, plain ending counts as an extension, so "Mr. Brown" keeps
/// what it has.
void strip_ext(char* name) {
    char* dot = strrchr(name, '.');
    if (!dot || dot == name) return;
    const size_t n = strlen(dot + 1);
    if (n < 2 || n > 4) return;
    for (const char* p = dot + 1; *p; p++) {
        const char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return;
    }
    *dot = '\0';
}

void fill(int idx, m3w::VItem& it, void*) {
    static char name[player_core::Playlist::MAX_NAME];
    static char folder[player_core::Playlist::MAX_DIR];
    auto& q = player_core::playlist();
    if (!q.name_at(idx, name, sizeof(name)))       name[0] = '\0';
    if (!q.folder_at(idx, folder, sizeof(folder))) folder[0] = '\0';
    strip_ext(name);
    const bool cur = (idx == q.cursor());
    it.lead  = UI_SYM_MUSIC;
    it.title = name[0] ? name : "—";
    it.sub   = folder[0] ? folder : nullptr;            // "" is the top of the card
    it.trail = (cur && g.ctx && card_playing(g.ctx->s)) ? UI_SYM_GRAPHIC_EQ : nullptr;
    it.on    = cur;
}

void pick(int idx, void*) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(UiLvglModule::Req::PLAY_QUEUE, idx);
    ui_shell_show_player();
}

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    g.cap = m3w::text(root, t.t.label_m, t.c.on_surface_var, "");
    lv_obj_set_pos(g.cap, 16, 4);
    g.list.build(root, 0, CAP_H, uig::CONT_W, uig::CONT_H - CAP_H, fill, pick, nullptr, nullptr);
    g.empty = m3w::text(root, t.t.body_l, t.c.on_surface_var, tr(S::lib_empty));
    lv_obj_set_pos(g.empty, 16, CAP_H + 16);
    uih::show(g.empty, false);
    return root;
}

void refresh(lv_obj_t*, UiCtx& c) {
    auto& q = player_core::playlist();
    const uint32_t gen = q.generation();
    bool repaint = false;
    if (gen != g.seen_gen && !(gen & 1u)) {           // odd: the player is rebuilding it
        g.seen_gen = gen;
        const int count = q.count();
        g.list.set_count(count);
        char cap[64], n[12];
        snprintf(n, sizeof(n), "%d", count);
        i18n::fmt(cap, sizeof(cap), i18n::plural(P::lib_on_card, count), {{"n", n}});
        uih::set_text(g.cap, g.cap_txt, sizeof(g.cap_txt), cap);
        // an empty card says so once: «Картка порожня…», not «0 треків на картці» over it
        uih::show(g.cap, count > 0);
        uih::show(g.empty, count == 0);
        g.list.reveal(q.cursor());
        repaint = true;
    }
    const int cur = q.cursor();
    const int8_t playing = card_playing(c.s) ? 1 : 0;
    if (repaint || cur != g.shown_cursor || playing != g.shown_playing) {
        g.shown_cursor = cur;
        g.shown_playing = playing;
        g.list.refill();                               // the highlight moves; the list does not
    }
}

void shown(lv_obj_t*, UiCtx&) {
    // Land on the track the queue is on — once, on opening. Following it while the page is
    // open would drag the list out from under a finger every time a track ends.
    g.list.reveal(player_core::playlist().cursor());
}

}  // namespace

const UiScreenDef UI_SCREEN_LIBRARY = {"library", UI_SYM_LIBRARY, build, refresh, shown, nullptr};
