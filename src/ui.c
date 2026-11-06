/* raylib GUI: setup notice -> sign-in -> player. The loop sleeps inside
 * glfwWaitEvents (EnableEventWaiting) and background threads wake it with
 * glfwPostEmptyEvent, so an idle window costs no CPU. All network work runs
 * on worker threads; the UI thread only copies snapshots and draws. */
#include "ui.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "art.h"
#include "auth.h"
#include "http.h"
#include "librespot.h"
#include "model.h"
#include "platform.h"
#include "poller.h"
#include "spotify.h"

/* Part of the GLFW bundled inside libraylib.a; raylib has no wrapper for it.
 * GLFW documents it as callable from any thread. */
void glfwPostEmptyEvent(void);

#define WIN_W 360
#define WIN_H 520
#define MIN_W 320
#define MIN_H 440
#define PAD   16
/* The default font is a 10 px bitmap font: integer multiples stay crisp. */
#define FS_S  10
#define FS_L  20

/* Optimistic edits survive contradicting snapshots this long: right after a
 * command Spotify often still reports the old state. */
#define PENDING_MS        3000
#define DEVICE_WAIT_TRIES 15
#define DEVICE_FIRST_MS   1500 /* librespot needs a moment to register */
#define DEVICE_RETRY_MS   1000
#define DEVICE_LIST_CAP   16
#define IDLE_WAIT_MS      3600000u

#define C_BG     CLITERAL(Color){  18,  18,  18, 255 }
#define C_PANEL  CLITERAL(Color){  40,  40,  40, 255 }
#define C_HOVER  CLITERAL(Color){  58,  58,  58, 255 }
#define C_PRESS  CLITERAL(Color){  28,  28,  28, 255 }
#define C_TEXT   CLITERAL(Color){ 235, 235, 235, 255 }
#define C_DIM    CLITERAL(Color){ 150, 150, 150, 255 }
#define C_ACCENT CLITERAL(Color){  30, 215,  96, 255 }
#define C_ON     CLITERAL(Color){  22, 110,  55, 255 }
#define C_ERR    CLITERAL(Color){ 240, 110, 110, 255 }

static void ui_wake(void *ud)
{
    (void)ud;
    glfwPostEmptyEvent();
}

/* --- command worker: single-slot mailbox, latest command wins ----------- */
typedef enum {
    CMD_PLAY, CMD_PAUSE, CMD_NEXT, CMD_PREV,
    CMD_SEEK, CMD_VOLUME, CMD_SHUFFLE, CMD_REPEAT,
    CMD_PLAY_URI,  /* str = URI, arg = is_track */
    CMD_PLAYLISTS, /* results land in CmdWorker.lib */
    CMD_SEARCH,    /* str = query; results land in CmdWorker.lib */
    CMD_DEVICES,   /* results land in CmdWorker.lib */
    CMD_TRANSFER   /* str = device id; plays there, then refreshes devices */
} CmdKind;

typedef struct {
    CmdKind kind;
    int     arg;
    char    str[SP_NAME_LEN];
} Cmd;

#define LIB_PLAYLISTS 50 /* first page of /me/playlists */
#define LIB_RESULTS   20

/* Library panel data, filled by the command worker. */
typedef struct {
    Playlist pl[LIB_PLAYLISTS];
    int      npl, pl_loaded;
    Track    hits[LIB_RESULTS];
    int      nhits, searched;
    Device   devs[DEVICE_LIST_CAP];
    int      ndev, devs_loaded;
} Library;

typedef struct {
    AuthStore   *auth;
    Poller      *poller;
    plat_mutex  *mu;
    plat_event  *ev;
    plat_thread *th;
    /* Guarded by mu. */
    Cmd      slot;
    int      has;
    int      stop;
    int      err;     /* result of the last command */
    uint64_t err_seq; /* bumped after every command */
    Library  lib;     /* latest playlists/search results */
    uint64_t lib_seq; /* bumped when lib changes */
    int      no_local; /* Spotify refused our engine: never fall back to it */
} CmdWorker;

/* Our own device as the fallback target, unless Spotify refused it. */
static int find_local(CmdWorker *w, HttpClient *h, Device *d)
{
    plat_mutex_lock(w->mu);
    int no_local = w->no_local;
    plat_mutex_unlock(w->mu);
    return no_local ? SP_ERR_NODEVICE : sp_find_device(h, CFG_DEVICE_NAME, d);
}

static int fetch_devices(CmdWorker *w, HttpClient *h)
{
    static Device devs[DEVICE_LIST_CAP]; /* worker-thread only */
    int n = 0;
    int rc = sp_get_devices(h, devs, DEVICE_LIST_CAP, &n);
    if (rc != SP_OK)
        return rc;
    plat_mutex_lock(w->mu);
    memcpy(w->lib.devs, devs, sizeof devs);
    w->lib.ndev = n;
    w->lib.devs_loaded = 1;
    w->lib_seq++;
    plat_mutex_unlock(w->mu);
    return rc;
}

static int run_cmd(CmdWorker *w, HttpClient *h, const Cmd *c)
{
    Device d;
    int rc;

    switch (c->kind) {
    case CMD_PLAY:
        rc = sp_play(h, NULL);
        /* Nothing active anywhere: start playback on our own device. */
        if (rc == SP_ERR_NODEVICE && (rc = find_local(w, h, &d)) == SP_OK)
            rc = sp_transfer(h, d.id, 1);
        return rc;
    case CMD_PAUSE:   return sp_pause(h);
    case CMD_NEXT:    return sp_next(h);
    case CMD_PREV:    return sp_prev(h);
    case CMD_SEEK:    return sp_seek(h, c->arg);
    case CMD_VOLUME:  return sp_set_volume(h, c->arg);
    case CMD_SHUFFLE: return sp_set_shuffle(h, c->arg);
    case CMD_REPEAT:  return sp_set_repeat(h, (RepeatMode)c->arg);
    case CMD_PLAY_URI:
        rc = sp_play_uri(h, NULL, c->str, c->arg);
        if (rc == SP_ERR_NODEVICE && (rc = find_local(w, h, &d)) == SP_OK)
            rc = sp_play_uri(h, d.id, c->str, c->arg);
        return rc;
    case CMD_DEVICES:
        return fetch_devices(w, h);
    case CMD_TRANSFER:
        rc = sp_transfer(h, c->str, 1);
        if (rc == SP_OK)
            fetch_devices(w, h); /* only refreshes the marker; its error is moot */
        return rc;
    case CMD_PLAYLISTS:
    case CMD_SEARCH: {
        /* Fetched outside the lock: the UI thread reads lib every frame. */
        static Playlist pl[LIB_PLAYLISTS]; /* worker-thread only; keeps the stack small */
        static Track hits[LIB_RESULTS];
        int n = 0;
        rc = c->kind == CMD_PLAYLISTS ? sp_get_playlists(h, pl, LIB_PLAYLISTS, &n)
                                      : sp_search_tracks(h, c->str, hits, LIB_RESULTS, &n);
        if (rc != SP_OK)
            return rc;
        plat_mutex_lock(w->mu);
        if (c->kind == CMD_PLAYLISTS) {
            memcpy(w->lib.pl, pl, sizeof pl);
            w->lib.npl = n;
            w->lib.pl_loaded = 1;
        } else {
            memcpy(w->lib.hits, hits, sizeof hits);
            w->lib.nhits = n;
            w->lib.searched = 1;
        }
        w->lib_seq++;
        plat_mutex_unlock(w->mu);
        return rc;
    }
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
        int stop = w->stop, has = w->has;
        Cmd c = w->slot;
        w->has = 0;
        plat_mutex_unlock(w->mu);
        if (stop)
            break;
        if (!has) {
            /* One extra wake once optimistic edits expire, so the UI resyncs
             * even if no snapshot ever confirms or contradicts them. */
            int got = plat_event_wait(w->ev, recheck ? PENDING_MS : IDLE_WAIT_MS);
            if (!got && recheck) {
                recheck = 0;
                ui_wake(NULL);
            }
            continue;
        }
        int rc = http ? run_cmd(w, http, &c) : SP_ERR_OTHER;
        poller_kick(w->poller);
        plat_mutex_lock(w->mu);
        w->err = rc;
        w->err_seq++;
        plat_mutex_unlock(w->mu);
        ui_wake(NULL);
        recheck = 1;
    }
    http_client_free(http);
}

