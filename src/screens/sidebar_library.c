/* sidebar_library.c: the Library tab of the sidebar (sidebar_tabs.h): its
 * title and search field over the content, which is the sections (they
 * open one at a time), or a page of the stack: the search results, or what
 * a playlist, album, artist or show row holds, each with its way back and
 * its Play, deeper ones on pages of their own so going back finds a page
 * as it was left. What it shows is library.c's; this file makes the rows
 * and turns clicks into calls there and to ui.c. */
#include "screens/sidebar_tabs.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browse.h"
#include "gui/components.h"
#include "library.h"
#include "platform.h"
#include "ui_internal.h"

#define ELLIPSIS "\xE2\x80\xA6" /* U+2026 */
#define MIDDOT   "\xC2\xB7"     /* U+00B7 */

#define SECTION_GAP 4.0f /* between the sections */
#define ROW_GAP     2.0f /* between the rows of a list */
#define TEXT_INSET  10.0f /* comp_row's padding: lines outside rows line up with theirs */
#define FOLLOW_MS   900  /* an opened section keeps its header on top at least this long (the spring) */

/* The lists shown: the sections', then the pages of the stack (depth 1..). */
#define VIEW_LEVEL(depth) (LIB_SECTIONS + (depth) - 1)
#define VIEWS             (LIB_SECTIONS + LIB_DEPTH - 1)

/* A row's list and item, in the ud of its callbacks. */
#define ROW_UD(view, item) ((void *)(intptr_t)((view) << 16 | (item)))
#define UD_VIEW(ud)        ((int)((intptr_t)(ud) >> 16))
#define UD_ITEM(ud)        ((int)((intptr_t)(ud) & 0xFFFF))

/* What follows the rows of a list. */
typedef enum { FOOT_NONE, FOOT_LOADING, FOOT_SCOPE, FOOT_ERROR, FOOT_EMPTY, FOOT_CAP, FOOT_MORE } Foot;

/* A list as rows. They are made as its pages come and kept while it holds
 * the same items (Level.id, Level.epoch): item i is row[i]. */
typedef struct {
    Rid note;     /* over the rows: why a playlist lists nothing */
    Rid rows;     /* VBox: the rows, and the headings of groups */
    Rid foot;     /* VBox under them */
    CompRow *row;
    int n, cap;
    uint64_t id, epoch, gen; /* of the Level shown */
    Foot foot_kind;
    int foot_err, foot_n, foot_total;
} View;

/* A page of the stack: the way back, what it shows, Play, its list. */
typedef struct {
    Rid page, title, detail, play, scroll;
} LevelPage;

static struct {
    Ui *u;
    Rid search, content, root_scroll;
    CompSection sec[LIB_SECTIONS];
    LevelPage lp[LIB_DEPTH]; /* [1..LIB_DEPTH-1] */
    View view[VIEWS];
    uint64_t gen;            /* lib_gen shown */
    int depth;               /* the page shown: the sections (0) or lib_level(depth) */
    int searching;           /* a search was on the stack */
    char playing[SP_URI_LEN]; /* what the rows mark as playing; "" = nothing */
    uint64_t note_until;     /* u->note_until seen: a queue add */
    /* The section just opened, brought to the top of the list while it
     * opens and its first page comes: -1 = none. */
    int follow;
    float follow_at;         /* the offset that puts its header on top */
    uint64_t follow_from;    /* plat_now_ms when it opened */
} g;

static Level *view_level(int v)
{
    return v < LIB_SECTIONS ? lib_section(v) : lib_level(v - LIB_SECTIONS + 1);
}

/* The rows of view v show what lv holds now. */
static int view_current(int v, const Level *lv)
{
    return lv && lv->id && g.view[v].id == lv->id && g.view[v].epoch == lv->epoch;
}

/* The list and item of a row clicked; NULL when its list changed since. */
static const Item *row_item(void *ud, Level **out)
{
    int v = UD_VIEW(ud), i = UD_ITEM(ud);
    Level *lv = v >= 0 && v < VIEWS ? view_level(v) : NULL;
    if (!view_current(v, lv) || i >= lv->items.n)
        return NULL;
    *out = lv;
    return &lv->items.v[i];
}

