/* library.c: the Library panel over the player area, drawn with the GUI kit.
 * The Library's state (levels, pages, requests) and its navigation stay in
 * ui.c; this file draws them and turns clicks and keys into calls there. */
#include "library.h"

#include <stdio.h>
#include <string.h>

#include "../ui_internal.h"
#include "common.h"

/* The root's sections, each a screen of its list. */
static const struct {
    const char *name;
    SpList      list;
} LIB_SECTIONS[] = {
    { "Liked Songs", SP_LIST_LIKED },      { "Playlists", SP_LIST_PLAYLISTS },
    { "Albums", SP_LIST_ALBUMS },          { "Artists", SP_LIST_ARTISTS },
    { "Podcasts", SP_LIST_SHOWS },         { "Your Episodes", SP_LIST_EPISODES },
    { "Recently played", SP_LIST_RECENT }, { "Queue", SP_LIST_QUEUE },
};
#define LIB_SECTIONS_N ((int)(sizeof LIB_SECTIONS / sizeof LIB_SECTIONS[0]))

/* Search results of every kind on one screen, grouped by kind. */
static int lib_grouped(const Level *lv)
{
    return lv->kind == LV_SEARCH && lv->types == SP_SEARCH_ALL;
}

/* The heading over row i, or NULL: search results group by kind, the queue
 * into what plays now and what comes next. */
static const char *lib_heading(const Level *lv, int i)
{
    const Item *v = lv->items.v;
    if (lib_grouped(lv)) {
        int g = browse_group(v[i].kind);
        return i == 0 || browse_group(v[i - 1].kind) != g ? browse_group_name(g) : NULL;
    }
    if (lv->kind == LV_LIST && lv->list == SP_LIST_QUEUE) {
        if (i == 0 && lv->page.now_playing)
            return "Now playing";
        return i == lv->page.now_playing ? "Next up" : NULL;
    }
    return NULL;
}

/* A heading's line, with a gap to its first row. */
static float lib_head_h(const Metrics *m)
{
    return m->line + ROW_GAP;
}

/* Row i's height; the one after the items is the footer. */
static float lib_row_h(const Ui *u, const Level *lv, int i)
{
    if (lv->kind == LV_ROOT || i >= lv->items.n)
        return u->m.row;
    return u->m.row2 + (lib_heading(lv, i) ? lib_head_h(&u->m) : 0.0f);
}

/* The footer under the items, if any: more on the way, a page that failed
 * (a click tries again), or the cap. */
static int lib_footer(const Level *lv, char *out, size_t cap)
{
    if (lv->want)
        snprintf(out, cap, "Loading" ELLIPSIS);
    else if (lv->err != SP_OK)
        snprintf(out, cap, "Could not load more. Click to try again.");
    else if (lv->items.n >= BROWSE_KEEP && lv->page.more)
        snprintf(out, cap, "Showing first %d", BROWSE_KEEP);
    else
        return 0;
    return 1;
}

/* How many of the rows, from lv->scroll on, fit in avail. The mouse wheel
 * moves lv->scroll first, never past where the last row shows. */
static int lib_scroll(Ui *u, Level *lv, int rows, float avail)
{
    u->wheel += gui_input.wheel;
    int step = (int)u->wheel; /* whole rows; the rest adds up */
    u->wheel -= (float)step;
    lv->scroll -= step;
    int last = rows; /* the first row of the last screenful */
    for (float used = 0.0f; last > 0; last--) {
        used += lib_row_h(u, lv, last - 1) + (last < rows ? ROW_GAP : 0.0f);
        if (used > avail)
            break;
    }
    if (lv->scroll > last)
        lv->scroll = last;
    if (lv->scroll < 0)
        lv->scroll = 0;
    int n = 0;
    for (float used = 0.0f; lv->scroll + n < rows; n++) {
        used += lib_row_h(u, lv, lv->scroll + n) + (n ? ROW_GAP : 0.0f);
        if (used > avail)
            break;
    }
    return n;
}

/* The root: the search field (typing always goes there; Enter searches
 * every kind) and the sections. */
