/* GUI: setup notice -> sign-in -> Soloist key / download / pairing -> player.
 * The loop sleeps until input or window_wake (Raven event waiting), so an idle
 * window costs no CPU. Background threads call window_wake when they have
 * news. All network work runs on worker threads; the UI thread only copies
 * snapshots and draws. */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gui/gui.h"
#include "gui/window.h"

#include "art.h"
#include "auth.h"
#include "browse.h"
#include "http.h"
#include "model.h"
#include "platform.h"
#include "poller.h"
#include "soloist.h"
#include "spotify.h"

#include "screens/common.h"
#include "screens/player.h"
#include "screens/signin.h"
#include "ui_internal.h"

/* Body and title text in logical pixels for each CFG_FONT_* setting. */
const FontSize font_sizes[CFG_FONT_SIZES] = {
    { 12, 19, "Small" }, { 14, 22, "Default" }, { 16, 25, "Large" }, { 19, 29, "Extra large" },
};

const char *const bar_label[BAR_BUTTONS] = { "Devices", "Library", "Settings", "Log out" };

/* The workers take a void(*)(void*) wake callback; this adapts window_wake. */
static void ui_wake(void *ud)
{
    (void)ud;
    window_wake();
}

static int fetch_devices(CmdWorker *w, HttpClient *h)
{
    static Device devs[SP_MAX_DEVICES]; /* worker-thread only */
    int n = 0;
    int rc = sp_get_devices(h, devs, SP_MAX_DEVICES, &n);
    if (rc != SP_OK)
        return rc;
    plat_mutex_lock(w->mu);
    memcpy(w->devs.devs, devs, sizeof devs);
    w->devs.ndev = n;
    w->devs.loaded = 1;
    w->devs_seq++;
    plat_mutex_unlock(w->mu);
    return rc;
}

/* Play c's URIs (none = resume): on our own device when it is paired and
 * running and something was picked (or nothing else is active), else on
 * the active device, else on any available one (said in played_on). */
static int play(CmdWorker *w, HttpClient *h, const Cmd *c)
{
    plat_mutex_lock(w->mu);
    int local = w->local;
    if (w->me_stale) {
        w->me_stale = 0;
        w->me.id[0] = 0;
    }
    plat_mutex_unlock(w->mu);
    const char *ctx = c->ctx[0] ? c->ctx : NULL, *item = c->uri[0] ? c->uri : NULL;
    int rc = SP_OK;
    if (c->arg) { /* Liked Songs: its context names the account */
        if (!w->me.id[0])
            rc = sp_get_me(h, &w->me);
        if (rc != SP_OK)
            return rc;
        ctx = w->me.liked_uri;
    }
    Device on;
    int where = SP_ON_ACTIVE;
    rc = sp_play_smart(h, ctx, item, local ? w->name : NULL, local && (ctx || item), &on, &where);
    plat_mutex_lock(w->mu);
    snprintf(w->played_on, sizeof w->played_on, "%s",
             rc == SP_OK && where == SP_ON_OTHER ? on.name : "");
    plat_mutex_unlock(w->mu);
    return rc;
}

static int run_cmd(CmdWorker *w, HttpClient *h, const Cmd *c)
{
    int rc;

    switch (c->kind) {
    case CMD_PLAY:
    case CMD_PLAY_URI:
        return play(w, h, c);
    case CMD_PAUSE:   return sp_pause(h);
    case CMD_NEXT:    return sp_next(h);
    case CMD_PREV:    return sp_prev(h);
    case CMD_SEEK:    return sp_seek(h, c->arg);
    case CMD_VOLUME:  return sp_set_volume(h, c->arg);
    case CMD_SHUFFLE: return sp_set_shuffle(h, c->arg);
    case CMD_REPEAT:  return sp_set_repeat(h, (RepeatMode)c->arg);
    case CMD_QUEUE:   return sp_queue_add(h, c->uri);
    case CMD_DEVICES:
        return fetch_devices(w, h);
    case CMD_TRANSFER:
        rc = sp_transfer(h, c->uri, 1);
        if (rc == SP_OK)
            fetch_devices(w, h); /* only refreshes the marker; its error is moot */
        return rc;
    }
    return SP_ERR_OTHER;
}

static void cmd_thread(void *arg)
{
    CmdWorker *w = arg;
    HttpAuth ha = auth_store_http(w->auth);
    HttpClient *http = http_client_new(&ha);
    int recheck = 0;

    for (;;) {
        plat_mutex_lock(w->mu);
        int stop = w->stop, has = w->len > 0;
        Cmd c = w->fifo[w->head];
        if (has) {
            w->head = (w->head + 1) % CMD_FIFO;
            w->len--;
        }
        plat_mutex_unlock(w->mu);
        if (stop)
            break;
        if (!has) {
            /* One extra wake once optimistic edits expire, so the UI resyncs
             * even if no snapshot ever confirms or contradicts them. */
            int got = plat_event_wait(w->ev, recheck ? PENDING_MS : IDLE_WAIT_MS);
            if (!got && recheck) {
                recheck = 0;
                window_wake();
            }
            continue;
        }
        int rc = http ? run_cmd(w, http, &c) : SP_ERR_OTHER;
        poller_kick(w->poller);
        plat_mutex_lock(w->mu);
        w->err = rc;
        w->done = c.kind;
        w->done_at = plat_now_ms();
        w->err_seq++;
        plat_mutex_unlock(w->mu);
        window_wake();
        recheck = 1;
    }
    http_client_free(http);
}

/* Ask the worker to stop without waiting (cmd_stop then joins). */
static void cmd_signal_stop(CmdWorker *w)
{
    if (w->th) {
        plat_mutex_lock(w->mu);
        w->stop = 1;
        plat_mutex_unlock(w->mu);
        plat_event_signal(w->ev);
    }
}

static void cmd_stop(CmdWorker *w)
{
    if (w->th) {
        cmd_signal_stop(w);
        plat_thread_join(w->th);
    }
    plat_event_free(w->ev);
    plat_mutex_free(w->mu);
    memset(w, 0, sizeof *w);
}

static int cmd_start(CmdWorker *w, AuthStore *auth, Poller *poller, const char *name)
{
    memset(w, 0, sizeof *w);
    w->auth = auth;
    w->poller = poller;
    w->name = name;
    w->mu = plat_mutex_new();
    w->ev = plat_event_new();
    if (w->mu && w->ev)
        w->th = plat_thread_start(cmd_thread, w);
    if (!w->th) {
        cmd_stop(w);
        return -1;
    }
    return 0;
}