static void cmd_stop(CmdWorker *w)
{
    if (w->th) {
        plat_mutex_lock(w->mu);
        w->stop = 1;
        plat_mutex_unlock(w->mu);
        plat_event_signal(w->ev);
        plat_thread_join(w->th);
    }
    plat_event_free(w->ev);
    plat_mutex_free(w->mu);
    memset(w, 0, sizeof *w);
}

static int cmd_start(CmdWorker *w, AuthStore *auth, Poller *poller)
{
    memset(w, 0, sizeof *w);
    w->auth = auth;
    w->poller = poller;
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

static void cmd_post_str(CmdWorker *w, CmdKind kind, int arg, const char *str)
{
    plat_mutex_lock(w->mu);
    w->slot.kind = kind;
    w->slot.arg = arg;
    snprintf(w->slot.str, sizeof w->slot.str, "%s", str);
    w->has = 1;
    plat_mutex_unlock(w->mu);
    plat_event_signal(w->ev);
}

static void cmd_post(CmdWorker *w, CmdKind kind, int arg)
{
    cmd_post_str(w, kind, arg, "");
}

/* --- one-shot device worker: make our Connect device the active one ----- */
typedef struct {
    AuthStore   *auth;
    Poller      *poller;
    Librespot   *ls;
    plat_event  *stop; /* signaled = give up now */
    plat_thread *th;
} DeviceWorker;

static void device_thread(void *arg)
{
    DeviceWorker *d = arg;
    HttpAuth ha = auth_store_http(d->auth);
    HttpClient *http = http_client_new(&ha);

    for (int i = 0; http && i < DEVICE_WAIT_TRIES; i++) {
        if (plat_event_wait(d->stop, i ? DEVICE_RETRY_MS : DEVICE_FIRST_MS))
            break;
        Device me;
        if (sp_find_device(http, CFG_DEVICE_NAME, &me) != SP_OK)
            continue;
        /* Don't pick ours once Spotify refused it, and never steal playback
         * from another device the user is using. */
        if (librespot_playback_refused(d->ls))
            break;
        Device list[DEVICE_LIST_CAP];
        int n = 0, busy = 1;
        if (sp_get_devices(http, list, DEVICE_LIST_CAP, &n) == SP_OK) {
            busy = 0;
            for (int j = 0; j < n; j++)
                busy |= list[j].is_active;
        }
        if (!busy && sp_transfer(http, me.id, 0) == SP_OK)
            poller_kick(d->poller);
        break;
    }
    http_client_free(http);
}

static void device_stop(DeviceWorker *d)
{
    if (d->th) {
        plat_event_signal(d->stop);
        plat_thread_join(d->th);
    }
    plat_event_free(d->stop);
    memset(d, 0, sizeof *d);
}

static void device_start(DeviceWorker *d, AuthStore *auth, Poller *poller, Librespot *ls)
{
    memset(d, 0, sizeof *d);
    d->auth = auth;
    d->poller = poller;
    d->ls = ls;
    d->stop = plat_event_new();
    if (d->stop)
        d->th = plat_thread_start(device_thread, d);
    if (!d->th)
        device_stop(d); /* not fatal: the user can still pick the device in Spotify */
}

/* --- engine sign-in worker: librespot's own one-time OAuth -------------- */
#define ENGINE_POLL_MS 500
#define SIGNIN_URL_LEN 4096 /* authorize URLs with many scopes are long */

typedef enum { ENG_WAITING, ENG_DONE, ENG_DIED } EngineState;

typedef struct {
    const Config *cfg;
    Librespot    *ls;
    plat_mutex   *mu;
    plat_event   *stop;
    plat_thread  *th;
    /* Guarded by mu. */
    char         url[SIGNIN_URL_LEN]; /* "" while no sign-in page is pending */
    uint64_t     url_seq;
    EngineState  state;
} EngineLogin;

/* librespot only reports through files and its output, so check both at a
 * slow fixed rate and wake the UI only when something changed. */
static void engine_thread(void *arg)
{
    EngineLogin *e = arg;
    char url[SIGNIN_URL_LEN], last[SIGNIN_URL_LEN] = "";
    EngineState state = ENG_WAITING;

    while (state == ENG_WAITING && !plat_event_wait(e->stop, ENGINE_POLL_MS)) {
        if (librespot_has_credentials(e->cfg))
            state = ENG_DONE;
        else if (!librespot_running(e->ls))
            state = ENG_DIED;
        if (librespot_login_url(e->ls, url, sizeof url) != 1)
            url[0] = 0;
        if (state == ENG_WAITING && strcmp(url, last) == 0)
            continue;
        memcpy(last, url, sizeof last);
        plat_mutex_lock(e->mu);
        memcpy(e->url, url, sizeof e->url);
        e->url_seq++;
        e->state = state;
        plat_mutex_unlock(e->mu);
        ui_wake(NULL);
    }
}

static void engine_stop(EngineLogin *e)
{
    if (e->th) {
        plat_event_signal(e->stop);
        plat_thread_join(e->th);
    }
    plat_event_free(e->stop);
    plat_mutex_free(e->mu);
    memset(e, 0, sizeof *e);
}

static int engine_start(EngineLogin *e, const Config *cfg, Librespot *ls)
{
    memset(e, 0, sizeof *e);
    e->cfg = cfg;
    e->ls = ls;
    e->mu = plat_mutex_new();
    e->stop = plat_event_new();
    if (e->mu && e->stop)
        e->th = plat_thread_start(engine_thread, e);
    if (!e->th) {
        engine_stop(e);
        return -1;
    }
    return 0;
}

/* --- UI state ------------------------------------------------------------ */
typedef enum { SCR_SETUP, SCR_LOGIN, SCR_PLAYER } Screen;
typedef enum { PANEL_NONE, PANEL_LIBRARY, PANEL_DEVICES } Panel;

/* Widget ids for the one widget that may hold the mouse. */
enum {
    W_NONE, W_RELOAD, W_SIGNIN, W_CANCEL, W_CONNECT, W_OPEN_URL,
    W_PREV, W_PLAY, W_NEXT, W_SEEK, W_VOLUME, W_SHUFFLE, W_REPEAT, W_LOGOUT,
    W_LIBRARY, W_LIB_BACK, W_DEVICES, W_DEV_REFRESH,
    W_LIB_ROW /* + row index; library and device rows never show together */
};

enum { PEND_PLAY = 1, PEND_SEEK = 2, PEND_SHUFFLE = 4, PEND_REPEAT = 8, PEND_VOLUME = 16 };

typedef struct {
    int        mask;
    int        playing, shuffle, volume;
    RepeatMode repeat;
    int        progress_ms;
    uint64_t   progress_at;
    uint64_t   until;
} Pending;

typedef struct {
    Config     cfg;
    AuthStore *auth;
    Screen     screen;
    Font       font;
    int        active;   /* widget holding the mouse, W_NONE if free */
    int        redraw;   /* state changed mid-frame: draw one more frame */
    int        w, h;
    int        text_dirty;

    /* setup screen */
    char env_wrapped[CFG_PATH_LEN + 64];
    const char *setup_msg;

    /* sign-in screen: step 1 = our PKCE login, step 2 = librespot's OAuth */
    int          step;
    int          need_engine; /* step 2 is required after step 1 */
    plat_mutex  *mu;
    plat_thread *login_th;
    volatile int login_cancel;
    int          login_done, login_rc;     /* guarded by mu */
    char         login_url[SIGNIN_URL_LEN]; /* guarded by mu */
    const char  *login_msg;
    EngineLogin  eng;
    uint64_t     eng_seq;
    int          eng_has_url;

    /* player screen */
    Librespot    *ls;
    Poller       *poller;
    CmdWorker     cmd;
    DeviceWorker  dev;
    PlaybackState st;
    uint64_t      seq;
    Pending       pend;
    uint64_t      cmd_seq;
    int           cmd_err, poll_err, background;
    float         seek_t, vol_t; /* slider positions while dragged */
    char ls_status[256], ls_shown[256];
    char title[SP_NAME_LEN], artists[SP_NAME_LEN], album[SP_NAME_LEN];
    char status[SP_NAME_LEN];
    char t_pos[16], t_dur[16];
    int  pos_sec; /* second shown in t_pos, -1 = reformat */

    /* album art: decoded once per image, drawn from a texture */
    ArtLoader *art;
    uint64_t   art_seq;
    Texture2D  art_tex; /* id 0 = none */

    /* side panels over the player area (one at a time) */
    Panel    panel;
    int      refused_seen; /* the device picker auto-opened once for a refusal */
    int      lib_scroll;
    int      lib_show_hits; /* list search results instead of playlists */
    uint64_t lib_seq;
    Library  lib;
    char     query[SP_NAME_LEN];
} Ui;

/* --- text helpers -------------------------------------------------------- */
static float spacing(int fs) { return (float)fs / 10.0f; }

static float text_w(const Ui *u, const char *s, int fs)
{
    return MeasureTextEx(u->font, s, (float)fs, spacing(fs)).x;
}

static void text(const Ui *u, const char *s, float x, float y, int fs, Color c)
{
    DrawTextEx(u->font, s, (Vector2){ x, y }, (float)fs, spacing(fs), c);
}

static void text_center(const Ui *u, const char *s, float y, int fs, Color c)
{
    text(u, s, ((float)u->w - text_w(u, s, fs)) / 2.0f, y, fs, c);
}

/* Copy src into out, cut at a UTF-8 boundary with "..." so it fits maxw.
 * (The default font has no U+2026 glyph.) */
static void fit_text(const Ui *u, const char *src, int fs, float maxw, char *out, size_t cap)
{
    snprintf(out, cap, "%s", src);
    size_t n = strlen(out);
    if (text_w(u, out, fs) <= maxw)
        return;
    while (n > 0) {
        do
            n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
        if (n + 4 > cap)
            continue;
        memcpy(out + n, "...", 4);
        if (text_w(u, out, fs) <= maxw)
            return;
    }
}

/* Word-wrap src onto at most two lines of maxw; the second is cut with "...". */
static void fit_two_lines(const Ui *u, const char *src, int fs, float maxw, char *out, size_t cap)
{
    size_t best = 0;
    for (size_t i = 0; src[i] && i + 1 < cap; i++) {
        if (src[i] != ' ')
            continue;
        memcpy(out, src, i);
        out[i] = 0;
        if (text_w(u, out, fs) > maxw)
            break;
        best = i;
    }
    fit_text(u, src, fs, maxw, out, cap);
    if (strcmp(out, src) == 0 || best == 0)
        return; /* fits on one line, or no word break fits */
    memcpy(out, src, best);
    out[best] = '\n';
    fit_text(u, src + best + 1, fs, maxw, out + best + 1, cap - best - 1);
}

/* Hard-wrap src (a path: no spaces to break at) into lines of maxw. */
static void wrap_text(const Ui *u, const char *src, int fs, float maxw, char *out, size_t cap)
{
    size_t o = 0, line = 0, i = 0;
    out[0] = 0;
    while (src[i]) {
        size_t len = 1;
        while (src[i + len] && ((unsigned char)src[i + len] & 0xC0) == 0x80)
            len++;
        if (o + len + 2 > cap)
            break;
        memcpy(out + o, src + i, len);
        out[o + len] = 0;
        if (o > line && text_w(u, out + line, fs) > maxw) {
            memmove(out + o + 1, out + o, len);
            out[o] = '\n';
            line = ++o;
        }
        o += len;
        out[o] = 0;
        i += len;
    }
}

static void fmt_time(char *out, size_t cap, int ms)
{
    int s = ms > 0 ? ms / 1000 : 0;
    snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

/* --- widgets ------------------------------------------------------------- */
static int hovered(Rectangle r)
{
    return CheckCollisionPointRec(GetMousePosition(), r);
}

/* Draws the button body; returns 1 on press-and-release inside r. */
static int button(Ui *u, int id, Rectangle r, const char *label, int fs, int on)
{
    int hot = hovered(r);
    int click = 0;
    if (u->active == W_NONE && hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        u->active = id;
    if (u->active == id && !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        u->active = W_NONE;
        click = hot;
        u->redraw |= click;
    }
    Color bg = on ? C_ON : C_PANEL;
    if (u->active == id && hot)
        bg = C_PRESS;
    else if (hot && !on)
        bg = C_HOVER;
    DrawRectangleRounded(r, 0.3f, 6, bg);
    if (label) {
        float tw = text_w(u, label, fs);
        text(u, label, r.x + (r.width - tw) / 2.0f, r.y + (r.height - (float)fs) / 2.0f, fs, C_TEXT);
    }
    return click;
}

/* Horizontal slider over [0,1]. While dragged, *t follows the mouse;
 * returns 1 on release so callers send one command, not one per frame. */
static int slider(Ui *u, int id, Rectangle r, float *t, int enabled)
{
    Rectangle hit = { r.x - 6, r.y - 8, r.width + 12, r.height + 16 };
    int commit = 0;
    if (enabled && u->active == W_NONE && hovered(hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        u->active = id;
    if (u->active == id) {
        float v = (GetMousePosition().x - r.x) / r.width;
        *t = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            u->active = W_NONE;
            commit = 1;
            u->redraw = 1;
        }
    }
    float fill = r.width * *t;
    DrawRectangleRounded(r, 1.0f, 6, C_PANEL);
    if (enabled) {
        DrawRectangleRounded((Rectangle){ r.x, r.y, fill, r.height }, 1.0f, 6,
                             u->active == id || hovered(hit) ? C_ACCENT : C_TEXT);
        if (u->active == id || hovered(hit))
            DrawCircleV((Vector2){ r.x + fill, r.y + r.height / 2.0f }, r.height + 1.0f, C_TEXT);
    }
    return commit;
}

typedef enum { ICON_PREV, ICON_PLAY, ICON_PAUSE, ICON_NEXT } Icon;

static void icon(Icon k, Rectangle r, Color c)
{
    float cx = r.x + r.width / 2.0f, cy = r.y + r.height / 2.0f, s = r.height * 0.22f;
    switch (k) {
    case ICON_PLAY:
        DrawTriangle((Vector2){ cx - s * 0.8f, cy - s }, (Vector2){ cx - s * 0.8f, cy + s },
                     (Vector2){ cx + s, cy }, c);
        break;
    case ICON_PAUSE:
        DrawRectangleRec((Rectangle){ cx - s * 0.8f, cy - s, s * 0.55f, s * 2.0f }, c);
        DrawRectangleRec((Rectangle){ cx + s * 0.25f, cy - s, s * 0.55f, s * 2.0f }, c);
        break;
    case ICON_PREV:
        DrawRectangleRec((Rectangle){ cx - s, cy - s, s * 0.3f, s * 2.0f }, c);
        DrawTriangle((Vector2){ cx + s, cy - s }, (Vector2){ cx - s * 0.6f, cy },
                     (Vector2){ cx + s, cy + s }, c);
        break;
    case ICON_NEXT:
        DrawTriangle((Vector2){ cx - s, cy - s }, (Vector2){ cx - s, cy + s },
                     (Vector2){ cx + s * 0.6f, cy }, c);
        DrawRectangleRec((Rectangle){ cx + s * 0.7f, cy - s, s * 0.3f, s * 2.0f }, c);
        break;
    }
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
    u->ls_status[0] = u->ls_shown[0] = 0;
    u->text_dirty = 1;
    u->art_seq = 0;
    u->panel = PANEL_NONE;
    u->refused_seen = 0;
    u->lib_scroll = u->lib_show_hits = 0;
    u->lib_seq = 0;
    memset(&u->lib, 0, sizeof u->lib);
    u->query[0] = 0;
}

static void set_screen(Ui *u, Screen s)
{
    u->screen = s;
    u->active = W_NONE;
    u->text_dirty = 1;
    u->redraw = 1;
}

/* Stops everything that talks to Spotify; safe to call in any state. */
static void stop_session(Ui *u)
{
    engine_stop(&u->eng);
    device_stop(&u->dev);
    cmd_stop(&u->cmd);
    art_stop(u->art);
    u->art = NULL;
    if (u->art_tex.id)
        UnloadTexture(u->art_tex);
    u->art_tex = (Texture2D){ 0 };
    poller_stop(u->poller);
    u->poller = NULL;
    librespot_stop(u->ls); /* never leave an orphaned librespot process */
    u->ls = NULL;
}

/* Both sign-in steps are complete here; only now does the player start.
 * A librespot started for step 2 keeps running: restarting it would only
 * repeat its login. */
static void enter_player(Ui *u)
{
    engine_stop(&u->eng);
    reset_player_state(u);
    if (!u->ls)
        u->ls = librespot_start(&u->cfg, u->auth);
    u->poller = poller_start(u->auth, ui_wake, NULL);
    if (!u->poller || cmd_start(&u->cmd, u->auth, u->poller) != 0) {
        stop_session(u);
        u->login_msg = "Could not start the player.";
        u->step = 2;
        set_screen(u, SCR_LOGIN);
        return;
    }
    device_start(&u->dev, u->auth, u->poller, u->ls);
    u->art = art_start(ui_wake, NULL); /* not fatal: the slot stays empty */
    set_screen(u, SCR_PLAYER);
}

/* Next unfinished sign-in step, or the player once both are done. */
static void advance_signin(Ui *u)
{
    u->need_engine = !librespot_has_credentials(&u->cfg);
    if (!auth_store_logged_in(u->auth))
        u->step = 1;
    else if (u->need_engine)
        u->step = 2;
    else {
        enter_player(u);
        return;
    }
    set_screen(u, SCR_LOGIN);
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

/* --- setup screen (no client id) ----------------------------------------- */
static void draw_setup(Ui *u)
{
    float maxw = (float)(u->w - 2 * PAD);
    if (u->text_dirty) {
        char env_path[CFG_PATH_LEN];
        if (plat_path_join(env_path, sizeof env_path, u->cfg.app_dir, ".env") != 0)
            snprintf(env_path, sizeof env_path, ".env");
        wrap_text(u, env_path, FS_S, maxw, u->env_wrapped, sizeof u->env_wrapped);
        u->text_dirty = 0;
    }
    float y = 48;
    text_center(u, "Spotify C Player", y, FS_L, C_TEXT);
    y += 48;
    text(u, "A Spotify client ID is needed.", PAD, y, FS_S, C_TEXT);
    y += 24;
    text(u, "Add this line to the file:", PAD, y, FS_S, C_DIM);
    y += 18;
    text(u, u->env_wrapped, PAD, y, FS_S, C_TEXT);
    y += MeasureTextEx(u->font, u->env_wrapped, FS_S, spacing(FS_S)).y + 16;
    text(u, "SPOTIFY_CLIENT_ID=...", PAD, y, FS_S, C_ACCENT);
    y += 28;
    text(u, "Create the app at developer.spotify.com", PAD, y, FS_S, C_DIM);
    y += 16;
    text(u, "with this redirect URI:", PAD, y, FS_S, C_DIM);
    y += 16;
    text(u, u->cfg.redirect_uri, PAD, y, FS_S, C_TEXT);
    y += 36;

    Rectangle rb = { ((float)u->w - 120.0f) / 2.0f, y, 120, 32 };
    if (button(u, W_RELOAD, rb, "Reload", FS_S, 0)) {
        Config c;
        if (config_load(&c) == 0)
            u->cfg = c; /* no AuthStore exists yet, so nothing holds the old one */
        u->text_dirty = 1;
        if (!u->cfg.client_id[0])
            u->setup_msg = "Still no SPOTIFY_CLIENT_ID in .env.";
        else if (enter_app(u) != 0)
            u->setup_msg = "Out of memory.";
    }
    if (u->setup_msg && u->screen == SCR_SETUP)
        text_center(u, u->setup_msg, y + 48, FS_S, C_ERR);
}

/* --- login screen -------------------------------------------------------- */
/* Called on the login thread just before auth opens the browser. */
static void login_url_cb(const char *url, void *ud)
{
    Ui *u = ud;
    plat_mutex_lock(u->mu);
    snprintf(u->login_url, sizeof u->login_url, "%s", url);
    plat_mutex_unlock(u->mu);
    ui_wake(NULL);
}

static void login_thread(void *arg)
{
    Ui *u = arg;
    int rc = auth_store_login(u->auth, &u->login_cancel);
    plat_mutex_lock(u->mu);
    u->login_done = 1;
    u->login_rc = rc;
    plat_mutex_unlock(u->mu);
    ui_wake(NULL);
}

/* Fallback when the browser tab got lost: open the pending URL again. */
static void open_again(Ui *u, plat_mutex *mu, const char *src)
{
    char url[SIGNIN_URL_LEN];
    plat_mutex_lock(mu);
    memcpy(url, src, sizeof url);
    plat_mutex_unlock(mu);
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

static void start_login(Ui *u)
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

static void start_engine(Ui *u)
{
    u->login_msg = NULL;
    u->eng_has_url = 0;
    u->eng_seq = 0;
    if (!u->ls)
        u->ls = librespot_start(&u->cfg, u->auth);
    if (!u->ls)
        u->login_msg = "Could not start librespot (missing?).";
    else if (engine_start(&u->eng, &u->cfg, u->ls) != 0) {
        stop_session(u);
        u->login_msg = "Could not start sign-in.";
    }
}

/* Step 1: our PKCE login for the Web API. */
static void draw_step1(Ui *u, float y)
{
    if (u->login_th) {
        plat_mutex_lock(u->mu);
        int done = u->login_done, rc = u->login_rc;
        plat_mutex_unlock(u->mu);
        if (done) {
            int cancelled = u->login_cancel;
            login_join(u);
            if (rc == 0) {
                u->login_msg = NULL;
                advance_signin(u);
                return;
            }
            u->login_msg = cancelled ? "Sign-in cancelled." : "Sign-in failed. Please try again.";
        }
    }

    if (u->need_engine)
        text_center(u, "Step 1 of 2", y, FS_S, C_DIM);
    y += 20;
    text_center(u, "Sign in to control your music.", y, FS_S, C_TEXT);
    y += 36;
    Rectangle rb = { ((float)u->w - 200.0f) / 2.0f, y, 200, 40 };
    if (!u->login_th) {
        if (button(u, W_SIGNIN, rb, "Sign in with Spotify", FS_S, 1))
            start_login(u);
        if (u->login_msg)
            text_center(u, u->login_msg, y + 56, FS_S, C_ERR);
        return;
    }
    text_center(u, u->login_cancel ? "Cancelling..." : "Waiting for browser...", y + 12, FS_S, C_TEXT);
    float by = y + 48;
    plat_mutex_lock(u->mu);
    int has_url = u->login_url[0] != 0;
    plat_mutex_unlock(u->mu);
    if (has_url && !u->login_cancel) {
        Rectangle ro = { ((float)u->w - 200.0f) / 2.0f, by, 200, 28 };
        if (button(u, W_OPEN_URL, ro, "Open sign-in page again", FS_S, 0))
            open_again(u, u->mu, u->login_url);
        by += 36;
    }
    Rectangle rc = { ((float)u->w - 100.0f) / 2.0f, by, 100, 28 };
    if (button(u, W_CANCEL, rc, "Cancel", FS_S, 0))
        u->login_cancel = 1;
    if (u->login_msg)
        text_center(u, u->login_msg, by + 40, FS_S, C_ERR);
}

/* Step 2: librespot's own one-time OAuth (it opens the browser itself). */
static void draw_step2(Ui *u, float y)
{
    if (u->eng.th) {
        plat_mutex_lock(u->eng.mu);
        uint64_t seq = u->eng.url_seq;
        EngineState state = u->eng.state;
        int has_url = u->eng.url[0] != 0;
        plat_mutex_unlock(u->eng.mu);
        if (seq != u->eng_seq) {
            u->eng_seq = seq;
            u->eng_has_url = has_url;
            if (state == ENG_DONE) {
                enter_player(u);
                return;
            }
            if (state == ENG_DIED) {
                librespot_status(u->ls, u->ls_status, sizeof u->ls_status);
                fit_text(u, u->ls_status[0] ? u->ls_status : "librespot exited.", FS_S,
                         (float)(u->w - 2 * PAD), u->status, sizeof u->status);
                u->login_msg = u->status;
                stop_session(u);
            }
        }
    }

    text_center(u, "Step 2 of 2", y, FS_S, C_DIM);
    y += 20;
    text_center(u, "Connect the playback engine so this", y, FS_S, C_TEXT);
    y += 16;
    text_center(u, "computer can play the audio itself.", y, FS_S, C_TEXT);
    y += 20;
    Rectangle rb = { ((float)u->w - 200.0f) / 2.0f, y, 200, 40 };
    if (!u->eng.th) {
        if (button(u, W_CONNECT, rb, "Connect playback engine", FS_S, 1))
            start_engine(u);
        if (u->login_msg)
            text_center(u, u->login_msg, y + 56, FS_S, C_ERR);
        return;
    }
    text_center(u, u->eng_has_url ? "Waiting for browser..." : "Starting playback engine...",
                y + 12, FS_S, C_TEXT);
    float by = y + 48;
    if (u->eng_has_url) {
        Rectangle ro = { ((float)u->w - 200.0f) / 2.0f, by, 200, 28 };
        if (button(u, W_OPEN_URL, ro, "Open sign-in page again", FS_S, 0))
            open_again(u, u->eng.mu, u->eng.url);
        by += 36;
    }
    Rectangle rc = { ((float)u->w - 100.0f) / 2.0f, by, 100, 28 };
    if (button(u, W_CANCEL, rc, "Cancel", FS_S, 0)) {
        stop_session(u);
        u->login_msg = "Cancelled.";
    }
    if (u->login_msg)
        text_center(u, u->login_msg, by + 40, FS_S, C_ERR);
}

static void draw_login(Ui *u)
{
    text_center(u, "Spotify C Player", 120, FS_L, C_TEXT);
    if (u->step == 1)
        draw_step1(u, 168);
    else
        draw_step2(u, 168);
}

/* --- player screen ------------------------------------------------------- */
static int progress_at(const PlaybackState *s, uint64_t now)
{
    int64_t p = s->progress_ms;
    if (s->is_playing && now > s->fetched_at_ms)
        p += (int64_t)(now - s->fetched_at_ms);
    if (s->track.duration_ms > 0 && p > s->track.duration_ms)
        p = s->track.duration_ms;
    return p < 0 ? 0 : (int)p;
}

static void pend_reapply(Ui *u)
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
    u->text_dirty = 1;
}

static void pend_begin(Ui *u, int bit, uint64_t now)
{
    u->pend.mask |= bit;
    u->pend.until = now + PENDING_MS;
}

/* Truth wins again: drop optimistic edits and take the poller's state. */
static void resync(Ui *u)
{
    u->pend.mask = 0;
    poller_snapshot(u->poller, &u->st, NULL);
    u->text_dirty = 1;
}

static void refresh_player_text(Ui *u, float maxw, float status_w)
{
    const PlaybackState *s = &u->st;
    if (s->active) {
        fit_text(u, s->track.name[0] ? s->track.name : "Unknown title", FS_L, maxw, u->title, sizeof u->title);
        fit_text(u, s->track.artists, FS_S, maxw, u->artists, sizeof u->artists);
        fit_text(u, s->track.album, FS_S, maxw, u->album, sizeof u->album);
    } else {
        fit_text(u, "Nothing playing", FS_L, maxw, u->title, sizeof u->title);
        fit_text(u, librespot_playback_refused(u->ls) ? "Pick a device with the Devices button."
                                                      : "Press play, or pick this device in Spotify.",
                 FS_S, maxw, u->artists, sizeof u->artists);
        u->album[0] = 0;
    }
    fmt_time(u->t_dur, sizeof u->t_dur, s->track.duration_ms);
    u->pos_sec = -1;

    char raw[SP_NAME_LEN + 64 + sizeof u->ls_shown];
    int err = u->cmd_err ? u->cmd_err : u->poll_err;
    snprintf(raw, sizeof raw, "%s%s%s%s%s",
             s->active && s->device_name[0] ? s->device_name : "No active device",
             err ? " | " : "", err ? sp_strerror(err) : "",
             u->ls_shown[0] ? " | " : "", u->ls_shown);
    fit_two_lines(u, raw, FS_S, status_w, u->status, sizeof u->status);
    u->text_dirty = 0;
}

/* New image bytes: decode once, shrink to the slot, upload, drop the CPU
 * copy. Frames then only draw the texture. Empty bytes clear the slot. */
static void update_art(Ui *u, float slot_px)
{
    const unsigned char *data;
    size_t len;
    if (!art_take(u->art, &data, &len, &u->art_seq))
        return;
    if (u->art_tex.id)
        UnloadTexture(u->art_tex);
    u->art_tex = (Texture2D){ 0 };
    const char *ext = NULL;
    if (len > 2 && data[0] == 0xFF && data[1] == 0xD8)
        ext = ".jpg";
    else if (len > 4 && !memcmp(data, "\x89PNG", 4))
        ext = ".png";
    if (!ext || len > (size_t)INT_MAX)
        return;
    Image img = LoadImageFromMemory(ext, data, (int)len);
    if (!img.data)
        return;
    int side = (int)slot_px;
    if (side > 0 && (img.width > side || img.height > side))
        ImageResize(&img, side, side);
    u->art_tex = LoadTextureFromImage(img);
    UnloadImage(img);
}

/* Pull new panel data from the command worker; copies only on change. */
static void sync_library(Ui *u)
{
    plat_mutex_lock(u->cmd.mu);
    if (u->cmd.lib_seq != u->lib_seq) {
        u->lib_seq = u->cmd.lib_seq;
        u->lib = u->cmd.lib;
    }
    plat_mutex_unlock(u->cmd.mu);
}

/* Remove the last UTF-8 character of s. */
static void pop_utf8(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && ((unsigned char)s[--n] & 0xC0) == 0x80)
        ;
    s[n] = 0;
}

/* Playlists and track search over the player area; rows reuse button(). */
static void draw_library(Ui *u, Rectangle area)
{
    sync_library(u);
    if (IsKeyPressed(KEY_ESCAPE)) {
        u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    size_t qlen = strlen(u->query);
    for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed()) {
        int n = 0;
        const char *bytes = CodepointToUTF8(cp, &n);
        if (n > 0 && qlen + (size_t)n < sizeof u->query) {
            memcpy(u->query + qlen, bytes, (size_t)n);
            qlen += (size_t)n;
            u->query[qlen] = 0;
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
        pop_utf8(u->query);
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && u->query[0]) {
        cmd_post_str(&u->cmd, CMD_SEARCH, 0, u->query);
        u->lib_show_hits = 1;
        u->lib_scroll = 0;
    }

    float bw = 72, row_h = 28, gap = 6;
    Rectangle box = { area.x, area.y, area.width - bw - gap, row_h };
    Rectangle back = { box.x + box.width + gap, area.y, bw, row_h };
    char shown[SP_NAME_LEN + 2];
    DrawRectangleRounded(box, 0.3f, 6, C_PANEL);
    if (u->query[0]) {
        /* Static caret: a blinking one would need timed redraws. */
        snprintf(shown, sizeof shown, "%s_", u->query);
        float tw = text_w(u, shown, FS_S), room = box.width - 16;
        BeginScissorMode((int)box.x + 8, (int)box.y, (int)room, (int)box.height);
        text(u, shown, box.x + 8 + (tw > room ? room - tw : 0), box.y + 9, FS_S, C_TEXT);
        EndScissorMode();
    } else {
        text(u, "Type to search tracks, Enter", box.x + 8, box.y + 9, FS_S, C_DIM);
    }
    if (button(u, W_LIB_BACK, back, "Playlists", FS_S, !u->lib_show_hits)) {
        u->lib_show_hits = 0;
        u->lib_scroll = 0;
        if (!u->lib.pl_loaded)
            cmd_post(&u->cmd, CMD_PLAYLISTS, 0);
    }

    int hits = u->lib_show_hits;
    int n = hits ? u->lib.nhits : u->lib.npl;
    float y = area.y + row_h + 10;
    const char *empty = hits ? (u->lib.searched ? "No results." : "Searching...")
                             : (u->lib.pl_loaded ? "No playlists." : "Loading playlists...");
    if (n == 0) {
        text(u, empty, area.x, y + 4, FS_S, C_DIM);
        return;
    }
    int visible = (int)((area.y + area.height - y + gap) / (row_h + gap));
    if (visible < 1)
        visible = 1;
    float wheel = GetMouseWheelMove();
    u->lib_scroll += wheel > 0 ? -1 : wheel < 0 ? 1 : 0;
    if (u->lib_scroll > n - visible)
        u->lib_scroll = n - visible;
    if (u->lib_scroll < 0)
        u->lib_scroll = 0;
    for (int i = u->lib_scroll; i < n && i < u->lib_scroll + visible; i++, y += row_h + gap) {
        Rectangle r = { area.x, y, area.width, row_h };
        const char *name = hits ? u->lib.hits[i].name : u->lib.pl[i].name;
        const char *uri = hits ? u->lib.hits[i].uri : u->lib.pl[i].uri;
        char sub[SP_NAME_LEN], fit[SP_NAME_LEN];
        if (hits)
            snprintf(sub, sizeof sub, "%s", u->lib.hits[i].artists);
        else
            snprintf(sub, sizeof sub, "%d tracks", u->lib.pl[i].track_count);
        if (button(u, W_LIB_ROW + i, r, NULL, 0, 0) && uri[0])
            cmd_post_str(&u->cmd, CMD_PLAY_URI, hits, uri);
        float sw = text_w(u, sub, FS_S);
        fit_text(u, name, FS_S, r.width - sw - 28, fit, sizeof fit);
        text(u, fit, r.x + 8, r.y + 9, FS_S, C_TEXT);
        text(u, sub, r.x + r.width - 8 - sw, r.y + 9, FS_S, C_DIM);
    }
}

/* Spotify Connect devices; a click moves playback there via the worker. */
static void draw_devices(Ui *u, Rectangle area)
{
    sync_library(u);
    if (IsKeyPressed(KEY_ESCAPE)) {
        u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    float row_h = 28, gap = 6, bw = 72;
    text(u, "Play on", area.x, area.y + 9, FS_S, C_TEXT);
    Rectangle refresh = { area.x + area.width - bw, area.y, bw, row_h };
    if (button(u, W_DEV_REFRESH, refresh, "Refresh", FS_S, 0))
        cmd_post(&u->cmd, CMD_DEVICES, 0);

    float y = area.y + row_h + 10;
    if (u->lib.ndev == 0) {
        text(u, u->lib.devs_loaded ? "No devices. Open Spotify on a phone or computer."
                                   : "Loading devices...", area.x, y + 4, FS_S, C_DIM);
        return;
    }
    int refused = librespot_playback_refused(u->ls);
    for (int i = 0; i < u->lib.ndev && y + row_h <= area.y + area.height; i++, y += row_h + gap) {
        const Device *d = &u->lib.devs[i];
        Rectangle r = { area.x, y, area.width, row_h };
        if (button(u, W_LIB_ROW + i, r, NULL, 0, d->is_active) && d->id[0]) {
            cmd_post_str(&u->cmd, CMD_TRANSFER, 0, d->id);
            u->panel = PANEL_NONE;
        }
        const char *sub = d->is_active ? "active"
                        : refused && strcmp(d->name, CFG_DEVICE_NAME) == 0 ? "can't play"
                        : d->type;
        char fit[SP_NAME_LEN];
        float sw = text_w(u, sub, FS_S);
        fit_text(u, d->name, FS_S, r.width - sw - 28, fit, sizeof fit);
        text(u, fit, r.x + 8, r.y + 9, FS_S, C_TEXT);
        text(u, sub, r.x + r.width - 8 - sw, r.y + 9, FS_S, d->is_active ? C_TEXT : C_DIM);
    }
}

static void draw_player(Ui *u)
{
    uint64_t now = plat_now_ms();
    PlaybackState snap;

    int bg = IsWindowMinimized() || !IsWindowFocused();
    if (bg != u->background) {
        u->background = bg;
        poller_set_background(u->poller, bg);
    }

    if (poller_snapshot(u->poller, &snap, &u->seq)) {
        apply_snapshot(u, &snap, now);
        art_request(u->art, snap.track.art_url); /* no-op unless the URL changed */
    }
    if (u->pend.mask && now >= u->pend.until)
        resync(u);

    plat_mutex_lock(u->cmd.mu);
    uint64_t cseq = u->cmd.err_seq;
    int cerr = u->cmd.err;
    plat_mutex_unlock(u->cmd.mu);
    if (cseq != u->cmd_seq) {
        u->cmd_seq = cseq;
        if (cerr != u->cmd_err)
            u->text_dirty = 1;
        u->cmd_err = cerr;
        if (cerr != SP_OK)
            resync(u);
    }
    int perr = poller_last_error(u->poller);
    if (perr != u->poll_err) {
        u->poll_err = perr;
        u->text_dirty = 1;
    }
    if (u->ls)
        librespot_status(u->ls, u->ls_status, sizeof u->ls_status);
    else
        snprintf(u->ls_status, sizeof u->ls_status, "librespot not running");
    if (strcmp(u->ls_status, u->ls_shown) != 0) {
        memcpy(u->ls_shown, u->ls_status, sizeof u->ls_shown);
        u->text_dirty = 1;
    }

    /* Layout (logical pixels). The art square takes the slack height. */
    float w = (float)u->w, h = (float)u->h, cw = w - 2 * PAD;
    float art = h - 300.0f;
    if (art > cw)
        art = cw;
    if (art < 64.0f)
        art = 64.0f;
    Rectangle logout = { w - PAD - 64, h - PAD - 20, 64, 20 };
    Rectangle devices = { PAD, logout.y, 64, 20 };
    Rectangle library = { devices.x + devices.width + 8, logout.y, 64, 20 };
    float status_y = logout.y - 28; /* up to two lines */
    if (u->text_dirty)
        refresh_player_text(u, cw, cw);

    int refused = librespot_playback_refused(u->ls);
    if (refused && !u->refused_seen) {
        /* Once per session: this account can only drive other devices. */
        u->refused_seen = 1;
        plat_mutex_lock(u->cmd.mu);
        u->cmd.no_local = 1;
        plat_mutex_unlock(u->cmd.mu);
        u->panel = PANEL_DEVICES;
        cmd_post(&u->cmd, CMD_DEVICES, 0);
        u->text_dirty = 1;
        u->redraw = 1;
    }

    text(u, u->status, PAD, status_y, FS_S, refused || u->cmd_err || u->poll_err ? C_ERR : C_DIM);
    if (button(u, W_LOGOUT, logout, "Log out", FS_S, 0)) {
        auth_store_logout(u->auth);
        stop_session(u);
        u->login_msg = NULL;
        advance_signin(u);
        return;
    }
    if (button(u, W_DEVICES, devices, "Devices", FS_S, u->panel == PANEL_DEVICES)) {
        u->panel = u->panel == PANEL_DEVICES ? PANEL_NONE : PANEL_DEVICES;
        if (u->panel == PANEL_DEVICES)
            cmd_post(&u->cmd, CMD_DEVICES, 0); /* devices come and go: always refresh */
    }
    if (button(u, W_LIBRARY, library, "Library", FS_S, u->panel == PANEL_LIBRARY)) {
        u->panel = u->panel == PANEL_LIBRARY ? PANEL_NONE : PANEL_LIBRARY;
        if (u->panel == PANEL_LIBRARY && !u->lib.pl_loaded)
            cmd_post(&u->cmd, CMD_PLAYLISTS, 0);
    }
    Rectangle panel = { PAD, PAD, cw, status_y - 8 - PAD };
    if (u->panel == PANEL_LIBRARY) {
        draw_library(u, panel);
        return;
    }
    if (u->panel == PANEL_DEVICES) {
        draw_devices(u, panel);
        return;
    }

    float y = PAD;
    Rectangle art_r = { (w - art) / 2.0f, y, art, art };
    update_art(u, art * GetWindowScaleDPI().x);
    DrawRectangleRounded(art_r, 0.03f, 8, C_PANEL);
    if (u->art_tex.id)
        DrawTexturePro(u->art_tex, (Rectangle){ 0, 0, (float)u->art_tex.width, (float)u->art_tex.height },
                       art_r, (Vector2){ 0, 0 }, 0.0f, WHITE);
    DrawRectangleRoundedLinesEx(art_r, 0.03f, 8, 1.0f, C_HOVER);
    y += art + 14;

    text(u, u->title, PAD, y, FS_L, C_TEXT);
    y += 26;
    text(u, u->artists, PAD, y, FS_S, C_TEXT);
    y += 16;
    text(u, u->album, PAD, y, FS_S, C_DIM);
    y += 22;

    PlaybackState *s = &u->st;
    int dur = s->track.duration_ms;
    int can_seek = s->active && dur > 0;
    Rectangle bar = { PAD, y, cw, 6 };
    if (u->active != W_SEEK)
        u->seek_t = can_seek ? (float)progress_at(s, now) / (float)dur : 0.0f;
    if (slider(u, W_SEEK, bar, &u->seek_t, can_seek)) {
        u->pend.progress_ms = (int)(u->seek_t * (float)dur);
        u->pend.progress_at = now;
        pend_begin(u, PEND_SEEK, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_SEEK, u->pend.progress_ms);
    }
    int pos = can_seek ? (int)(u->seek_t * (float)dur) : 0;
    if (pos / 1000 != u->pos_sec) {
        u->pos_sec = pos / 1000;
        fmt_time(u->t_pos, sizeof u->t_pos, pos);
    }
    y += 12;
    text(u, u->t_pos, PAD, y, FS_S, C_DIM);
    text(u, u->t_dur, w - PAD - text_w(u, u->t_dur, FS_S), y, FS_S, C_DIM);
    y += 22;

    Rectangle rprev = { w / 2.0f - 28 - 16 - 44, y + 2, 44, 44 };
    Rectangle rplay = { w / 2.0f - 28, y, 56, 48 };
    Rectangle rnext = { w / 2.0f + 28 + 16, y + 2, 44, 44 };
    if (button(u, W_PREV, rprev, NULL, 0, 0)) {
        u->pend.mask &= ~PEND_SEEK;
        cmd_post(&u->cmd, CMD_PREV, 0);
    }
    if (button(u, W_PLAY, rplay, NULL, 0, 1)) {
        int play = !(s->active && s->is_playing);
        if (play && librespot_playback_refused(u->ls) &&
            (!s->active || strcmp(s->device_name, CFG_DEVICE_NAME) == 0)) {
            /* Playing here can't work: offer the other devices instead. */
            u->panel = PANEL_DEVICES;
            cmd_post(&u->cmd, CMD_DEVICES, 0);
            return;
        }
        u->pend.playing = play;
        u->pend.progress_ms = progress_at(s, now);
        u->pend.progress_at = now;
        pend_begin(u, PEND_PLAY, now);
        pend_reapply(u);
        cmd_post(&u->cmd, play ? CMD_PLAY : CMD_PAUSE, 0);
    }
    if (button(u, W_NEXT, rnext, NULL, 0, 0)) {
        u->pend.mask &= ~PEND_SEEK;
        cmd_post(&u->cmd, CMD_NEXT, 0);
    }
    icon(ICON_PREV, rprev, C_TEXT);
    icon(s->active && s->is_playing ? ICON_PAUSE : ICON_PLAY, rplay, C_TEXT);
    icon(ICON_NEXT, rnext, C_TEXT);
    y += 56;

    static const char *const repeat_label[] = { "Repeat: off", "Repeat: all", "Repeat: one" };
    float half = (cw - 8) / 2.0f;
    Rectangle rshuf = { PAD, y, half, 28 };
    Rectangle rrep = { PAD + half + 8, y, half, 28 };
    if (button(u, W_SHUFFLE, rshuf, "Shuffle", FS_S, s->shuffle)) {
        u->pend.shuffle = !s->shuffle;
        pend_begin(u, PEND_SHUFFLE, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_SHUFFLE, u->pend.shuffle);
    }
    unsigned rep = (unsigned)s->repeat <= REPEAT_TRACK ? (unsigned)s->repeat : 0;
    if (button(u, W_REPEAT, rrep, repeat_label[rep], FS_S, rep != REPEAT_OFF)) {
        u->pend.repeat = (RepeatMode)((rep + 1) % 3);
        pend_begin(u, PEND_REPEAT, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_REPEAT, (int)u->pend.repeat);
    }
    y += 42;

    text(u, "Vol", PAD, y - 2, FS_S, C_DIM);
    Rectangle rvol = { PAD + 32, y, cw - 32, 6 };
    if (u->active != W_VOLUME)
        u->vol_t = s->volume >= 0 ? (float)s->volume / 100.0f : 0.0f;
    if (slider(u, W_VOLUME, rvol, &u->vol_t, s->active)) {
        u->pend.volume = (int)(u->vol_t * 100.0f + 0.5f);
        pend_begin(u, PEND_VOLUME, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_VOLUME, u->pend.volume);
    }
}

/* --- main loop ----------------------------------------------------------- */
static void frame(Ui *u)
{
    int w = GetScreenWidth(), h = GetScreenHeight();
    if (w != u->w || h != u->h) {
        u->w = w;
        u->h = h;
        u->text_dirty = 1;
    }
    BeginDrawing();
    ClearBackground(C_BG);
    switch (u->screen) {
    case SCR_SETUP:  draw_setup(u); break;
    case SCR_LOGIN:  draw_login(u); break;
    case SCR_PLAYER: draw_player(u); break;
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        u->active = W_NONE;
    /* Posted before EndDrawing so its wait returns at once and the next
     * frame shows the new state. */
    if (u->redraw) {
        u->redraw = 0;
        ui_wake(NULL);
    }
    EndDrawing();
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

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
    InitWindow(WIN_W, WIN_H, "Spotify C Player");
    if (!IsWindowReady()) {
        plat_mutex_free(u->mu);
        return 1;
    }
    SetWindowMinSize(MIN_W, MIN_H);
    SetExitKey(KEY_NULL);
    SetTextLineSpacing(4);
    EnableEventWaiting();
    u->font = GetFontDefault();

    int rc = 0;
    if (!u->cfg.client_id[0])
        set_screen(u, SCR_SETUP);
    else if (enter_app(u) != 0)
        rc = 1;

    while (rc == 0 && !WindowShouldClose())
        frame(u);

    /* Every thread that could call ui_wake is joined before the window goes. */
    stop_session(u);
    login_join(u);
    auth_store_free(u->auth);
    plat_mutex_free(u->mu);
    CloseWindow();
    return rc;
}