static void draw_lib_root(Ui *u, GuiRect area, Level *lv)
{
    const Metrics *m = &u->m;
    gui_edit_line(u->query, sizeof u->query, 0);
    if (gui_input.key_enter && u->query[strspn(u->query, " ")]) {
        char title[sizeof lv->title];
        snprintf(title, sizeof title, "\xE2\x80\x9C%s\xE2\x80\x9D", u->query); /* in quotes */
        lib_open_search(u, u->query, SP_SEARCH_ALL, title);
        return;
    }
    draw_field(u, (GuiRect){ area.x, area.y, area.w, m->row }, u->query, "Type to search, Enter");
    float y = area.y + m->row + 10;
    int n = lib_scroll(u, lv, LIB_SECTIONS_N, area.y + area.h - y);
    for (int i = lv->scroll; i < lv->scroll + n; i++, y += m->row + ROW_GAP) {
        GuiRect r = { area.x, y, area.w, m->row };
        if (gui_button(W_LIB_ROW + i, r, NULL, px_of(u, FS_S), 0)) {
            lib_open_list(u, LIB_SECTIONS[i].list, LIB_SECTIONS[i].name, "");
            return;
        }
        text(u, LIB_SECTIONS[i].name, r.x + ROW_PAD, mid_y(u, r), FS_S, GUI_TEXT);
        if (LIB_SECTIONS[i].list == SP_LIST_LIKED && lv->loaded && lv->page.total >= 0) {
            char total[32];
            snprintf(total, sizeof total, "%d song%s", lv->page.total, lv->page.total == 1 ? "" : "s");
            text(u, total, r.x + r.w - ROW_PAD - text_w(u, total, FS_S), mid_y(u, r), FS_S, GUI_DIM);
        }
    }
}

/* Row i of lv in r, under its heading if it starts a group. A track or
 * episode plays and has +Queue, a container opens, anything else is
 * dimmed. Returns 1 when it opened a screen. */
static int draw_lib_row(Ui *u, Level *lv, int i, GuiRect r)
{
    const Metrics *m = &u->m;
    const Item *it = &lv->items.v[i];
    const char *head = lib_heading(lv, i);
    if (head) {
        GuiRect hr = { r.x, r.y, r.w, m->line };
        text(u, head, hr.x, mid_y(u, hr), FS_S, GUI_DIM);
        int g = browse_group(it->kind), in_group = 0;
        for (int k = i; lib_grouped(lv) && k < lv->items.n && browse_group(lv->items.v[k].kind) == g; k++)
            in_group++;
        if (in_group >= SP_SEARCH_LIMIT) { /* a full share: Spotify has more */
            float sw = text_w(u, "See all", FS_S) + 2 * ROW_PAD;
            GuiRect rs = { r.x + r.w - sw, r.y, sw, m->line };
            if (gui_button(W_LIB_MORE + g, rs, "See all", px_of(u, FS_S), 0)) {
                char title[sizeof lv->title];
                snprintf(title, sizeof title, "%s: \xE2\x80\x9C%s\xE2\x80\x9D", browse_group_name(g),
                         lv->query);
                lib_open_search(u, lv->query, browse_group_type(g), title);
                return 1;
            }
        }
        r.y += lib_head_h(m);
        r.h -= lib_head_h(m);
    }
    PlayTarget t;
    int track = it->kind == ITEM_TRACK || it->kind == ITEM_EPISODE;
    int plays = track && browse_play_target(lib_list_of(lv), lv->uri, it, &t) == 0;
    int opens = !track && it->playable && it->uri[0];
    GuiRect rb = r;
    if (track) { /* the +Queue column, kept by unplayable rows too so they line up */
        float qw = text_w(u, "+Queue", FS_S) + (float)m->fs;
        GuiRect rq = { r.x + r.w - qw, r.y + (r.h - m->bar) / 2.0f, qw, m->bar };
        rb.w -= qw + ROW_GAP;
        if (plays && gui_button(W_LIB_QUEUE + i, rq, "+Queue", px_of(u, FS_S), 0))
            cmd_post_uri(&u->cmd, CMD_QUEUE, 0, it->uri, NULL);
    }
    if (!plays && !opens) {
        gui_fill(rb, 0.3f, GUI_PRESS); /* Spotify can't play it */
    } else if (gui_button(W_LIB_ROW + i, rb, NULL, px_of(u, FS_S), 0)) {
        if (opens) {
            lib_open_item(u, it);
            return 1;
        }
        cmd_post_uri(&u->cmd, CMD_PLAY_URI, t.liked, t.item, t.ctx);
    }
    GuiColor name_c = !plays && !opens                                  ? GUI_DIM
                    : u->st.active && !strcmp(it->uri, u->st.track.uri) ? GUI_ACCENT
                                                                        : GUI_TEXT;
    char sub[SP_NAME_LEN], info[32], fit[SP_NAME_LEN];
    float tx = rb.x + ROW_PAD, tw = rb.w - 2 * ROW_PAD;
    browse_row_text(it, sub, sizeof sub, info, sizeof info);
    gui_fit_text(px_of(u, FS_S), it->name, tw, fit, sizeof fit);
    if (!sub[0] && !info[0]) { /* one line (an artist) */
        text(u, fit, tx, mid_y(u, rb), FS_S, name_c);
        return 0;
    }
    float ty = rb.y + (rb.h - (2.0f * (float)m->fs + LINE_GAP)) / 2.0f, iw = 0.0f;
    text(u, fit, tx, ty, FS_S, name_c);
    ty += (float)m->fs + LINE_GAP;
    if (info[0]) {
        iw = text_w(u, info, FS_S);
        text(u, info, rb.x + rb.w - ROW_PAD - iw, ty, FS_S, GUI_DIM);
        iw += ROW_PAD;
    }
    gui_fit_text(px_of(u, FS_S), sub, tw - iw, fit, sizeof fit);
    text(u, fit, tx, ty, FS_S, GUI_DIM);
    return 0;
}