/* Line c up: it replaces a playback command still waiting at the end, else
 * it waits its turn. A full FIFO (the worker is stuck, e.g. offline) lets a
 * playback command replace a playback command at its end and drops c
 * otherwise, so a waiting queue add is never overwritten. */
static void cmd_send(CmdWorker *w, const Cmd *c)
{
    plat_mutex_lock(w->mu);
    Cmd *last = w->len ? &w->fifo[(w->head + w->len - 1) % CMD_FIFO] : NULL;
    if (last && c->kind != CMD_QUEUE && last->kind != CMD_QUEUE)
        *last = *c;
    else if (w->len < CMD_FIFO)
        w->fifo[(w->head + w->len++) % CMD_FIFO] = *c;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

void cmd_post_uri(CmdWorker *w, CmdKind kind, int arg, const char *uri, const char *ctx)
{
    Cmd c = { kind, arg, "", "" };
    snprintf(c.uri, sizeof c.uri, "%s", uri ? uri : "");
    snprintf(c.ctx, sizeof c.ctx, "%s", ctx ? ctx : "");
    cmd_send(w, &c);
}

void cmd_post(CmdWorker *w, CmdKind kind, int arg)
{
    cmd_post_uri(w, kind, arg, NULL, NULL);
}

static void lib_thread(void *arg)
{
    LibWorker *w = arg;
    HttpAuth ha = auth_store_http(w->auth);
    HttpClient *http = http_client_new(&ha);
    static Item page[LIB_PAGE]; /* worker-thread only; keeps the stack small */

    for (;;) {
        plat_mutex_lock(w->mu);
        int stop = w->stop, save = w->has_save, list = !save && w->has_req;
        int check = !save && !list && w->has_check, on = w->save_on;
        uint64_t seq = w->save_seq;
        LibReq r = w->req;
        char uri[SP_URI_LEN];
        memcpy(uri, save ? w->save : w->check, sizeof uri);
        w->has_save &= !save;
        w->has_req &= !list;
        w->has_check &= !check;
        plat_mutex_unlock(w->mu);
        if (stop)
            break;
        const char *one = uri;
        if (save) {
            int rc = http ? sp_library_set(http, &one, 1, on) : SP_ERR_OTHER;
            plat_mutex_lock(w->mu);
            memcpy(w->saved, uri, sizeof w->saved);
            w->saved_on = on;
            w->saved_rc = rc;
            w->saved_seq = seq;
            plat_mutex_unlock(w->mu);
        } else if (list) {
            int n = 0, rc = SP_ERR_OTHER;
            if (http)
                rc = r.search ? sp_search(http, r.query, r.types, &r.page, page, r.cap, &n)
                              : sp_get_list(http, r.list, r.id, &r.page, page, r.cap, &n);
            plat_mutex_lock(w->mu);
            w->res_seq = r.seq;
            w->res_rc = rc;
            w->res_n = rc == SP_OK ? n : 0;
            w->res_page = r.page;
            memcpy(w->res, page, (size_t)w->res_n * sizeof *page);
            plat_mutex_unlock(w->mu);
        } else if (check) {
            int saved = 0, rc = http ? sp_library_contains(http, &one, 1, &saved) : SP_ERR_OTHER;
            plat_mutex_lock(w->mu);
            memcpy(w->checked, uri, sizeof w->checked);
            w->checked_saved = saved;
            w->checked_rc = rc;
            w->checked_gen++;
            plat_mutex_unlock(w->mu);
        } else {
            plat_event_wait(w->ev, IDLE_WAIT_MS);
            continue;
        }
        window_wake();
    }
    http_client_free(http);
}

/* Ask the worker to stop without waiting (lib_stop then joins). */
static void lib_signal_stop(LibWorker *w)
{
    if (w->th) {
        plat_mutex_lock(w->mu);
        w->stop = 1;
        plat_mutex_unlock(w->mu);
        plat_event_signal(w->ev);
    }
}

static void lib_stop(LibWorker *w)
{
    if (w->th) {
        lib_signal_stop(w);
        plat_thread_join(w->th);
    }
    plat_event_free(w->ev);
    plat_mutex_free(w->mu);
    memset(w, 0, sizeof *w);
}

static int lib_start(LibWorker *w, AuthStore *auth)
{
    memset(w, 0, sizeof *w);
    w->auth = auth;
    w->mu = plat_mutex_new();
    w->ev = plat_event_new();
    if (w->mu && w->ev)
        w->th = plat_thread_start(lib_thread, w);
    if (!w->th) {
        lib_stop(w);
        return -1;
    }
    return 0;
}

static void lib_post(LibWorker *w, const LibReq *r)
{
    plat_mutex_lock(w->mu);
    w->req = *r;
    w->has_req = 1;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

static void lib_post_check(LibWorker *w, const char *uri)
{
    plat_mutex_lock(w->mu);
    snprintf(w->check, sizeof w->check, "%s", uri);
    w->has_check = 1;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

void lib_post_save(LibWorker *w, const char *uri, int on, uint64_t seq)
{
    plat_mutex_lock(w->mu);
    snprintf(w->save, sizeof w->save, "%s", uri);
    w->save_on = on;
    w->save_seq = seq;
    w->has_save = 1;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

/* The pairing poll cadence: quick while the user is likely picking the
 * device, then slow; always slow while minimized. */
static unsigned pair_poll_ms(uint64_t elapsed_ms, int slow)
{
    return !slow && elapsed_ms < PAIR_FAST_FOR_MS ? PAIR_FAST_MS : PAIR_SLOW_MS;
}

static void transfer_to_us(DeviceWorker *d, HttpClient *http)
{
    for (int i = 0; i < DEVICE_WAIT_TRIES; i++) {
        if (plat_event_wait(d->stop, i ? DEVICE_RETRY_MS : DEVICE_FIRST_MS))
            return;
        Device list[SP_MAX_DEVICES];
        int n = 0;
        if (sp_get_devices(http, list, SP_MAX_DEVICES, &n) != SP_OK)
            continue;
        const Device *me = NULL;
        int busy = 0;
        for (int j = 0; j < n; j++) {
            busy |= list[j].is_active;
            if (!me && list[j].id[0] && strcmp(list[j].name, d->name) == 0)
                me = &list[j];
        }
        if (!me)
            continue; /* Soloist needs a moment to register */
        /* Never steal playback from another device the user is using. */
        if (!busy && !plat_event_wait(d->stop, 0) && sp_transfer(http, me->id, 0) == SP_OK)
            poller_kick(d->poller);
        return;
    }
}

static void pair_poll(DeviceWorker *d, HttpClient *http)
{
    uint64_t start = plat_now_ms(), last_other = start;
    while (!plat_event_wait(d->stop, pair_poll_ms(plat_now_ms() - start, d->slow))) {
        Device list[SP_MAX_DEVICES];
        int n = 0;
        uint64_t now = plat_now_ms();
        if (sp_get_devices(http, list, SP_MAX_DEVICES, &n) == SP_OK) {
            const char *other = NULL;
            for (int j = 0; j < n; j++) {
                if (strcmp(list[j].name, d->name) == 0) {
                    d->found = 1;
                    window_wake();
                    return;
                }
                if (!other || list[j].is_active)
                    other = list[j].name; /* the active one names it best */
            }
            if (other) {
                last_other = now;
                plat_mutex_lock(d->mu);
                int changed = strcmp(d->other, other) != 0;
                snprintf(d->other, sizeof d->other, "%s", other);
                plat_mutex_unlock(d->mu);
                if (changed)
                    window_wake();
            }
        }
        if (now - last_other >= PAIR_IDLE_STOP_MS) {
            d->idle_out = 1;
            window_wake();
            return;
        }
    }
}

static void device_thread(void *arg)
{
    DeviceWorker *d = arg;
    HttpAuth ha = auth_store_http(d->auth);
    HttpClient *http = http_client_new(&ha);
    if (http && d->pair)
        pair_poll(d, http);
    else if (http)
        transfer_to_us(d, http);
    http_client_free(http);
}

static void device_stop(DeviceWorker *d)
{
    if (d->th) {
        plat_event_signal(d->stop);
        plat_thread_join(d->th);
    }
    plat_event_free(d->stop);
    plat_mutex_free(d->mu);
    memset(d, 0, sizeof *d);
}

static void device_start(DeviceWorker *d, AuthStore *auth, Poller *poller, const char *name,
                         int pair)
{
    memset(d, 0, sizeof *d);
    d->auth = auth;
    d->poller = poller;
    d->name = name;
    d->pair = pair;
    d->stop = plat_event_new();
    d->mu = plat_mutex_new();
    if (d->stop && d->mu)
        d->th = plat_thread_start(device_thread, d);
    if (!d->th)
        device_stop(d); /* not fatal: the user can still pick the device in Spotify */
}

static void install_progress(void *ud, int percent)
{
    Installer *in = ud;
    plat_mutex_lock(in->mu);
    in->percent = percent;
    plat_mutex_unlock(in->mu);
    window_wake();
}

static void install_thread(void *arg)
{
    Installer *in = arg;
    char err[ERR_LEN] = "";
    int rc = 0;
    if (in->force || !soloist_up_to_date(in->cfg)) {
        plat_mutex_lock(in->mu);
        in->downloading = 1;
        in->percent = -1;
        plat_mutex_unlock(in->mu);
        window_wake();
        rc = soloist_install(in->cfg, install_progress, in, &in->cancel, err, sizeof err);
    }
    plat_mutex_lock(in->mu);
    in->done = 1;
    in->rc = rc;
    memcpy(in->err, err, sizeof in->err);
    plat_mutex_unlock(in->mu);
    window_wake();
}

static void installer_stop(Installer *in)
{
    if (in->th) {
        in->cancel = 1; /* aborts a download within moments */
        plat_thread_join(in->th);
    }
    plat_mutex_free(in->mu);
    memset(in, 0, sizeof *in);
}

static int installer_start(Installer *in, const Config *cfg, int force)
{
    memset(in, 0, sizeof *in);
    in->cfg = cfg;
    in->force = force;
    in->mu = plat_mutex_new();
    if (in->mu)
        in->th = plat_thread_start(install_thread, in);
    if (!in->th) {
        installer_stop(in);
        return -1;
    }
    return 0;
}

/* --- font and text ------------------------------------------------------- */
static Metrics metrics_for(int size)
{
    if (size < 0 || size >= CFG_FONT_SIZES)
        size = CFG_FONT_DEFAULT;
    Metrics m = { 0 };
    m.size = size;
    m.fs = font_sizes[size].body;
    m.fl = font_sizes[size].title;
    m.line = (float)m.fs + 6;
    m.row = (float)m.fs + 18;
    m.row2 = 2.0f * (float)m.fs + LINE_GAP + 16;
    m.bar = (float)m.fs + 10;
    return m;
}

/* The text sizes and the geometry that follows from them, for the size
 * setting: at start and whenever it changes. Also the narrowest window the
 * bottom bar needs (Metrics.min_w). */
static void apply_metrics(Ui *u)
{
    Metrics m = metrics_for(u->cfg.font_size);
    if (u->m.min_w > 0.0f && u->m.size == m.size)
        return;
    u->m = m;
    float labels = 0.0f;
    for (int i = 0; i < BAR_BUTTONS; i++)
        labels += gui_text_width(px_of(u, FS_S), bar_label[i]);
    u->m.min_w = labels + 2 * BAR_BUTTONS * BAR_MARGIN + (BAR_BUTTONS - 1) * BAR_GAP + 2 * PAD;
    u->text_dirty = 1;
}

/* --- Library panel: its stack of screens ---------------------------------- */
/* The list a screen shows, as browse_play_target takes it: -1 for search. */
int lib_list_of(const Level *lv)
{
    return lv->kind == LV_LIST ? (int)lv->list : -1;
}

/* Ask the library worker for lv's next page (the root's: the Liked Songs
 * total). One request per screen at a time; its seq matches the answer. */
void lib_request(Ui *u, Level *lv)
{
    LibReq r;
    int room = BROWSE_KEEP - lv->items.n;
    memset(&r, 0, sizeof r);
    r.seq = ++u->lib_req_seq;
    r.page = lv->page;
    switch (lv->kind) {
    case LV_ROOT:
        r.list = SP_LIST_LIKED;
        memset(&r.page, 0, sizeof r.page);
        r.cap = 1;
        break;
    case LV_LIST:
        r.list = lv->list;
        snprintf(r.id, sizeof r.id, "%s", lv->uri);
        r.cap = room < SP_PAGE_MAX ? room : SP_PAGE_MAX;
        break;
    case LV_SEARCH:
        r.search = 1;
        r.types = lv->types;
        snprintf(r.query, sizeof r.query, "%s", lv->query);
        /* Every kind: one page, each kind's "See all" has the rest. */
        r.cap = lv->types == SP_SEARCH_ALL ? LIB_PAGE : room < SP_SEARCH_LIMIT ? room : SP_SEARCH_LIMIT;
        break;
    }
    lv->want = r.seq;
    lib_post(&u->libw, &r);
}

/* Start lv over from its first page. */
static void lib_reload(Ui *u, Level *lv)
{
    browse_free(&lv->items);
    memset(&lv->page, 0, sizeof lv->page);
    lv->loaded = lv->scroll = 0;
    lv->err = SP_OK;
    lib_request(u, lv);
}

/* A new screen on top for the caller to fill and fetch; NULL when full. */
static Level *lib_push(Ui *u, LevelKind kind, const char *title)
{
    if (u->depth + 1 >= LIB_DEPTH)
        return NULL;
    u->lv[u->depth].want = 0; /* a page on its way there is of no use now */
    Level *lv = &u->lv[++u->depth];
    memset(lv, 0, sizeof *lv);
    lv->kind = kind;
    snprintf(lv->title, sizeof lv->title, "%s", title);
    u->wheel = 0.0f;
    u->redraw = 1;
    return lv;
}

/* Back one screen: the root fetches its Liked Songs total again, a list
 * that never loaded tries again. */
void lib_pop(Ui *u)
{
    if (u->depth == 0)
        return;
    Level *lv = &u->lv[u->depth--];
    browse_free(&lv->items);
    memset(lv, 0, sizeof *lv);
    Level *top = &u->lv[u->depth];
    if (top->kind == LV_ROOT || !top->loaded)
        lib_request(u, top);
    u->wheel = 0.0f;
    u->redraw = 1;
}

static void lib_reset(Ui *u)
{
    for (int i = 0; i < LIB_DEPTH; i++)
        browse_free(&u->lv[i].items);
    memset(u->lv, 0, sizeof u->lv);
    u->depth = 0;
    u->wheel = 0.0f;
    u->query[0] = 0;
}

void lib_open_list(Ui *u, SpList list, const char *title, const char *uri)
{
    Level *lv = lib_push(u, LV_LIST, title);
    if (!lv)
        return;
    lv->list = list;
    snprintf(lv->uri, sizeof lv->uri, "%s", uri);
    lib_request(u, lv);
}

/* Search results of every kind (SP_SEARCH_ALL, grouped), or of one. */
void lib_open_search(Ui *u, const char *query, unsigned types, const char *title)
{
    Level *lv = lib_push(u, LV_SEARCH, title);
    if (!lv)
        return;
    lv->types = types;
    snprintf(lv->query, sizeof lv->query, "%s", query);
    lib_request(u, lv);
}

/* What an album, artist, playlist or show row holds, on a screen of its own. */
void lib_open_item(Ui *u, const Item *it)
{
    SpList list = it->kind == ITEM_ALBUM    ? SP_LIST_ALBUM
                : it->kind == ITEM_ARTIST   ? SP_LIST_ARTIST
                : it->kind == ITEM_PLAYLIST ? SP_LIST_PLAYLIST
                                            : SP_LIST_SHOW;
    lib_open_list(u, list, it->name, it->uri);
}

/* The panel opened: the Liked Songs total, the queue and what played last
 * may have changed since. */
void lib_opened(Ui *u)
{
    Level *lv = &u->lv[u->depth];
    if (lv->kind == LV_ROOT)
        lib_request(u, lv);
    else if (lv->kind == LV_LIST && (lv->list == SP_LIST_QUEUE || lv->list == SP_LIST_RECENT))
        lib_reload(u, lv);
}

/* Take the library worker's page if it answers the screen shown; one for a
 * screen left since is dropped. */
void lib_sync(Ui *u)
{
    Level *lv = &u->lv[u->depth];
    LibWorker *w = &u->libw;
    int sort = 0;
    if (!lv->want)
        return;
    plat_mutex_lock(w->mu);
    if (w->res_seq == lv->want) {
        int rc = w->res_rc;
        lv->want = 0;
        if (rc == SP_OK && lv->kind != LV_ROOT &&
            browse_append(&lv->items, w->res, w->res_n, BROWSE_KEEP) < 0)
            rc = SP_ERR_OTHER; /* out of memory: keep what there is */
        if (rc == SP_OK) {
            lv->page = w->res_page;
            lv->loaded = 1;
            sort = lv->kind == LV_SEARCH && lv->types == SP_SEARCH_ALL;
        }
        lv->err = rc;
    }
    plat_mutex_unlock(w->mu);
    if (sort)
        browse_sort_groups(lv->items.v, lv->items.n);
}

/* --- screen transitions -------------------------------------------------- */
static void reset_player_state(Ui *u)
{
    memset(&u->st, 0, sizeof u->st);
    u->st.volume = -1;
    memset(&u->pend, 0, sizeof u->pend);
    u->seq = 0;
    u->cmd_seq = 0;
    u->cmd_err = u->poll_err = 0;
    u->background = 0;
    u->msg[0] = u->msg_shown[0] = u->hint[0] = 0;
    u->played_on[0] = 0;
    u->text_dirty = 1;
    u->art_seq = 0;
    u->panel = PANEL_NONE;
    u->devs_seq = u->dev_seq_noted = 0;
    memset(&u->devs, 0, sizeof u->devs);
    lib_reset(u);
    u->heart_uri[0] = 0;
    u->heart = -1;
    u->heart_gen = u->like_sent = u->like_done = 0;
    u->like_err = SP_OK;
    u->note_until = 0;
}

void set_screen(Ui *u, Screen s)
{
    u->screen = s;
    gui_active = W_NONE;
    u->text_dirty = 1;
    u->redraw = 1;
}

/* Soloist and its helpers; safe to call in any state. */
static void stop_engine(Ui *u)
{
    u->inst.cancel = 1;
    if (u->dev.stop)
        plat_event_signal(u->dev.stop);
    installer_stop(&u->inst);
    device_stop(&u->dev);
    soloist_stop(u->sol); /* never leave an orphaned Soloist process */
    u->sol = NULL;
}

/* Stops everything that talks to Spotify; safe to call in any state.
 * Every worker is told first and only then joined, so the wait is the
 * slowest one's, not their sum. */
void stop_session(Ui *u)
{
    u->inst.cancel = 1;
    if (u->dev.stop)
        plat_event_signal(u->dev.stop);
    cmd_signal_stop(&u->cmd);
    lib_signal_stop(&u->libw);
    art_signal_stop(u->art);
    poller_signal_stop(u->poller);
    stop_engine(u);
    cmd_stop(&u->cmd);
    lib_stop(&u->libw);
    art_stop(u->art);
    u->art = NULL;
    if (Rtexture_is_valid(u->art_tex))
        Rtexture_destroy(u->art_tex);
    u->art_tex = R_INVALID_ID;
    poller_stop(u->poller);
    u->poller = NULL;
}

/* Check (and if needed download, silently) Soloist on the install worker;
 * the player starts it once that is done (poll_engine). No-op if it is
 * already on its way, or can't run here. */
void start_engine(Ui *u, int force)
{
    if (u->inst.th || u->sol || !soloist_supported() || !u->cfg.soloist_key[0])
        return;
    u->eng_err[0] = 0;
    u->text_dirty = 1;
    if (installer_start(&u->inst, &u->cfg, force) != 0)
        snprintf(u->eng_err, sizeof u->eng_err, "Could not start the Soloist setup.");
}

/* Signed in: the player. Soloist starts right away once paired; unpaired,
 * only when another device of the account shows up (note_other). */
static void enter_player(Ui *u)
{
    stop_engine(u);
    reset_player_state(u);
    u->poller = poller_start(u->auth, ui_wake, NULL);
    if (!u->poller || cmd_start(&u->cmd, u->auth, u->poller, u->cfg.device_name) != 0 ||
        lib_start(&u->libw, u->auth) != 0) {
        stop_session(u);
        u->login_msg = "Could not start the player.";
        set_screen(u, SCR_LOGIN);
        return;
    }
    u->art = art_start(ui_wake, NULL); /* not fatal: the slot stays empty */
    u->paired = config_paired(&u->cfg);
    u->auto_failed = u->hint_hidden = 0;
    u->other_dev[0] = 0;
    u->eng_err[0] = 0;
    /* A device id without the marker: maybe paired with an older build;
     * a restored session then shows as READY (poll_engine). */
    if (u->paired || config_soloist_used(&u->cfg))
        start_engine(u, 0);
    set_screen(u, SCR_PLAYER);
}

/* Sign-in first; everything else happens in the player. An older sign-in
 * still works for what it was granted, so the sign-in screen asks for the
 * new permissions once per run, over the running player. */
void advance_signin(Ui *u)
{
    u->reauth = 0;
    if (!auth_store_logged_in(u->auth)) {
        set_screen(u, SCR_LOGIN);
        return;
    }
    enter_player(u);
    if (u->screen == SCR_PLAYER && !u->consent_asked && auth_store_needs_reconsent(u->auth)) {
        u->consent_asked = u->reauth = 1;
        u->login_msg = NULL;
        set_screen(u, SCR_LOGIN);
    }
}

/* Client id known: open the token store and continue with sign-in. */
static int enter_app(Ui *u)
{
    u->auth = auth_store_new(&u->cfg);
    if (!u->auth)
        return -1;
    advance_signin(u);
    return 0;
}

/* Letters, digits, '-' and '_': what seed_env.cmake accepts as a client ID. */
static int client_id_ok(const char *s)
{
    if (!s[0])
        return 0;
    for (; *s; s++) {
        int ok = (*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') ||
                 (*s >= 'a' && *s <= 'z') || *s == '_' || *s == '-';
        if (!ok)
            return 0;
    }
    return 1;
}

/* edit_line drops control characters, so only the blanks of a paste are
 * left to trim. */
static void trim_blanks(char *s)
{
    size_t n = strlen(s), a = 0;
    while (a < n && s[a] == ' ')
        a++;
    while (n > a && s[n - 1] == ' ')
        n--;
    memmove(s, s + a, n - a);
    s[n - a] = 0;
}

/* Read .env again and go on once it has a client id; missing says why not. */
static void setup_reload(Ui *u, const char *missing)
{
    u->setup_msg = NULL;
    Config c;
    if (config_load(&c) == 0)
        u->cfg = c; /* no AuthStore exists yet, so nothing holds the old one */
    u->text_dirty = 1;
    if (!u->cfg.client_id[0])
        u->setup_msg = missing;
    else if (enter_app(u) != 0)
        u->setup_msg = "Out of memory.";
}

/* Save the pasted client id into <app>/.env, then read .env again. The
 * text stays in the box when the save fails, so it can be fixed. */
void setup_save(Ui *u)
{
    u->setup_msg = NULL;
    trim_blanks(u->id_input);
    if (!u->id_input[0]) {
        setup_reload(u, "Still no SPOTIFY_CLIENT_ID in .env.");
        return;
    }
    if (!client_id_ok(u->id_input)) {
        u->setup_msg = "A Client ID has only letters, digits, - and _.";
        return;
    }
    if (config_set_env_value(&u->cfg, "SPOTIFY_CLIENT_ID", u->id_input) != 0) {
        u->setup_msg = "Could not save the Client ID to .env. Is the app folder writable?";
        return;
    }
    memset(u->id_input, 0, sizeof u->id_input);
    setup_reload(u, "Could not read the Client ID back from .env.");
}

/* --- login screen -------------------------------------------------------- */
/* Called on the login thread just before auth opens the browser. */
static void login_url_cb(const char *url, void *ud)
{
    Ui *u = ud;
    plat_mutex_lock(u->mu);
    snprintf(u->login_url, sizeof u->login_url, "%s", url);
    plat_mutex_unlock(u->mu);
    window_wake();
}

static void login_thread(void *arg)
{
    Ui *u = arg;
    int rc = auth_store_login(u->auth, &u->login_cancel);
    plat_mutex_lock(u->mu);
    u->login_done = 1;
    u->login_rc = rc;
    plat_mutex_unlock(u->mu);
    window_wake();
}

/* Fallback when the browser tab got lost: open the pending URL again. */
void open_again(Ui *u)
{
    char url[SIGNIN_URL_LEN];
    plat_mutex_lock(u->mu);
    memcpy(url, u->login_url, sizeof url);
    plat_mutex_unlock(u->mu);
    if (url[0] && plat_open_url(url) != 0)
        u->login_msg = "Could not open the browser.";
}

void login_join(Ui *u)
{
    if (!u->login_th)
        return;
    u->login_cancel = 1;
    plat_thread_join(u->login_th);
    u->login_th = NULL;
}

void start_login(Ui *u)
{
    plat_mutex_lock(u->mu);
    u->login_done = 0;
    u->login_url[0] = 0;
    plat_mutex_unlock(u->mu);
    auth_store_set_url_callback(u->auth, login_url_cb, u);
    u->login_cancel = 0;
    u->login_msg = NULL;
    u->login_th = plat_thread_start(login_thread, u);
    if (!u->login_th)
        u->login_msg = "Could not start sign-in.";
}

/* Sign in again over the running player, for permissions newer features
 * need; now: open the browser at once (the user asked for it). */
void start_reauth(Ui *u, int now)
{
    u->reauth = 1;
    u->login_msg = NULL;
    set_screen(u, SCR_LOGIN);
    if (now)
        start_login(u);
}

/* Back to the player from the sign-in over it; after a new sign-in, what
 * needed its permissions tries again. */
void end_reauth(Ui *u, int signed_in)
{
    u->reauth = 0;
    u->login_msg = NULL;
    set_screen(u, SCR_PLAYER);
    if (!signed_in)
        return;
    plat_mutex_lock(u->cmd.mu);
    u->cmd.me_stale = 1; /* it may be another account */
    plat_mutex_unlock(u->cmd.mu);
    Level *lv = &u->lv[u->depth];
    if (lv->err != SP_OK)
        lib_reload(u, lv);
    u->heart_uri[0] = 0; /* the heart checks the playing track again */
}

/* Back to the player, which kept running behind this screen. */
void leave_key(Ui *u)
{
    memset(u->key_input, 0, sizeof u->key_input);
    u->setup_msg = NULL;
    set_screen(u, SCR_PLAYER);
}

/* '*' for all but the last 4 characters: enough to recognise the key. */
void mask_secret(const char *s, char *out, size_t cap)
{
    size_t n = strlen(s), keep = n > 4 ? 4 : 0, i = 0;
    for (; i + keep < n && i + 1 < cap; i++)
        out[i] = '*';
    snprintf(out + i, cap - i, "%s", s + n - keep);
}

/* Read SOLOIST_API_KEY from .env (or the environment) again. Only the key
 * changes; the AuthStore keeps its own copy of the rest. */
void reload_key(Ui *u)
{
    Config c;
    if (config_load(&c) == 0) {
        memcpy(u->cfg.soloist_key, c.soloist_key, sizeof u->cfg.soloist_key);
        memset(c.soloist_key, 0, sizeof c.soloist_key);
    }
}

/* Save the pasted key into <app>/.env (if any), then read .env again. */
void save_key(Ui *u)
{
    int typed = u->key_input[0] != 0;
    int rc = typed ? config_set_env_value(&u->cfg, "SOLOIST_API_KEY", u->key_input) : 0;
    memset(u->key_input, 0, sizeof u->key_input);
    if (rc != 0) {
        u->setup_msg = "Could not save the key to .env.";
        return;
    }
    reload_key(u);
    if (!u->cfg.soloist_key[0]) {
        u->setup_msg = typed ? "Could not read the key back from .env."
                             : "Still no SOLOIST_API_KEY in .env.";
        return;
    }
    leave_key(u);
    stop_engine(u); /* a running Soloist still has the old key */
    u->auto_failed = 0;
    u->reinstalled = 0;
    start_engine(u, 0);
}

/* --- Soloist in the player: install, start, pair, stop ------------------ */
/* Install worker finished: start Soloist on this (the main) thread. */
static void finish_install(Ui *u)
{
    plat_mutex_lock(u->inst.mu);
    int rc = u->inst.rc;
    memcpy(u->eng_err, u->inst.err, sizeof u->eng_err);
    plat_mutex_unlock(u->inst.mu);
    installer_stop(&u->inst);
    u->text_dirty = 1;
    if (rc == 0) {
        u->eng_err[0] = 0;
        u->sol = soloist_start(&u->cfg, ui_wake, NULL);
        if (!u->sol)
            snprintf(u->eng_err, sizeof u->eng_err, "Could not start Spotify Soloist.");
    }
    if (!u->sol) {
        u->auto_failed = !u->paired;
        return;
    }
    u->sol_was_paired = u->paired;
    u->transferred = 0;
    if (!u->paired) /* watch for the user picking us in a Spotify app */
        device_start(&u->dev, u->auth, NULL, u->cfg.device_name, 1);
}

/* Another device of the account is online: when unpaired, start Soloist
 * (installing it first if needed) so that device can pick this one. */
void note_other(Ui *u, const char *name)
{
    if (!name[0] || strcmp(name, u->cfg.device_name) == 0)
        return;
    if (strcmp(u->other_dev, name) != 0) {
        snprintf(u->other_dev, sizeof u->other_dev, "%s", name);
        u->text_dirty = 1;
    }
    if (!u->paired && !u->auto_failed)
        start_engine(u, 0);
}

/* Follow the install worker and Soloist's state; runs every player frame
 * (frames only happen on events, so this adds no wakeups). */
void poll_engine(Ui *u)
{
    if (u->inst.th) {
        plat_mutex_lock(u->inst.mu);
        int done = u->inst.done;
        plat_mutex_unlock(u->inst.mu);
        if (done)
            finish_install(u);
    }
    if (!u->sol)
        return;
    SoloistState st = soloist_state(u->sol);
    if (st == SOLOIST_EXPIRED && !u->reinstalled) {
        /* Expired mid-session: replace the build and start once more. */
        u->reinstalled = 1;
        stop_engine(u);
        start_engine(u, 1);
        return;
    }
    if (st == SOLOIST_STOPPED || st == SOLOIST_EXPIRED) {
        soloist_status(u->sol, u->eng_err, sizeof u->eng_err);
        u->auto_failed = !u->paired; /* a paired one gets the Restart button */
        stop_engine(u);
        u->text_dirty = 1;
        return;
    }
    if (u->paired && u->sol_was_paired && st == SOLOIST_PAIRING) {
        /* Its stored session is gone: pair again like the first time. */
        u->paired = u->sol_was_paired = 0;
        (void)config_set_paired(&u->cfg, 0);
        device_stop(&u->dev);
        device_start(&u->dev, u->auth, NULL, u->cfg.device_name, 1);
        u->text_dirty = 1;
    }
    if (!u->paired && st == SOLOIST_READY) {
        /* "running" without "waiting for login": a stored session came
         * back, so it is paired (from before the marker existed). */
        device_stop(&u->dev);
        u->paired = u->sol_was_paired = 1;
        (void)config_set_paired(&u->cfg, 1);
        u->text_dirty = 1;
    }
    if (!u->paired && u->dev.th) {
        if (u->dev.found) {
            /* Picked in a Spotify app: ours from now on, at every launch. */
            device_stop(&u->dev);
            u->paired = 1;
            (void)config_set_paired(&u->cfg, 1);
            u->text_dirty = 1;
        } else if (u->dev.idle_out) {
            stop_engine(u); /* nobody can pick us: save the battery */
            u->text_dirty = 1;
        } else {
            char other[SP_NAME_LEN];
            plat_mutex_lock(u->dev.mu);
            memcpy(other, u->dev.other, sizeof other);
            plat_mutex_unlock(u->dev.mu);
            if (other[0] && strcmp(other, u->other_dev) != 0) {
                memcpy(u->other_dev, other, sizeof u->other_dev);
                u->text_dirty = 1;
            }
        }
    } else if (u->paired && st == SOLOIST_READY && !u->transferred) {
        u->transferred = 1;
        device_stop(&u->dev);
        device_start(&u->dev, u->auth, u->poller, u->cfg.device_name, 0);
    }
}

/* Our device is paired and running: the library plays here. */
int local_ready(Ui *u)
{
    SoloistState st = soloist_state(u->sol);
    return u->sol && u->paired && (st == SOLOIST_READY || st == SOLOIST_PAIRING);
}

/* --- player screen ------------------------------------------------------- */
int progress_at(const PlaybackState *s, uint64_t now)
{
    int64_t p = s->progress_ms;
    if (s->is_playing && now > s->fetched_at_ms)
        p += (int64_t)(now - s->fetched_at_ms);
    if (s->track.duration_ms > 0 && p > s->track.duration_ms)
        p = s->track.duration_ms;
    return p < 0 ? 0 : (int)p;
}

void pend_reapply(Ui *u)
{
    Pending *p = &u->pend;
    if (p->mask & (PEND_PLAY | PEND_SEEK)) {
        u->st.progress_ms = p->progress_ms;
        u->st.fetched_at_ms = p->progress_at;
    }
    if (p->mask & PEND_PLAY)
        u->st.is_playing = p->playing;
    if (p->mask & PEND_SHUFFLE)
        u->st.shuffle = p->shuffle;
    if (p->mask & PEND_REPEAT)
        u->st.repeat = p->repeat;
    if (p->mask & PEND_VOLUME)
        u->st.volume = p->volume;
}

/* New snapshot: fields it confirms stop being pending; the rest of the
 * optimistic edits stay on top until PENDING_MS runs out. */
void apply_snapshot(Ui *u, const PlaybackState *s, uint64_t now)
{
    Pending *p = &u->pend;
    if ((p->mask & PEND_PLAY) && s->is_playing == p->playing)
        p->mask &= ~PEND_PLAY;
    if (p->mask & PEND_SEEK) {
        PlaybackState want = *s;
        want.progress_ms = p->progress_ms;
        want.fetched_at_ms = p->progress_at;
        int d = progress_at(s, now) - progress_at(&want, now);
        if (d > -2000 && d < 2000)
            p->mask &= ~PEND_SEEK;
    }
    if ((p->mask & PEND_SHUFFLE) && s->shuffle == p->shuffle)
        p->mask &= ~PEND_SHUFFLE;
    if ((p->mask & PEND_REPEAT) && s->repeat == p->repeat)
        p->mask &= ~PEND_REPEAT;
    if ((p->mask & PEND_VOLUME) && s->volume == p->volume)
        p->mask &= ~PEND_VOLUME;
    /* Only text-bearing fields re-fit text; playing tracks snapshot every
     * second for the progress bar alone. */
    const PlaybackState *o = &u->st;
    if (s->active != o->active || strcmp(s->track.uri, o->track.uri) ||
        strcmp(s->track.name, o->track.name) || strcmp(s->track.artists, o->track.artists) ||
        strcmp(s->track.album, o->track.album) || strcmp(s->device_name, o->device_name))
        u->text_dirty = 1;
    u->st = *s;
    pend_reapply(u);
}

void pend_begin(Ui *u, int bit, uint64_t now)
{
    u->pend.mask |= bit;
    u->pend.until = now + PENDING_MS;
}

/* Truth wins again: drop optimistic edits and take the poller's state. */
void resync(Ui *u)
{
    u->pend.mask = 0;
    poller_snapshot(u->poller, &u->st, NULL);
    u->text_dirty = 1;
}

/* --- the heart: is the playing track in your library? -------------------- */
/* A track or episode the heart can save: anything playing but a local file. */
int likeable(const PlaybackState *s)
{
    return s->active && s->track.uri[0] && strncmp(s->track.uri, "spotify:local:", 14) != 0;
}

/* One check per new track; a click shows at once, and the library worker's
 * answer confirms it or puts it back. Returns 1 when it left the player: a
 * save the sign-in lacks the permission for asks for it. */
int heart_sync(Ui *u)
{
    LibWorker *w = &u->libw;
    const char *uri = likeable(&u->st) ? u->st.track.uri : "";
    int scope = 0;
    if (strcmp(uri, u->heart_uri) != 0) {
        snprintf(u->heart_uri, sizeof u->heart_uri, "%s", uri);
        u->heart = -1;
        u->like_err = SP_OK;
        if (uri[0])
            lib_post_check(w, uri);
    }
    plat_mutex_lock(w->mu);
    /* Checks first: one answered while a save was on its way is old news. */
    if (w->checked_gen != u->heart_gen) {
        u->heart_gen = w->checked_gen;
        if (u->like_done == u->like_sent && !strcmp(w->checked, u->heart_uri))
            u->heart = w->checked_rc == SP_OK ? w->checked_saved : -1;
    }
    if (w->saved_seq != u->like_done) {
        u->like_done = w->saved_seq;
        /* Only the newest save puts the heart back; a later one is coming. */
        if (w->saved_rc != SP_OK && u->like_done == u->like_sent && !strcmp(w->saved, u->heart_uri))
            u->heart = !w->saved_on;
        scope = w->saved_rc == SP_ERR_SCOPE;
        u->like_err = scope ? SP_OK : w->saved_rc;
    }
    plat_mutex_unlock(w->mu);
    if (scope)
        start_reauth(u, 0);
    return scope;
}

void refresh_player_text(Ui *u, float maxw, float status_w, float hint_w)
{
    const PlaybackState *s = &u->st;
    if (s->active) {
        float tw = likeable(s) ? maxw - heart_side(&u->m) - ROW_PAD : maxw; /* the heart */
        gui_fit_text(px_of(u, FS_L), s->track.name[0] ? s->track.name : "Unknown title", tw,
                     u->title, sizeof u->title);
        gui_fit_text(px_of(u, FS_S), s->track.artists, maxw, u->artists, sizeof u->artists);
        gui_fit_text(px_of(u, FS_S), s->track.album, maxw, u->album, sizeof u->album);
    } else {
        gui_fit_text(px_of(u, FS_L), "Nothing playing", maxw, u->title, sizeof u->title);
        gui_fit_text(px_of(u, FS_S), "Press play, or pick a device with Devices.", maxw,
                     u->artists, sizeof u->artists);
        u->album[0] = 0;
    }
    sp_fmt_time(u->t_dur, sizeof u->t_dur, s->track.duration_ms);
    u->pos_sec = -1;

    gui_wrap_text(px_of(u, FS_S), u->msg_shown, status_w, 2, u->status, sizeof u->status);
    gui_wrap_text(px_of(u, FS_S), u->hint, hint_w, 2, u->hint_fit, sizeof u->hint_fit);
    u->text_dirty = 0;
}

/* The single most relevant status line; returns 1 if it is an error. */
int status_message(Ui *u, char *out, size_t cap)
{
    const PlaybackState *s = &u->st;
    if (u->cmd_err == SP_ERR_NODEVICE) {
        /* Nothing at all could play: say how to get a device, kindly. */
        if (u->sol && !u->paired)
            snprintf(out, cap, "To play on this computer: open Spotify on your phone or computer "
                               "and choose '%s' once.", u->cfg.device_name);
        else
            snprintf(out, cap, "Open Spotify on any device to start.");
        return 0;
    }
    int err = u->cmd_err ? u->cmd_err : u->like_err ? u->like_err : u->poll_err;
    if (err) {
        snprintf(out, cap, "%s", sp_strerror(err));
        return 1;
    }
    if (plat_now_ms() < u->note_until) {
        snprintf(out, cap, "Added to queue");
        return 0;
    }
    if (u->paired && u->eng_err[0] && !u->sol && !u->inst.th) {
        snprintf(out, cap, "%s%s", strncmp(u->eng_err, "Soloist", 7) ? "Soloist: " : "", u->eng_err);
        return 1;
    }
    if (u->inst.th) {
        plat_mutex_lock(u->inst.mu);
        int downloading = u->inst.downloading, pct = u->inst.percent;
        plat_mutex_unlock(u->inst.mu);
        if (downloading && pct >= 0)
            snprintf(out, cap, "Getting Spotify Soloist... %d%%", pct);
        else if (downloading)
            snprintf(out, cap, "Getting Spotify Soloist...");
        else if (u->paired)
            snprintf(out, cap, "Starting this computer's player...");
        else
            out[0] = 0;
        if (out[0])
            return 0;
    }
    if (u->played_on[0] && !(s->active && s->device_name[0])) {
        snprintf(out, cap, "Playing on %s", u->played_on);
        return 0;
    }
    if (u->paired && u->sol && soloist_state(u->sol) == SOLOIST_STARTING) {
        snprintf(out, cap, "Starting this computer's player...");
        return 0;
    }
    if (s->active && s->device_name[0])
        snprintf(out, cap, "%s on %s", s->is_playing ? "Playing" : "Paused",
                 strcmp(s->device_name, u->cfg.device_name) == 0 ? "this computer" : s->device_name);
    else
        snprintf(out, cap, "No active device");
    return 0;
}

/* Pull a new device list from the command worker; copies only on change. */
void sync_devices(Ui *u)
{
    plat_mutex_lock(u->cmd.mu);
    if (u->cmd.devs_seq != u->devs_seq) {
        u->devs_seq = u->cmd.devs_seq;
        u->devs = u->cmd.devs;
    }
    plat_mutex_unlock(u->cmd.mu);
}

/* --- main loop: window_run calls frame once per frame -------------------- */
/* At least as wide as the bottom bar needs (Metrics.min_w) and as tall as
 * the screen drawn last needs (Ui.need_h): grow the window if it is smaller.
 * A height measured in a window still too narrow (text wraps more there)
 * waits for the next frame, which the widening brings. */
static void fit_window(Ui *u)
{
    int mw = (int)ceilf(fmaxf(MIN_W, u->m.min_w));
    int mh = u->w < mw ? (u->min_h ? u->min_h : MIN_H) : (int)ceilf(fmaxf(MIN_H, u->need_h));
    if (mw == u->min_w && mh == u->min_h)
        return;
    u->min_w = mw;
    u->min_h = mh;
    window_set_min_size(mw, mh);
    if (u->w < mw || u->h < mh)
        window_set_size(u->w > mw ? u->w : mw, u->h > mh ? u->h : mh);
}

/* Set when enter_app fails: frame() then draws nothing before the loop ends. */
static int startup_failed;

/* The window's draw callback: one frame of the active screen. window.c
 * brackets the frame with gui_begin_paint and gui_end_paint. */
static void frame(void *user, float width, float height)
{
    Ui *u = user;
    if (startup_failed)
        return;
    apply_metrics(u); /* remade only when the size setting changed */
    int w = (int)width, h = (int)height;
    if (w != u->w || h != u->h) {
        u->w = w;
        u->h = h;
        u->text_dirty = 1;
    }
    switch (u->screen) {
    case SCR_MOVE:   draw_move(u); break;
    case SCR_SETUP:  draw_setup(u); break;
    case SCR_LOGIN:  draw_login(u); break;
    case SCR_KEY:    draw_key(u); break;
    case SCR_PLAYER: draw_player(u); break;
    }
    fit_window(u);
    /* Posted after the draw so the next frame shows the new state. */
    if (u->redraw) {
        u->redraw = 0;
        window_wake();
    }
}

/* Runs once when the window closes, or when window_quit ends the loop: every
 * thread that could call window_wake is joined before the window goes. */
static void close_cb(void *ud)
{
    Ui *u = ud;
    stop_session(u);
    login_join(u);
    lib_reset(u);
    auth_store_free(u->auth);
    (void)plat_quit_watch(NULL, NULL); /* no wake-ups once the window is gone */
    plat_mutex_free(u->mu);
}

/* A quit signal ends the loop like closing the window does, so the cleanup
 * always runs. */
static void quit_cb(void *ud)
{
    (void)ud;
    window_quit();
}

int ui_run(const Config *cfg)
{
    static Ui ui; /* large; only one window per process */
    Ui *u = &ui;
    memset(u, 0, sizeof *u);
    u->cfg = *cfg;
    u->mu = plat_mutex_new();
    if (!u->mu)
        return 1;

    /* A new window shows the art WIN_ART tall at the chosen text size. */
    Metrics m0 = metrics_for(u->cfg.font_size);
    if (window_open("Smalltify Player", WIN_W, (int)ceilf(player_reserve(&m0) + WIN_ART), MIN_W, MIN_H,
                    frame, u) != 0) {
        plat_mutex_free(u->mu);
        return 1;
    }
    (void)plat_quit_watch(quit_cb, NULL); /* not fatal: signals then just kill us */
    apply_metrics(u); /* the window's minimum size follows from it (fit_window) */

    int rc = 0;
    if (u->cfg.read_only)
        set_screen(u, SCR_MOVE);
    else if (!u->cfg.client_id[0])
        set_screen(u, SCR_SETUP);
    else if (enter_app(u) != 0) {
        rc = 1;
        startup_failed = 1;
        window_quit(); /* nothing to show: window_run returns and cleans up */
    }

    window_run(close_cb, u);
    return rc;
}