/* n with thousands separators: 2,434. */
static void fmt_count(char out[16], int n)
{
    char d[16];
    int len = snprintf(d, sizeof d, "%d", n < 0 ? 0 : n), j = 0;
    for (int i = 0; i < len && j < 14; i++) {
        if (i > 0 && (len - i) % 3 == 0)
            out[j++] = ',';
        out[j++] = d[i];
    }
    out[j] = 0;
}

static int is_playing(const Item *it)
{
    return g.playing[0] && strcmp(it->uri, g.playing) == 0;
}

/* --- clicks ---------------------------------------------------------------------- */

/* A track or episode plays (in its album, playlist, show or Liked Songs,
 * else on its own); an album, artist, playlist or show opens. */
static void on_row(Rid button, void *ud)
{
    (void)button;
    Level *lv;
    const Item *it = row_item(ud, &lv);
    PlayTarget t;
    if (!it)
        return;
    if (it->kind == ITEM_TRACK || it->kind == ITEM_EPISODE) {
        if (browse_play_target(lib_list_of(lv), lv->uri, it, &t) == 0)
            cmd_post_uri(&g.u->cmd, CMD_PLAY_URI, t.liked, t.item, t.ctx);
    } else if (it->playable && it->uri[0]) {
        lib_open_item(g.u, it);
    }
}

static void on_queue(Rid button, void *ud)
{
    (void)button;
    Level *lv;
    const Item *it = row_item(ud, &lv);
    if (it && it->playable && it->uri[0])
        cmd_post_uri(&g.u->cmd, CMD_QUEUE, 0, it->uri, NULL);
}

static void on_more(Rid button, void *ud)
{
    (void)button;
    int v = UD_VIEW(ud);
    Level *lv = view_level(v);
    if (view_current(v, lv))
        lib_more(g.u, lv);
}

static void on_again(Rid button, void *ud)
{
    (void)button;
    int v = UD_VIEW(ud);
    Level *lv = view_level(v);
    if (view_current(v, lv))
        lib_again(g.u, lv);
}

static void on_reauth(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    start_reauth(g.u, 1);
}

/* A kind of the search results, all of it, on a page of its own. */
static void on_see_all(Rid button, void *ud)
{
    (void)button;
    int v = UD_VIEW(ud), group = UD_ITEM(ud);
    Level *lv = view_level(v);
    if (view_current(v, lv) && lv->kind == LV_SEARCH)
        lib_open_search(g.u, lv->query, browse_group_type(group), browse_group_name(group));
}

static void on_back(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    lib_pop(g.u);
}

/* Play on a page of the stack: the playlist, album, artist or show. */
static void on_play(Rid button, void *ud)
{
    (void)button;
    Level *lv = lib_level((int)(intptr_t)ud);
    PlayTarget t;
    if (lv && browse_play_target(lib_list_of(lv), lv->uri, NULL, &t) == 0)
        cmd_post_uri(&g.u->cmd, CMD_PLAY_URI, t.liked, t.item, t.ctx);
}

static void on_play_liked(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    PlayTarget t;
    if (browse_play_target(SP_LIST_LIKED, NULL, NULL, &t) == 0)
        cmd_post_uri(&g.u->cmd, CMD_PLAY_URI, t.liked, t.item, t.ctx);
}

/* The section that opens comes to the top of the list: the offset where
 * its header will be once the one open before has closed (every header
 * above it closed), kept while it opens and its first page comes
 * (follow_open). Its rect is still a closed header's, unless it was
 * closing; the smallest header is a closed one. */
static void follow_start(int section)
{
    float x, w, h, view, first, header = 0.0f, offset;
    Rwidget_get_rect(g.root_scroll, &x, &view, &w, &h);
    Rwidget_get_rect(g.sec[0].expander, &x, &first, &w, &h);
    for (int s = 0; s < LIB_SECTIONS; s++) {
        Rwidget_get_rect(g.sec[s].expander, &x, &offset, &w, &h);
        if (s == 0 || h < header)
            header = h;
    }
    Rscroll_box_get_offset(g.root_scroll, NULL, &offset);
    g.follow = section;
    g.follow_at = first - view + offset + (float)section * (header + SECTION_GAP);
    g.follow_from = plat_now_ms();
}

/* Each pass while it follows: the offset goes to the header (the scroll
 * box lays out first and stops at the end of what there is so far). Ends
 * once the spring has settled and the first page is in, or when the user
 * scrolls (follow_event) or the section closes. */
