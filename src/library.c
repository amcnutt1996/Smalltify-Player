/* library.c: the Library's sections and stack of screens (library.h), fed
 * by the library worker of ui.c. */
#include "library.h"

#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define MIDDOT "\xC2\xB7"        /* U+00B7 */
#define LQUOTE "\xE2\x80\x9C"    /* U+201C */
#define RQUOTE "\xE2\x80\x9D"    /* U+201D */

static const struct {
    const char *name;
    SpList      list;
} SECTIONS[LIB_SECTIONS] = {
    [LIB_SEC_LIKED] = { "Liked Songs", SP_LIST_LIKED },
    [LIB_SEC_PLAYLISTS] = { "Playlists", SP_LIST_PLAYLISTS },
    [LIB_SEC_ALBUMS] = { "Albums", SP_LIST_ALBUMS },
    [LIB_SEC_ARTISTS] = { "Artists", SP_LIST_ARTISTS },
    [LIB_SEC_SHOWS] = { "Podcasts", SP_LIST_SHOWS },
    [LIB_SEC_EPISODES] = { "Your Episodes", SP_LIST_EPISODES },
    [LIB_SEC_RECENT] = { "Recently played", SP_LIST_RECENT },
    [LIB_SEC_QUEUE] = { "Queue", SP_LIST_QUEUE },
};

static Level g_lv[LIB_DEPTH];     /* the stack; g_lv[g_depth] shows */
static int g_depth;
static Level g_sec[LIB_SECTIONS]; /* the sections; only g_sec[g_open] holds a list */
static int g_open = -1;
static int g_fresh = 1;           /* nothing asked since the start or lib_reset */
static uint64_t g_req_seq;        /* the last LibReq.seq */
static uint64_t g_ids;            /* the last Level.id */
static uint64_t g_gen = 1;        /* lib_gen */

/* lv changed: the module showing it looks again. */
static void touch(Level *lv)
{
    lv->gen++;
    g_gen++;
}

Level *lib_top(void)
{
    return &g_lv[g_depth];
}

int lib_depth(void)
{
    return g_depth;
}

Level *lib_level(int depth)
{
    return depth >= 0 && depth <= g_depth ? &g_lv[depth] : NULL;
}

uint64_t lib_gen(void)
{
    return g_gen;
}

const char *lib_section_name(int section)
{
    return section >= 0 && section < LIB_SECTIONS ? SECTIONS[section].name : "";
}

Level *lib_section(int section)
{
    return section >= 0 && section < LIB_SECTIONS ? &g_sec[section] : NULL;
}

int lib_section_open(void)
{
    return g_open;
}

int lib_list_of(const Level *lv)
{
    return lv->kind == LV_LIST ? (int)lv->list : -1;
}

int lib_grouped(const Level *lv)
{
    return lv->kind == LV_SEARCH && lv->types == SP_SEARCH_ALL;
}

int lib_loading(const Level *lv)
{
    return (lv->want || lv->need) && !lv->refresh;
}

/* --- requests: one at a time ------------------------------------------------ */

/* The list whose request is in flight, if one still wants its page. */
static Level *busy(void)
{
    for (int i = 0; i <= g_depth; i++)
        if (g_lv[i].want)
            return &g_lv[i];
    for (int i = 0; i < LIB_SECTIONS; i++)
        if (g_sec[i].want)
            return &g_sec[i];
    return NULL;
}

/* Send lv's request. */
static void post(Ui *u, Level *lv)
{
    LibReq r;
    int room = BROWSE_KEEP - (lv->refresh ? 0 : lv->items.n);
    int cap = lv->cap < room ? lv->cap : room;
    memset(&r, 0, sizeof r);
    r.seq = ++g_req_seq;
    if (!lv->refresh)
        r.page = lv->page;
    switch (lv->kind) {
    case LV_ROOT:
        r.list = SP_LIST_LIKED; /* its total */
        memset(&r.page, 0, sizeof r.page);
        r.cap = 1;
        break;
    case LV_LIST:
        r.list = lv->list;
        snprintf(r.id, sizeof r.id, "%s", lv->uri);
        r.cap = cap;
        break;
    case LV_SEARCH:
        r.search = 1;
        r.types = lv->types;
        snprintf(r.query, sizeof r.query, "%s", lv->query);
        r.cap = cap;
        break;
    }
    lv->need = 0;
    lv->want = r.seq;
    touch(lv);
    lib_post(&u->libw, &r);
}

/* The next request waiting, unless one is in flight: the screen on top,
 * then the open section, then the root's total. */
static void pump(Ui *u)
{
    if (busy())
        return;
    Level *next = NULL;
    if (g_depth > 0 && g_lv[g_depth].need)
        next = &g_lv[g_depth];
    else if (g_open >= 0 && g_sec[g_open].need)
        next = &g_sec[g_open];
    else if (g_lv[0].need)
        next = &g_lv[0];
    if (next)
        post(u, next);
}