/* A list, a container or search results: Back, the title and Play (for a
 * context) on top, then the rows; scrolling to the end loads more. */
static void draw_lib_list(Ui *u, GuiRect area, Level *lv)
{
    const Metrics *m = &u->m;
    PlayTarget pt;
    int has_play = browse_play_target(lib_list_of(lv), lv->uri, NULL, &pt) == 0;
    float bw = btn_w(u, "Back", 0.0f), pw = btn_w(u, "Play", 0.0f);
    GuiRect back = { area.x, area.y, bw, m->row };
    GuiRect rp = { area.x + area.w - pw, area.y, pw, m->row };
    if (gui_button(W_LIB_BACK, back, "Back", px_of(u, FS_S), 0)) {
        lib_pop(u);
        return;
    }
    if (has_play && gui_button(W_LIB_PLAY, rp, "Play", px_of(u, FS_S), 1))
        cmd_post_uri(&u->cmd, CMD_PLAY_URI, pt.liked, pt.item, pt.ctx);
    char fit[sizeof lv->title];
    float tx = back.x + bw + 10, tr = has_play ? rp.x - 10 : area.x + area.w;
    gui_fit_text(px_of(u, FS_S), lv->title, tr - tx, fit, sizeof fit);
    text(u, fit, tx, mid_y(u, back), FS_S, GUI_TEXT);

    float y = area.y + m->row + 10;
    if (lv->page.restricted)
        y += para(u, "Spotify only lists songs of your own playlists here.", area.x, y, area.w,
                  GUI_DIM) + 10;
    int n = lv->items.n;
    if (n == 0) {
        if (lv->want) {
            text(u, "Loading" ELLIPSIS, area.x, y + 4, FS_S, GUI_DIM);
        } else if (lv->err == SP_ERR_SCOPE) {
            y += para(u, "Sign in again to allow this.", area.x, y + 4, area.w, GUI_DIM) + 12;
            GuiRect rs = { area.x, y, btn_w(u, "Sign in again", 0.0f), m->row };
            if (gui_button(W_LIB_SIGNIN, rs, "Sign in again", px_of(u, FS_S), 1))
                start_reauth(u, 1);
        } else if (lv->err != SP_OK) {
            para(u, sp_strerror(lv->err), area.x, y + 4, area.w, GUI_ERR);
        } else if (lv->loaded && !lv->page.restricted) {
            text(u, lv->kind == LV_SEARCH ? "No results." : "Nothing here.", area.x, y + 4, FS_S, GUI_DIM);
        }
        return;
    }
    char foot[64];
    int rows = n + lib_footer(lv, foot, sizeof foot);
    int shown = lib_scroll(u, lv, rows, area.y + area.h - y);
    for (int i = lv->scroll; i < lv->scroll + shown; i++) {
        GuiRect r = { area.x, y, area.w, lib_row_h(u, lv, i) };
        y += r.h + ROW_GAP;
        if (i < n) {
            if (draw_lib_row(u, lv, i, r))
                return;
            continue;
        }
        int retry = lv->err != SP_OK && !lv->want;
        if (retry && gui_button(W_LIB_MORE + BROWSE_GROUPS, r, NULL, px_of(u, FS_S), 0)) {
            lv->err = SP_OK;
            lib_request(u, lv);
        }
        gui_fit_text(px_of(u, FS_S), foot, r.w - 2 * ROW_PAD, fit, sizeof fit);
        text(u, fit, r.x + ROW_PAD, mid_y(u, r), FS_S, retry ? GUI_ERR : GUI_DIM);
    }
    /* The end shows: the next page. Grouped search results are one page of
     * every kind; See all has the rest of a kind. */
    if (lv->scroll + shown >= n && lv->page.more && !lv->want && lv->err == SP_OK &&
        n < BROWSE_KEEP && !lib_grouped(lv))
        lib_request(u, lv);
}

/* The Library over the player area: a stack of screens. Back or Esc goes
 * up one; Esc at the root closes the panel. */
void draw_library(Ui *u, GuiRect area)
{
    lib_sync(u);
    if (gui_input.key_escape) {
        if (u->depth > 0)
            lib_pop(u);
        else
            u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    Level *lv = &u->lv[u->depth];
    if (lv->kind == LV_ROOT)
        draw_lib_root(u, area, lv);
    else
        draw_lib_list(u, area, lv);
}