static void follow_open(void)
{
    if (g.follow < 0)
        return;
    const Level *lv = lib_section(g.follow);
    if (lib_section_open() != g.follow || g.depth != 0 || !lv) {
        g.follow = -1;
        return;
    }
    float oy;
    Rscroll_box_get_offset(g.root_scroll, NULL, &oy);
    if (fabsf(oy - g.follow_at) > 0.5f)
        Rscroll_box_set_offset(g.root_scroll, 0.0f, g.follow_at);
    if (plat_now_ms() - g.follow_from >= FOLLOW_MS && !lv->want && !lv->need)
        g.follow = -1;
}

/* The wheel or a press on the list (its scroll bar) leaves the offset to
 * the user. */
static int follow_event(Rid widget, const REvent *event, void *ud)
{
    (void)widget;
    (void)ud;
    if (event->type == R_EVENT_MOUSE_WHEEL || event->type == R_EVENT_MOUSE_DOWN)
        g.follow = -1;
    return R_FALSE;
}

static void on_section(Rid expander, int expanded, void *ud)
{
    (void)expander;
    int s = (int)(intptr_t)ud;
    if (!expanded) {
        lib_close_section(g.u, s);
        return;
    }
    lib_open_section(g.u, s);
    follow_start(s);
}

/* Enter: every kind of result for what was typed; nothing typed: back to
 * the sections. */
static void on_search_commit(Rid input, const char *text, void *ud)
{
    (void)input;
    (void)ud;
    const char *q = text + strspn(text, " ");
    if (q[0])
        lib_search(g.u, q);
    else
        lib_home(g.u);
}

/* The field emptied: the sections come back. */
static void on_search_change(Rid input, const char *text, void *ud)
{
    (void)input;
    (void)ud;
    if (!text[0] && lib_searching())
        lib_home(g.u);
}

/* Escape empties the field and leaves the results; an empty one lets the
 * keyboard go. */
static int on_search_key(Rid input, const REvent *event, void *ud)
{
    (void)ud;
    if (event->type != R_EVENT_KEY_DOWN || event->key != R_KEY_ESCAPE)
        return R_FALSE;
    if (!Rtext_input_get_text(input)[0] && !lib_searching())
        return R_FALSE;
    comp_field_set(input, "");
    if (lib_searching())
        lib_home(g.u);
    return R_TRUE;
}

/* --- lists ------------------------------------------------------------------------- */

static void view_build(View *w, Rid parent)
{
    w->note = comp_label(parent, "Spotify only lists songs of your own playlists here. Play still plays it.",
                         TEXT_CAPTION);
    Rwidget_add_class(w->note, "muted");
    Rslot_set_padding(w->note, TEXT_INSET, 4.0f, TEXT_INSET, 4.0f);
    comp_set_visible(w->note, 0);
    w->rows = comp_vbox(parent, ROW_GAP);
    w->foot = comp_vbox(parent, 2.0f);
    w->foot_kind = (Foot)-1;
}

static void view_clear(View *w)
{
    if (w->n || Rwidget_get_child_count(w->rows))
        Rwidget_clear_children(w->rows);
    w->n = 0;
    w->id = w->epoch = 0;
    w->foot_kind = (Foot)-1;
    if (Rwidget_get_child_count(w->foot))
        Rwidget_clear_children(w->foot);
    comp_set_visible(w->note, 0);
}

/* The heading over item i, or NULL: search results group by kind (*group
 * says which), the queue into what plays now and what comes next. */
static const char *heading(const Level *lv, int i, int *group)
{
    const Item *v = lv->items.v;
    *group = -1;
    if (lib_grouped(lv)) {
        int k = browse_group(v[i].kind);
        if (i > 0 && browse_group(v[i - 1].kind) == k)
            return NULL;
        *group = k;
        return browse_group_name(k);
    }
    if (lv->kind == LV_LIST && lv->list == SP_LIST_QUEUE) {
        if (i == 0 && lv->page.now_playing)
            return "Now playing";
        return i == lv->page.now_playing ? "Next up" : NULL;
    }
    return NULL;
}