/* Forget lv's request: a page on its way is not taken. */
static void drop(Level *lv)
{
    if (lv->want || lv->need || lv->refresh)
        touch(lv);
    lv->want = 0;
    lv->need = 0;
    lv->refresh = 0;
}

void lib_request(Ui *u, Level *lv)
{
    if (lv->want || lv->need)
        return;
    lv->need = 1;
    touch(lv);
    pump(u);
}

/* Start lv over from its first page, showing that it loads. */
static void lib_reload(Ui *u, Level *lv)
{
    drop(lv);
    browse_free(&lv->items);
    memset(&lv->page, 0, sizeof lv->page);
    lv->loaded = 0;
    lv->err = SP_OK;
    lv->epoch++;
    lib_request(u, lv);
}

void lib_more(Ui *u, Level *lv)
{
    if (lv->page.more && !lv->want && !lv->need && lv->err == SP_OK && lv->items.n < BROWSE_KEEP &&
        !lib_grouped(lv))
        lib_request(u, lv);
}

void lib_again(Ui *u, Level *lv)
{
    if (lv->err == SP_OK || lv->want || lv->need)
        return;
    if (!lv->loaded) {
        lib_reload(u, lv);
        return;
    }
    lv->err = SP_OK; /* the page that failed: lv->page stayed where it was */
    lib_request(u, lv);
}

void lib_refresh(Ui *u, Level *lv)
{
    if (lv->want || lv->need)
        return; /* a page is on its way already */
    if (!lv->loaded) {
        lib_reload(u, lv);
        return;
    }
    lv->refresh = 1;
    lib_request(u, lv);
}

void lib_sync(Ui *u)
{
    LibWorker *w = &u->libw;
    Level *lv = busy();
    int sort = 0;
    if (g_fresh) {
        g_fresh = 0;
        lib_opened(u);
        lv = busy();
    }
    if (lv && w->mu) {
        plat_mutex_lock(w->mu);
        if (w->res_seq == lv->want) {
            int rc = w->res_rc;
            lv->want = 0;
            if (rc == SP_OK && lv->kind != LV_ROOT) {
                /* a refresh's first page replaces the list once it is whole */
                ItemList fresh = { 0 };
                if (browse_append(lv->refresh ? &fresh : &lv->items, w->res, w->res_n, BROWSE_KEEP) < 0) {
                    rc = SP_ERR_OTHER; /* out of memory: keep what there is */
                } else if (lv->refresh) {
                    browse_free(&lv->items);
                    lv->items = fresh;
                    lv->epoch++;
                }
            }
            if (rc == SP_OK) {
                lv->page = w->res_page;
                lv->loaded = 1;
                sort = lib_grouped(lv);
            }
            /* a refresh that failed keeps the list it had, quietly */
            lv->err = rc != SP_OK && lv->refresh && lv->loaded ? SP_OK : rc;
            lv->refresh = 0;
            touch(lv);
        }
        plat_mutex_unlock(w->mu);
        if (sort)
            browse_sort_groups(lv->items.v, lv->items.n);
    }
    pump(u);
}

void lib_retry(Ui *u)
{
    if (g_depth > 0)
        lib_again(u, &g_lv[g_depth]);
    if (g_open >= 0)
        lib_again(u, &g_sec[g_open]);
    lib_again(u, &g_lv[0]);
}

/* --- sections ------------------------------------------------------------------- */

/* lv holds nothing (a section closed, a screen popped): its pages and
 * request are forgotten; its generations keep counting. */
static void clear_level(Level *lv)
{
    uint64_t epoch = lv->epoch + 1, gen = lv->gen;
    browse_free(&lv->items);
    memset(lv, 0, sizeof *lv);
    lv->epoch = epoch;
    lv->gen = gen;
    touch(lv);
}

void lib_open_section(Ui *u, int section)
{
    if (section < 0 || section >= LIB_SECTIONS || section == g_open)
        return;
    if (g_open >= 0)
        clear_level(&g_sec[g_open]);
    g_open = section;
    Level *lv = &g_sec[section];
    lv->kind = LV_LIST;
    lv->list = SECTIONS[section].list;
    lv->id = ++g_ids;
    /* the queue is one page: all of it */
    lv->cap = lv->list == SP_LIST_QUEUE ? SP_PAGE_MAX : LIB_SECTION_PAGE;
    snprintf(lv->title, sizeof lv->title, "%s", SECTIONS[section].name);
    lib_reload(u, lv);
}

void lib_close_section(Ui *u, int section)
{
    if (section < 0 || section != g_open)
        return;
    clear_level(&g_sec[section]);
    g_open = -1;
    pump(u); /* its request may have been the one in flight */
}

/* --- the stack ---------------------------------------------------------------------- */

