/* GUI: setup notice -> sign-in -> Soloist key / download / pairing -> player.
 * The loop sleeps until input or window_wake (Raven event waiting), so an idle
 * window costs no CPU. Background threads call window_wake when they have
 * news. All network work runs on worker threads; the UI thread only copies
 * snapshots and pushes what changed into the widgets.
 *
 * The update pass (ui_update) runs once per loop iteration, before Raven's
 * layout and paint: it pulls the workers' news into Ui, then calls the
 * update of each screen module shown, which compares and changes only the
 * widgets whose value changed. Nothing changed, nothing is drawn. */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui/shell.h"
#include "gui/theme.h"
#include "gui/window.h"

#include "art.h"
#include "auth.h"
#include "browse.h"
#include "http.h"
#include "library.h"
#include "model.h"
#include "platform.h"
#include "poller.h"
#include "soloist.h"
#include "spotify.h"

#include "screens/player.h"
#include "screens/settings.h"
#include "screens/sidebar.h"
#include "screens/signin.h"
#include "ui_internal.h"

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
 * otherwise, so a waiting queue add is never overwritten. Without a
 * session (no worker) there is nobody to send it to. */
static void cmd_send(CmdWorker *w, const Cmd *c)
{
    if (!w->th)
        return;
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

/* Requests go to slots the worker empties; without a session (no worker)
 * they are dropped. */
void lib_post(LibWorker *w, const LibReq *r)
{
    if (!w->th)
        return;
    plat_mutex_lock(w->mu);
    w->req = *r;
    w->has_req = 1;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

static void lib_post_check(LibWorker *w, const char *uri)
{
    if (!w->th)
        return;
    plat_mutex_lock(w->mu);
    snprintf(w->check, sizeof w->check, "%s", uri);
    w->has_check = 1;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

void lib_post_save(LibWorker *w, const char *uri, int on, uint64_t seq)
{
    if (!w->th)
        return;
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
    u->status[0] = u->hint[0] = 0;
    u->status_error = 0;
    u->played_on[0] = 0;
    u->art_seq = 0;
    u->devs_seq = u->dev_seq_noted = 0;
    memset(&u->devs, 0, sizeof u->devs);
    lib_reset();
    u->heart_uri[0] = 0;
    u->heart = -1;
    u->heart_gen = u->like_sent = u->like_done = 0;
    u->like_err = SP_OK;
    u->note_until = 0;
}

void set_screen(Ui *u, Screen s)
{
    u->screen = s;
}

/* The art goes: the screens show none (art_gen), the wallpaper and the
 * accent are the theme's again. The texture is deleted at the end of the
 * next update pass, once no Image shows it. */
static void drop_art(Ui *u)
{
    if (Rtexture_is_valid(u->art_old))
        Rtexture_destroy(u->art_old);
    u->art_old = u->art_tex;
    u->art_tex = R_INVALID_ID;
    u->art_gen++;
    theme_set_art(NULL, 0);
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
    drop_art(u);
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

/* A pasted ID may come with blanks around it. */
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

/* The sign-in screen's "Reload .env": a client ID fixed in .env applies
 * without a restart; the setup screen when .env has none any more. */
void reload_env(Ui *u)
{
    Config c;
    AuthStore *a = NULL;
    if (config_load(&c) == 0 && (a = auth_store_new(&c)) != NULL) {
        auth_store_free(u->auth); /* nothing else uses it on this screen */
        u->auth = a;
        u->cfg = c;
        u->login_msg = NULL;
        if (!c.client_id[0])
            set_screen(u, SCR_SETUP);
    }
    memset(&c, 0, sizeof c);
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

static void login_join(Ui *u)
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
    lib_retry(u);
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
static void note_other(Ui *u, const char *name)
{
    if (!name[0] || strcmp(name, u->cfg.device_name) == 0)
        return;
    if (strcmp(u->other_dev, name) != 0)
        snprintf(u->other_dev, sizeof u->other_dev, "%s", name);
    if (!u->paired && !u->auto_failed)
        start_engine(u, 0);
}

/* Follow the install worker and Soloist's state; runs in every update pass
 * of the player (passes only happen on events, so this adds no wakeups). */
static void poll_engine(Ui *u)
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
        return;
    }
    if (u->paired && u->sol_was_paired && st == SOLOIST_PAIRING) {
        /* Its stored session is gone: pair again like the first time. */
        u->paired = u->sol_was_paired = 0;
        (void)config_set_paired(&u->cfg, 0);
        device_stop(&u->dev);
        device_start(&u->dev, u->auth, NULL, u->cfg.device_name, 1);
    }
    if (!u->paired && st == SOLOIST_READY) {
        /* "running" without "waiting for login": a stored session came
         * back, so it is paired (from before the marker existed). */
        device_stop(&u->dev);
        u->paired = u->sol_was_paired = 1;
        (void)config_set_paired(&u->cfg, 1);
    }
    if (!u->paired && u->dev.th) {
        if (u->dev.found) {
            /* Picked in a Spotify app: ours from now on, at every launch. */
            device_stop(&u->dev);
            u->paired = 1;
            (void)config_set_paired(&u->cfg, 1);
        } else if (u->dev.idle_out) {
            stop_engine(u); /* nobody can pick us: save the battery */
        } else {
            char other[SP_NAME_LEN];
            plat_mutex_lock(u->dev.mu);
            memcpy(other, u->dev.other, sizeof other);
            plat_mutex_unlock(u->dev.mu);
            if (other[0] && strcmp(other, u->other_dev) != 0)
                memcpy(u->other_dev, other, sizeof u->other_dev);
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
static void apply_snapshot(Ui *u, const PlaybackState *s, uint64_t now)
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
static int heart_sync(Ui *u)
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

/* The single most relevant status line; returns 1 if it is an error. */
static int status_message(Ui *u, char *out, size_t cap)
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
static void sync_devices(Ui *u)
{
    plat_mutex_lock(u->cmd.mu);
    if (u->cmd.devs_seq != u->devs_seq) {
        u->devs_seq = u->cmd.devs_seq;
        u->devs = u->cmd.devs;
    }
    plat_mutex_unlock(u->cmd.mu);
}

void log_out(Ui *u)
{
    stop_session(u); /* Soloist first: logout deletes its pairing */
    auth_store_logout(u->auth);
    u->reinstalled = 0;
    u->login_msg = NULL;
    advance_signin(u);
}

void set_text_size(Ui *u, int size)
{
    if (size < 0 || size >= CFG_FONT_SIZES || size == u->cfg.font_size)
        return;
    u->cfg.font_size = size; /* the update pass applies it */
    u->size_unsaved = config_set_env_value(&u->cfg, "UI_FONT_SIZE", config_font_size_value(size)) != 0;
}

/* --- the update pass ----------------------------------------------------- */
/* New art bytes: one texture per image, and the wallpaper and accent taken
 * from it. The screens show the texture when art_gen changes; the old one
 * goes at the end of the pass. */
static void take_art(Ui *u)
{
    const unsigned char *data;
    size_t len;
    if (!u->art || !art_take(u->art, &data, &len, &u->art_seq))
        return;
    int jpg = len > 2 && data[0] == 0xFF && data[1] == 0xD8;
    int png = len > 4 && !memcmp(data, "\x89PNG", 4);
    drop_art(u);
    if (!jpg && !png)
        return; /* empty bytes: nothing playing */
    u->art_tex = Rtexture_create((const char *)data, len);
    theme_set_art(data, len);
}

/* The workers' news for the player: the snapshot, the commands' results,
 * errors, the heart, Soloist, the status line and the pairing tip, the
 * devices and the art. May leave the player (an expired session). */
static void pull_player(Ui *u, uint64_t now)
{
    PlaybackState snap;

    /* Only minimized counts as background: a visible window, focused or
     * not, keeps its 1 Hz progress tick. */
    int bg = window_is_minimized();
    if (bg != u->background) {
        u->background = bg;
        poller_set_background(u->poller, bg);
    }
    u->dev.slow = bg;

    if (poller_snapshot(u->poller, &snap, &u->seq)) {
        apply_snapshot(u, &snap, now);
        art_request(u->art, snap.track.art_url); /* no-op unless the URL changed */
        if (snap.active)
            note_other(u, snap.device_name);
    }
    if (u->pend.mask && now >= u->pend.until)
        resync(u);

    plat_mutex_lock(u->cmd.mu);
    uint64_t cseq = u->cmd.err_seq, done_at = u->cmd.done_at;
    int cerr = u->cmd.err, queued = u->cmd.done == CMD_QUEUE;
    char played_on[SP_NAME_LEN], other[SP_NAME_LEN] = "";
    memcpy(played_on, u->cmd.played_on, sizeof played_on);
    int fresh = u->cmd.devs_seq != u->dev_seq_noted;
    u->dev_seq_noted = u->cmd.devs_seq;
    if (fresh) /* a fresh device list names another device? */
        for (int i = 0; i < u->cmd.devs.ndev && !other[0]; i++)
            if (u->cmd.devs.devs[i].id[0] && strcmp(u->cmd.devs.devs[i].name, u->cfg.device_name))
                memcpy(other, u->cmd.devs.devs[i].name, sizeof other);
    plat_mutex_unlock(u->cmd.mu);
    note_other(u, other);
    if (cseq != u->cmd_seq) {
        u->cmd_seq = cseq;
        u->cmd_err = cerr;
        memcpy(u->played_on, played_on, sizeof u->played_on);
        /* The worker wakes us PENDING_MS after its last command, which
         * takes the note down again. */
        u->note_until = queued && cerr == SP_OK ? done_at + PENDING_MS : 0;
        if (cerr != SP_OK)
            resync(u);
    }
    u->poll_err = poller_last_error(u->poller);
    /* auth.c forgets a revoked refresh token; only a new sign-in helps. */
    if ((u->poll_err == SP_ERR_AUTH || u->cmd_err == SP_ERR_AUTH) && !auth_store_logged_in(u->auth)) {
        stop_session(u);
        u->login_msg = "Session expired. Please sign in again.";
        advance_signin(u);
        return;
    }
    if (heart_sync(u))
        return;
    poll_engine(u);
    plat_mutex_lock(u->cmd.mu);
    u->cmd.local = local_ready(u);
    plat_mutex_unlock(u->cmd.mu);

    u->status_error = status_message(u, u->status, sizeof u->status);
    /* The one pairing UI: a dismissible tip while an unpaired Soloist is
     * discoverable. */
    if (u->sol && !u->paired && !u->hint_hidden)
        snprintf(u->hint, sizeof u->hint, "Tip: in Spotify on %s, choose '%s' to play on this computer.",
                 u->other_dev[0] ? u->other_dev : "your phone or computer", u->cfg.device_name);
    else
        u->hint[0] = 0;
    sync_devices(u);
    take_art(u);
}

/* The sign-in thread finished: on to the player (or back to it, over which
 * the sign-in was), or say why not. */
static void pull_login(Ui *u)
{
    if (!u->login_th)
        return;
    plat_mutex_lock(u->mu);
    int done = u->login_done, rc = u->login_rc;
    plat_mutex_unlock(u->mu);
    if (!done)
        return;
    int cancelled = u->login_cancel;
    login_join(u);
    if (rc == 0) {
        u->login_msg = NULL;
        u->consent_asked = 1; /* it got what Spotify grants */
        if (u->reauth)
            end_reauth(u, 1);
        else
            advance_signin(u);
        return;
    }
    u->login_msg = cancelled ? "Sign-in cancelled." : auth_login_strerror(rc);
}

/* Once per loop iteration: what the workers brought goes into Ui, then each
 * module shown pushes what changed into its widgets. */
static void ui_update(Ui *u)
{
    theme_set_text_size(u->cfg.font_size); /* no-op unless it changed */
    if (u->screen == SCR_LOGIN)
        pull_login(u);
    if (u->screen == SCR_PLAYER && u->poller)
        pull_player(u, plat_now_ms());
    shell_show(u->screen == SCR_PLAYER ? SHELL_PLAYER : SHELL_CARD);
    if (u->screen == SCR_PLAYER) {
        player_update(u);
        sidebar_update(u);
        /* the player shows art_tex now: the art before it can go */
        if (Rtexture_is_valid(u->art_old))
            Rtexture_destroy(u->art_old);
        u->art_old = R_INVALID_ID;
    } else {
        signin_update(u);
    }
    settings_update(u);
}

/* --- main loop ----------------------------------------------------------- */
/* Set when enter_app fails: the loop then ends without an update. */
static int startup_failed;

static void frame(void *user, double dt)
{
    Ui *u = user;
    if (startup_failed)
        return;
    shell_frame(dt);
    ui_update(u);
}

/* Runs once when the window closes, or when window_quit ends the loop: every
 * thread that could call window_wake is joined before the window goes. */
static void close_cb(void *ud)
{
    Ui *u = ud;
    stop_session(u);
    login_join(u);
    lib_reset();
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

/* Settings (Cmd+, and the sidebar's gear) and Find (Cmd+F) belong to the
 * modules; they are bound here, where the modules meet. */
static void on_settings(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    settings_open(ud);
}

static void on_find(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    Ui *u = ud;
    if (u->screen != SCR_PLAYER)
        return;
    shell_set_sidebar(1);
    sidebar_find(u);
}

/* SMP_UI_PREVIEW=player|setup|key|move shows that screen without a
 * session, nothing talking to Spotify: for working on the screens
 * without signing in. */
static int preview_screen(Ui *u)
{
    static const struct {
        const char *name;
        Screen screen;
    } screens[] = { { "player", SCR_PLAYER }, { "setup", SCR_SETUP }, { "key", SCR_KEY }, { "move", SCR_MOVE } };
    const char *want = getenv("SMP_UI_PREVIEW");
    for (size_t i = 0; want && i < sizeof screens / sizeof screens[0]; i++) {
        if (strcmp(want, screens[i].name) == 0) {
            u->preview = 1;
            set_screen(u, screens[i].screen);
            return 1;
        }
    }
    return 0;
}

int ui_run(const Config *cfg)
{
    static Ui ui; /* large; only one window per process */
    Ui *u = &ui;
    memset(u, 0, sizeof *u);
    u->cfg = *cfg;
    u->heart = -1;
    u->art_tex = u->art_old = R_INVALID_ID;
    u->mu = plat_mutex_new();
    if (!u->mu)
        return 1;

    if (window_open("Smalltify Player", WIN_W, WIN_H, MIN_W, MIN_H) != 0) {
        plat_mutex_free(u->mu);
        return 1;
    }
    theme_set_text_size(u->cfg.font_size);
    shell_build();
    player_build(u, shell_main_slot());
    sidebar_build(u, shell_sidebar_slot());
    signin_build(u, shell_card_slot());
    Raction_bind(ACT_SETTINGS, on_settings, u);
    Raction_bind(ACT_FIND, on_find, u);
    (void)plat_quit_watch(quit_cb, NULL); /* not fatal: signals then just kill us */

    int rc = 0;
    if (preview_screen(u))
        rc = 0; /* no session: nothing to enter */
    else if (u->cfg.read_only)
        set_screen(u, SCR_MOVE);
    else if (!u->cfg.client_id[0])
        set_screen(u, SCR_SETUP);
    else if (enter_app(u) != 0) {
        rc = 1;
        startup_failed = 1;
        window_quit(); /* nothing to show: window_run returns and cleans up */
    }

    window_run(frame, close_cb, u);
    return rc;
}