static void add_heading(int v, const Level *lv, int i)
{
    int group;
    const char *head = heading(lv, i, &group);
    if (!head)
        return;
    int full = 0; /* a whole share of a kind came: Spotify has more of it */
    for (int k = i; group >= 0 && k < lv->items.n && browse_group(lv->items.v[k].kind) == group; k++)
        full++;
    full = full >= SP_SEARCH_LIMIT;
    CompHeader h = comp_header(g.view[v].rows, head, full ? "See all" : NULL, on_see_all, ROW_UD(v, group));
    Rslot_set_padding(h.row, TEXT_INSET, i ? 10.0f : 2.0f, 0.0f, 2.0f);
}

/* Row i of lv: a track or episode plays and has +Queue; an album, artist,
 * playlist or show opens; what Spotify can't play is dimmed. */
static void add_row(int v, const Level *lv, int i)
{
    View *w = &g.view[v];
    const Item *it = &lv->items.v[i];
    char sub[SP_NAME_LEN], info[32];
    PlayTarget t;
    if (w->n == w->cap) {
        int cap = w->cap ? 2 * w->cap : 32;
        CompRow *row = realloc(w->row, (size_t)cap * sizeof *row);
        if (!row)
            return;
        w->row = row;
        w->cap = cap;
    }
    add_heading(v, lv, i);
    browse_row_text(it, sub, sizeof sub, info, sizeof info);
    int track = it->kind == ITEM_TRACK || it->kind == ITEM_EPISODE;
    int plays = track && browse_play_target(lib_list_of(lv), lv->uri, it, &t) == 0;
    int opens = !track && it->playable && it->uri[0];
    CompRow r = comp_row(w->rows, it->name[0] ? it->name : "Untitled", sub, on_row, ROW_UD(v, i));
    comp_row_set(&r, it->name[0] ? it->name : "Untitled", sub, info);
    if (track) /* unplayable rows keep it too, dimmed: the lengths line up */
        comp_row_action(&r, ICON_QUEUE, "Add to queue", on_queue, ROW_UD(v, i));
    comp_set_enabled(r.row, plays || opens);
    comp_row_set_current(&r, is_playing(it));
    w->row[w->n++] = r;
}

/* What follows the rows: more on the way, an error to try again, Show
 * more, the cap, or that there is nothing. Made again when it changes. */
static void foot_update(int v, const Level *lv)
{
    View *w = &g.view[v];
    int n = lv->items.n, total = lv->page.total;
    Foot kind = lib_loading(lv)                                  ? FOOT_LOADING
              : lv->err == SP_ERR_SCOPE                          ? FOOT_SCOPE
              : lv->err != SP_OK                                 ? FOOT_ERROR
              : !lv->loaded                                      ? FOOT_NONE
              : n == 0                                           ? (lv->page.restricted ? FOOT_NONE : FOOT_EMPTY)
              : lv->page.more && n >= BROWSE_KEEP                ? FOOT_CAP
              : lv->page.more && !lib_grouped(lv)                ? FOOT_MORE
                                                                 : FOOT_NONE;
    if (kind == w->foot_kind && lv->err == w->foot_err && n == w->foot_n && total == w->foot_total)
        return;
    w->foot_kind = kind;
    w->foot_err = lv->err;
    w->foot_n = n;
    w->foot_total = total;
    if (Rwidget_get_child_count(w->foot))
        Rwidget_clear_children(w->foot);
    Rid line = R_INVALID_ID;
    switch (kind) {
    case FOOT_NONE:
        break;
    case FOOT_LOADING:
        line = comp_text(w->foot, "Loading" ELLIPSIS, TEXT_CAPTION);
        break;
    case FOOT_EMPTY:
        line = comp_text(w->foot, lv->kind == LV_SEARCH ? "No results." : "Nothing here.", TEXT_CAPTION);
        break;
    case FOOT_CAP: {
        char cap[48];
        snprintf(cap, sizeof cap, "Showing the first %d", BROWSE_KEEP);
        line = comp_text(w->foot, cap, TEXT_CAPTION);
        break;
    }
    case FOOT_SCOPE:
    case FOOT_ERROR: {
        const char *why = kind == FOOT_SCOPE ? "Sign in again to allow this."
                        : n                  ? "Could not load more."
                                             : sp_strerror(lv->err);
        Rid text = comp_label(w->foot, why, TEXT_CAPTION);
        Rwidget_add_class(text, kind == FOOT_SCOPE ? "muted" : "error");
        Rslot_set_padding(text, TEXT_INSET, 6.0f, TEXT_INSET, 0.0f);
        CompRow r = kind == FOOT_SCOPE ? comp_row(w->foot, "Sign in again", NULL, on_reauth, NULL)
                                       : comp_row(w->foot, "Try again", NULL, on_again, ROW_UD(v, 0));
        comp_set_class(r.title, "on", 1);
        break;
    }
    case FOOT_MORE: {
        char info[48] = "", a[16], b[16];
        if (total > n) {
            fmt_count(a, n);
            fmt_count(b, total);
            snprintf(info, sizeof info, "%s of %s", a, b);
        }
        CompRow r = comp_row(w->foot, "Show more", NULL, on_more, ROW_UD(v, 0));
        comp_row_set(&r, "Show more", NULL, info);
        comp_set_class(r.title, "on", 1);
        break;
    }
    }
    if (line != R_INVALID_ID) {
        Rwidget_add_class(line, "muted");
        Rslot_set_padding(line, TEXT_INSET, 6.0f, TEXT_INSET, 6.0f);
    }
}