/* A new screen on top for the caller to fill and fetch; NULL when full. */
static Level *lib_push(LevelKind kind, const char *title, const char *detail)
{
    if (g_depth + 1 >= LIB_DEPTH)
        return NULL;
    drop(&g_lv[g_depth]); /* a page on its way there is of no use now */
    Level *lv = &g_lv[++g_depth];
    clear_level(lv);
    lv->kind = kind;
    lv->id = ++g_ids;
    lv->cap = SP_PAGE_MAX;
    snprintf(lv->title, sizeof lv->title, "%s", title);
    snprintf(lv->detail, sizeof lv->detail, "%s", detail ? detail : "");
    return lv;
}

void lib_pop(Ui *u)
{
    if (g_depth == 0)
        return;
    clear_level(&g_lv[g_depth--]);
    Level *top = &g_lv[g_depth];
    touch(top);
    if (top->kind == LV_ROOT)
        lib_opened(u);
    else if (!top->loaded && !top->want && !top->need)
        lib_reload(u, top);
    pump(u);
}

void lib_home(Ui *u)
{
    if (g_depth == 0)
        return;
    while (g_depth > 0)
        clear_level(&g_lv[g_depth--]);
    touch(&g_lv[0]);
    lib_opened(u);
    pump(u);
}

int lib_searching(void)
{
    return g_depth > 0 && g_lv[1].kind == LV_SEARCH;
}

void lib_reset(void)
{
    for (int i = 0; i < LIB_DEPTH; i++)
        clear_level(&g_lv[i]);
    for (int i = 0; i < LIB_SECTIONS; i++)
        clear_level(&g_sec[i]);
    g_depth = 0;
    g_open = -1;
    g_fresh = 1;
}

/* A screen of list `list` on top, fetching its first page; NULL when the
 * stack is full. */
static Level *open_list(Ui *u, SpList list, const char *title, const char *detail, const char *uri)
{
    Level *lv = lib_push(LV_LIST, title, detail);
    if (!lv)
        return NULL;
    lv->list = list;
    if (list == SP_LIST_ARTIST)
        lv->cap = SP_ARTIST_PAGE_MAX;
    snprintf(lv->uri, sizeof lv->uri, "%s", uri);
    lib_request(u, lv);
    return lv;
}

void lib_open_list(Ui *u, SpList list, const char *title, const char *uri)
{
    open_list(u, list, title, NULL, uri);
}

void lib_open_search(Ui *u, const char *query, unsigned types, const char *title)
{
    char detail[sizeof g_lv[0].detail];
    if (types == SP_SEARCH_ALL)
        snprintf(detail, sizeof detail, "Search results");
    else
        snprintf(detail, sizeof detail, "Results for " LQUOTE "%s" RQUOTE, query);
    Level *lv = lib_push(LV_SEARCH, title, detail);
    if (!lv)
        return;
    lv->types = types;
    /* every kind: one page of each, "See all" has the rest of one */
    lv->cap = types == SP_SEARCH_ALL ? LIB_PAGE : SP_SEARCH_LIMIT;
    snprintf(lv->query, sizeof lv->query, "%s", query);
    lib_request(u, lv);
}

void lib_search(Ui *u, const char *query)
{
    char title[sizeof g_lv[0].title];
    while (g_depth > 0)
        clear_level(&g_lv[g_depth--]);
    snprintf(title, sizeof title, LQUOTE "%s" RQUOTE, query);
    lib_open_search(u, query, SP_SEARCH_ALL, title);
}

void lib_open_item(Ui *u, const Item *it)
{
    static const char *const kinds[] = {
        [ITEM_TRACK] = "Song",     [ITEM_EPISODE] = "Episode",   [ITEM_ALBUM] = "Album",
        [ITEM_ARTIST] = "Artist",  [ITEM_PLAYLIST] = "Playlist", [ITEM_SHOW] = "Podcast",
    };
    SpList list = it->kind == ITEM_ALBUM    ? SP_LIST_ALBUM
                : it->kind == ITEM_ARTIST   ? SP_LIST_ARTIST
                : it->kind == ITEM_PLAYLIST ? SP_LIST_PLAYLIST
                                            : SP_LIST_SHOW;
    /* what it is, and by whom: "Album · Artist", "Playlist · owner" */
    char detail[sizeof g_lv[0].detail];
    if (it->sub[0])
        snprintf(detail, sizeof detail, "%s " MIDDOT " %s", kinds[it->kind], it->sub);
    else
        snprintf(detail, sizeof detail, "%s", kinds[it->kind]);
    open_list(u, list, it->name, detail, it->uri);
}

void lib_opened(Ui *u)
{
    if (g_depth == 0)
        lib_refresh(u, &g_lv[0]);
    if (g_open == LIB_SEC_QUEUE || g_open == LIB_SEC_RECENT)
        lib_refresh(u, &g_sec[g_open]);
}