/* Push what lv holds into view v: rows for the items that came since,
 * all of them again when it holds another list. */
static void view_update(int v, const Level *lv)
{
    View *w = &g.view[v];
    if (w->gen == lv->gen && w->id == lv->id)
        return;
    w->gen = lv->gen;
    if (w->id != lv->id || w->epoch != lv->epoch) {
        view_clear(w);
        w->id = lv->id;
        w->epoch = lv->epoch;
    }
    comp_set_visible(w->note, lv->page.restricted && !lv->items.n);
    for (int i = w->n; i < lv->items.n && w->n == i; i++)
        add_row(v, lv, i);
    foot_update(v, lv);
}

/* The rows mark the item playing now. */
static void mark_playing(void)
{
    for (int v = 0; v < VIEWS; v++) {
        const Level *lv = view_level(v);
        if (!view_current(v, lv))
            continue;
        for (int i = 0; i < g.view[v].n && i < lv->items.n; i++)
            comp_row_set_current(&g.view[v].row[i], is_playing(&lv->items.v[i]));
    }
}

/* --- building ---------------------------------------------------------------------- */

static void level_page_build(int depth)
{
    LevelPage *p = &g.lp[depth];
    p->page = comp_vbox(g.content, COMP_GAP);
    Rid nav = comp_hbox(p->page, 6.0f);
    Rid back = comp_icon_button(nav, ICON_BACK, ICON_BUTTON_MEDIUM, "Back", on_back, NULL);
    Rslot_set_valign(back, R_VALIGN_CENTER);
    Rid texts = comp_vbox(nav, 1.0f);
    Rslot_set_fill(texts, 1.0f);
    Rslot_set_valign(texts, R_VALIGN_CENTER);
    p->title = comp_text(texts, "", TEXT_HEADLINE);
    p->detail = comp_text(texts, "", TEXT_CAPTION);
    Rwidget_add_class(p->detail, "muted");
    p->play = comp_button(nav, "Play", BUTTON_PRIMARY, on_play, (void *)(intptr_t)depth);
    Rslot_set_valign(p->play, R_VALIGN_CENTER);
    comp_set_visible(p->play, 0);
    Rid list = comp_scroll(p->page, 0.0f);
    p->scroll = Rwidget_get_parent(list);
    Rslot_set_fill(p->scroll, 1.0f);
    view_build(&g.view[VIEW_LEVEL(depth)], list);
}

void library_tab_build(Ui *u, Rid parent)
{
    g.u = u;
    comp_text(parent, "Library", TEXT_LARGE);
    g.search = comp_search_field(parent, "Search", on_search_change, on_search_commit, NULL);
    Rwidget_set_event_callback(g.search, on_search_key, NULL);
    Rwidget_set_accessible_name(g.search, "Search Spotify");

    g.content = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_SWITCHER));
    Rslot_set_fill(g.content, 1.0f);
    Rslot_set_padding(g.content, 0.0f, 4.0f, 0.0f, 0.0f);
    Rid sections = comp_scroll(g.content, SECTION_GAP);
    g.root_scroll = Rwidget_get_parent(sections);
    g.follow = -1;
    Rwidget_set_event_callback(g.root_scroll, follow_event, NULL);
    for (int s = 0; s < LIB_SECTIONS; s++) {
        g.sec[s] = comp_section(sections, lib_section_name(s), 0, on_section, (void *)(intptr_t)s);
        if (s == LIB_SEC_LIKED) {
            CompRow play = comp_row(g.sec[s].body, "Play Liked Songs", NULL, on_play_liked, NULL);
            comp_row_icon(&play, ICON_PLAY);
            comp_set_class(play.row, "on", 1);
            comp_set_class(play.title, "on", 1);
        }
        view_build(&g.view[s], g.sec[s].body);
    }
    for (int d = 1; d < LIB_DEPTH; d++)
        level_page_build(d);
    Rswitcher_set_index(g.content, 0);
}

void library_tab_find(void)
{
    Rwidget_focus(g.search);
    Rtext_input_select_all(g.search);
}

/* --- update ---------------------------------------------------------------------- */

static Rid page_of(int depth)
{
    return depth == 0 ? g.root_scroll : g.lp[depth].page;
}

/* Liked Songs says how many songs it holds once a page told. */
static void liked_title(void)
{
    const Level *root = lib_level(0), *liked = lib_section(LIB_SEC_LIKED);
    int total = root->loaded ? root->page.total : liked->loaded ? liked->page.total : -1;
    char title[64], count[16];
    if (total >= 0) {
        fmt_count(count, total);
        snprintf(title, sizeof title, "%s  " MIDDOT "  %s", lib_section_name(LIB_SEC_LIKED), count);
    } else {
        snprintf(title, sizeof title, "%s", lib_section_name(LIB_SEC_LIKED));
    }
    comp_section_set_title(&g.sec[LIB_SEC_LIKED], title);
}

static void stack_update(void)
{
    int depth = lib_depth();
    for (int d = 1; d < LIB_DEPTH; d++) {
        LevelPage *p = &g.lp[d];
        View *w = &g.view[VIEW_LEVEL(d)];
        const Level *lv = lib_level(d);
        PlayTarget t;
        if (!lv) {
            if (w->id)
                view_clear(w); /* popped: hidden, and gone */
            continue;
        }
        int fresh = w->id != lv->id;
        comp_text_set(p->title, lv->title);
        comp_text_set(p->detail, lv->detail);
        comp_set_visible(p->play, browse_play_target(lib_list_of(lv), lv->uri, NULL, &t) == 0);
        view_update(VIEW_LEVEL(d), lv);
        float oy;
        Rscroll_box_get_offset(p->scroll, NULL, &oy);
        if (fresh && oy != 0.0f) /* another list: from its top */
            Rscroll_box_set_offset(p->scroll, 0.0f, 0.0f);
    }
    if (depth != g.depth) {
        Rswitcher_set_index(g.content, depth);
        Rwidget_play_transition(page_of(depth), depth > g.depth ? R_TRANSITION_SLIDE_LEFT : R_TRANSITION_SLIDE_RIGHT,
                                0.0f);
        g.depth = depth;
    }
}

void library_tab_update(Ui *u)
{
    const char *now = u->st.active ? u->st.track.uri : "";
    int moved = strcmp(now, g.playing) != 0;
    if (moved) {
        snprintf(g.playing, sizeof g.playing, "%s", now);
        if (lib_section_open() == LIB_SEC_QUEUE) /* up next moved along */
            lib_refresh(u, lib_section(LIB_SEC_QUEUE));
    }
    if (u->note_until != g.note_until) { /* added to the queue */
        if (u->note_until > g.note_until && lib_section_open() == LIB_SEC_QUEUE)
            lib_refresh(u, lib_section(LIB_SEC_QUEUE));
        g.note_until = u->note_until;
    }
    if (lib_gen() != g.gen) {
        g.gen = lib_gen();
        int open = lib_section_open();
        for (int s = 0; s < LIB_SECTIONS; s++) {
            comp_section_set_open(&g.sec[s], s == open);
            if (s == open) /* a closed one keeps its rows until it opens again: they fold away */
                view_update(s, lib_section(s));
        }
        liked_title();
        stack_update();
        int searching = lib_searching();
        if (g.searching && !searching)
            comp_field_set(g.search, "");
        g.searching = searching;
    }
    if (moved)
        mark_playing();
    follow_open();
}
