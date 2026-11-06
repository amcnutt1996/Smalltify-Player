/* raylib GUI: setup notice -> sign-in -> Soloist key / download / pairing
 * -> player. The loop sleeps inside glfwWaitEvents (EnableEventWaiting) and
 * background threads wake it with glfwPostEmptyEvent, so an idle window
 * costs no CPU. All network work runs on worker threads; the UI thread only
 * copies snapshots and draws. */
#include "ui.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "raylib.h"

#include "art.h"
#include "auth.h"
#include "http.h"
#include "model.h"
#include "platform.h"
#include "poller.h"
#include "soloist.h"
#include "spotify.h"

#include "inter_subset.h" /* inter_ranges, inter_blanks, INTER_HEIGHT */

/* Part of the GLFW bundled inside libraylib.a; raylib has no wrapper for it.
 * GLFW documents it as callable from any thread. */
void glfwPostEmptyEvent(void);

/* The GUI font, third_party/inter/Inter-Regular.subset.ttf, compiled in by
 * CMakeLists.txt (ui_font.c in the build folder). */
extern const unsigned char ui_font_ttf[];
extern const unsigned int ui_font_ttf_len;

#define WIN_W      360
#define WIN_ART    220 /* album art side in a new window */
#define MIN_W      320
#define MIN_H      440
#define PAD        16
#define LINE_GAP   4   /* between the lines of one text */
#define BAR_GAP    8   /* between bottom-bar buttons */
#define BAR_MARGIN 8   /* narrowest label margin in a bottom-bar button */
#define GLYPH_PAD  2   /* atlas texels around each glyph: filtering stays inside */
#define ELLIPSIS   "\xE2\x80\xA6" /* U+2026 */

/* Text styles: body text and titles. Their sizes follow the font-size
 * setting (font_sizes, Ui.m). */
enum { FS_S, FS_L };

/* Body and title text in logical pixels for each CFG_FONT_* setting. */
static const struct {
    int         body, title;
    const char *label;
} font_sizes[CFG_FONT_SIZES] = {
    { 12, 19, "Small" }, { 14, 22, "Default" }, { 16, 25, "Large" }, { 19, 29, "Extra large" },
};

#define BAR_BUTTONS 4
static const char *const bar_label[BAR_BUTTONS] = { "Devices", "Library", "Settings", "Log out" };

/* Optimistic edits survive contradicting snapshots this long: right after a
 * command Spotify often still reports the old state. */
#define PENDING_MS        3000
#define DEVICE_WAIT_TRIES 15
#define DEVICE_FIRST_MS   1500 /* Soloist needs a moment to register */
#define DEVICE_RETRY_MS   1000
#define PAIR_FAST_MS      2000   /* pairing: look for our device this often... */
#define PAIR_FAST_FOR_MS  120000 /* ...for the first 2 minutes, */
#define PAIR_SLOW_MS      15000  /* then (or while minimized) this often */
#ifndef PAIR_IDLE_STOP_MS
/* Unpaired Soloist stops when no other device of the account has been
 * listed this long (overridable at compile time for tests). */
#define PAIR_IDLE_STOP_MS 600000
#endif
#define IDLE_WAIT_MS      3600000u
#define SIGNIN_URL_LEN    4096 /* authorize URLs with many scopes are long */

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
    Device   devs[SP_MAX_DEVICES];
    int      ndev, devs_loaded;
} Library;

typedef struct {
    AuthStore   *auth;
    Poller      *poller;
    const char  *name;   /* our device name */
    plat_mutex  *mu;
    plat_event  *ev;
    plat_thread *th;
    /* Guarded by mu. */
    Cmd      slot;
    int      has;
    int      stop;
    int      local;   /* our Soloist device is running and paired */
    int      err;     /* result of the last command */
    char     played_on[SP_NAME_LEN]; /* "" unless it played on another, inactive device */
    uint64_t err_seq; /* bumped after every command */
    Library  lib;     /* latest playlists/search results */
    uint64_t lib_seq; /* bumped when lib changes */
} CmdWorker;

static int fetch_devices(CmdWorker *w, HttpClient *h)
{
    static Device devs[SP_MAX_DEVICES]; /* worker-thread only */
    int n = 0;
    int rc = sp_get_devices(h, devs, SP_MAX_DEVICES, &n);
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

/* Play (uri NULL = resume): on our own device when it is paired and
 * running and a uri was picked (or nothing else is active), else on the
 * active device, else on any available one (said in played_on). */
static int play(CmdWorker *w, HttpClient *h, const char *uri, int is_track)
{
    plat_mutex_lock(w->mu);
    int local = w->local;
    plat_mutex_unlock(w->mu);
    Device on;
    int where = SP_ON_ACTIVE;
    int rc = sp_play_smart(h, uri, is_track, local ? w->name : NULL, local && uri, &on, &where);
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
        return play(w, h, NULL, 0);
    case CMD_PAUSE:   return sp_pause(h);
    case CMD_NEXT:    return sp_next(h);
    case CMD_PREV:    return sp_prev(h);
    case CMD_SEEK:    return sp_seek(h, c->arg);
    case CMD_VOLUME:  return sp_set_volume(h, c->arg);
    case CMD_SHUFFLE: return sp_set_shuffle(h, c->arg);
    case CMD_REPEAT:  return sp_set_repeat(h, (RepeatMode)c->arg);
    case CMD_PLAY_URI:
        return play(w, h, c->str, c->arg);
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

/* --- device worker ------------------------------------------------------- */
/* Transfer: one shot at engine start, make our device the active one when
 * nothing else plays. Pair: poll the device list until our device shows
 * up, which is the only sign that the user picked it in a Spotify app;
 * meanwhile report another device of the account to name in the hint,
 * and give up once none has been listed for PAIR_IDLE_STOP_MS. */
typedef struct {
    AuthStore   *auth;
    Poller      *poller;
    const char  *name;      /* our device name */
    int          pair;
    volatile int slow;      /* set by the UI: minimized */
    volatile int found;     /* pair: our device is listed */
    volatile int idle_out;  /* pair: no other device for too long */
    plat_mutex  *mu;
    char         other[SP_NAME_LEN]; /* pair: another device, guarded by mu */
    plat_event  *stop;      /* signaled = give up now */
    plat_thread *th;
} DeviceWorker;

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
                    ui_wake(NULL);
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
                    ui_wake(NULL);
            }
        }
        if (now - last_other >= PAIR_IDLE_STOP_MS) {
            d->idle_out = 1;
            ui_wake(NULL);
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

/* --- Soloist install worker: check the build, download it if needed ------ */
#define ERR_LEN 256

typedef struct {
    const Config *cfg;
    int           force;  /* re-download even if the build is fine */
    volatile int  cancel;
    plat_mutex   *mu;
    plat_thread  *th;
    /* Guarded by mu. */
    int           downloading, percent, done, rc;
    char          err[ERR_LEN];
} Installer;

static void install_progress(void *ud, int percent)
{
    Installer *in = ud;
    plat_mutex_lock(in->mu);
    in->percent = percent;
    plat_mutex_unlock(in->mu);
    ui_wake(NULL);
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
        ui_wake(NULL);
        rc = soloist_install(in->cfg, install_progress, in, &in->cancel, err, sizeof err);
    }
    plat_mutex_lock(in->mu);
    in->done = 1;
    in->rc = rc;
    memcpy(in->err, err, sizeof in->err);
    plat_mutex_unlock(in->mu);
    ui_wake(NULL);
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

/* --- UI state ------------------------------------------------------------ */
typedef enum { SCR_MOVE, SCR_SETUP, SCR_LOGIN, SCR_KEY, SCR_PLAYER } Screen;
typedef enum { PANEL_NONE, PANEL_LIBRARY, PANEL_DEVICES, PANEL_SETTINGS } Panel;

/* Widget ids for the one widget that may hold the mouse. */
enum {
    W_NONE, W_RELOAD, W_SIGNIN, W_CANCEL, W_OPEN_URL, W_OPEN_KEY, W_BACK, W_CHANGE_KEY,
    W_RESTART, W_HINT_CLOSE,
    W_PREV, W_PLAY, W_NEXT, W_SEEK, W_VOLUME, W_SHUFFLE, W_REPEAT, W_LOGOUT,
    W_LIBRARY, W_LIB_BACK, W_DEVICES, W_DEV_REFRESH, W_SETTINGS,
    W_LIB_ROW /* + row index; rows of two panels never show together */
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

/* Text sizes and the geometry that follows from them, in logical pixels. */
typedef struct {
    float dpi;   /* window scale the fonts were rasterized at; 0 = none yet */
    int   size;  /* CFG_FONT_* they were made for */
    int   fs;    /* body text */
    int   fl;    /* titles */
    float line;  /* pitch of one-line texts stacked under each other */
    float row;   /* list rows, text fields, most buttons */
    float bar;   /* bottom-bar buttons */
    float min_w; /* narrowest window: the bottom bar at BAR_MARGIN */
} Metrics;

typedef struct {
    Config     cfg;
    AuthStore *auth;
    Screen     screen;
    Font       font, font_l; /* body text and titles, made by apply_font */
    Metrics    m;
    int        active;   /* widget holding the mouse, W_NONE if free */
    int        redraw;   /* state changed mid-frame: draw one more frame */
    int        w, h;
    int        min_w, min_h; /* window minimum last set */
    float      need_h;   /* height the screen drawn last needs */
    int        text_dirty;
    int        size_unsaved; /* Settings could not write UI_FONT_SIZE */

    /* setup and key screens: <app>/.env, wrapped to the window */
    char env_wrapped[CFG_PATH_LEN + 64];
    const char *setup_msg;

    /* sign-in screen: our PKCE login */
    plat_mutex  *mu;
    plat_thread *login_th;
    volatile int login_cancel;
    int          login_done, login_rc;     /* guarded by mu */
    char         login_url[SIGNIN_URL_LEN]; /* guarded by mu */
    const char  *login_msg;

    /* Soloist, run from the player: always once paired, else only while
     * another device of the account is online so it can pick us */
    Installer    inst;
    Soloist     *sol;
    int          paired;         /* data/soloist-paired exists */
    int          sol_was_paired; /* paired when this Soloist started */
    int          transferred;    /* startup transfer tried for this Soloist */
    int          auto_failed;    /* an unpaired start failed: not again this run */
    int          reinstalled;    /* an expired build was replaced once */
    int          hint_hidden;    /* pairing tip dismissed this run */
    char         other_dev[SP_NAME_LEN]; /* another device of the account, for the tip */
    char         eng_err[ERR_LEN]; /* "" = no error to show */

    /* player screen */
    Poller       *poller;
    CmdWorker     cmd;
    DeviceWorker  dev;
    PlaybackState st;
    uint64_t      seq;
    Pending       pend;
    uint64_t      cmd_seq;
    int           cmd_err, poll_err, background;
    int           status_red; /* u->status is an error */
    char          played_on[SP_NAME_LEN];
    float         seek_t, vol_t; /* slider positions while dragged */
    char msg[320], msg_shown[320]; /* the one status message, see status_message */
    char hint[320], hint_fit[320]; /* pairing tip; "" = none */
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
    int      lib_scroll;
    int      lib_show_hits; /* list search results instead of playlists */
    uint64_t lib_seq;
    uint64_t dev_seq_noted; /* cmd.lib_seq last checked by note_other */
    Library  lib;
    char     query[SP_NAME_LEN];
    char     key_input[sizeof(((Config *)0)->soloist_key)]; /* key screen; secret */
} Ui;

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
    m.bar = (float)m.fs + 10;
    return m;
}

static const Font *font_of(const Ui *u, int fs)
{
    return fs == FS_L ? &u->font_l : &u->font;
}

static float px_of(const Ui *u, int fs)
{
    return (float)(fs == FS_L ? u->m.fl : u->m.fs);
}

/* No extra letter spacing: the glyph advances alone make a text's width,
 * so the width of a string is the sum of its characters' widths. */
static float text_w(const Ui *u, const char *s, int fs)
{
    return MeasureTextEx(*font_of(u, fs), s, px_of(u, fs), 0.0f).x;
}

/* Height of s, body text that may hold line breaks. */
static float text_h(const Ui *u, const char *s)
{
    return MeasureTextEx(u->font, s, (float)u->m.fs, 0.0f).y;
}

/* v on the screen's pixel grid: the fonts are made at the window scale, so
 * each glyph texel then covers exactly one pixel and text stays sharp. */
static float snap(const Ui *u, float v)
{
    return roundf(v * u->m.dpi) / u->m.dpi;
}

static void text(const Ui *u, const char *s, float x, float y, int fs, Color c)
{
    DrawTextEx(*font_of(u, fs), s, (Vector2){ snap(u, x), snap(u, y) }, px_of(u, fs), 0.0f, c);
}

static void text_center(const Ui *u, const char *s, float y, int fs, Color c)
{
    text(u, s, ((float)u->w - text_w(u, s, fs)) / 2.0f, y, fs, c);
}

/* Top of one line of body text centered vertically in r. */
static float mid_y(const Ui *u, Rectangle r)
{
    return r.y + (r.height - (float)u->m.fs) / 2.0f;
}

/* The font at px logical pixels, rasterized at window scale dpi. What
 * LoadFontFromMemory does, minus the CPU copy of every glyph it keeps for
 * ImageDrawText, and with the blank glyphs' widths that raylib drops. */
static Font load_font(int px, float dpi)
{
    int cps[INTER_CODEPOINTS], n = 0;
    for (size_t r = 0; r < sizeof inter_ranges / sizeof inter_ranges[0]; r++)
        for (int c = inter_ranges[r][0]; c <= inter_ranges[r][1] && n < INTER_CODEPOINTS; c++)
            cps[n++] = c;
    Font f = { 0 };
    f.baseSize = (int)lroundf((float)px * dpi);
    f.glyphCount = n;
    f.glyphPadding = GLYPH_PAD;
    /* raylib warns about every glyph taller than the line ('|', U+2016,
     * U+2295...); the skyline packer below gives them their full height. */
    SetTraceLogLevel(LOG_ERROR);
    f.glyphs = LoadFontData(ui_font_ttf, (int)ui_font_ttf_len, f.baseSize, cps, n, FONT_DEFAULT);
    SetTraceLogLevel(LOG_WARNING); /* ui_run's level */
    if (!f.glyphs)
        return GetFontDefault();
    Image atlas = GenImageFontAtlas(f.glyphs, &f.recs, n, f.baseSize, GLYPH_PAD, 1);
    f.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
    for (int i = 0; i < n; i++) {
        UnloadImage(f.glyphs[i].image);
        f.glyphs[i].image = (Image){ 0 };
    }
    for (size_t b = 0; b < sizeof inter_blanks / sizeof inter_blanks[0]; b++) {
        int i = GetGlyphIndex(f, inter_blanks[b][0]);
        if (f.glyphs[i].value == inter_blanks[b][0])
            f.glyphs[i].advanceX = (int)((float)inter_blanks[b][1] * (float)f.baseSize / INTER_HEIGHT);
    }
    return f;
}

/* Make the fonts for the size setting at the window's scale: at start, and
 * again whenever either changes (Settings, a reloaded .env, a display with
 * another scale). */
static void apply_font(Ui *u)
{
    float dpi = GetWindowScaleDPI().x;
    if (dpi <= 0.0f)
        dpi = 1.0f;
    Metrics m = metrics_for(u->cfg.font_size);
    if (u->m.dpi == dpi && u->m.size == m.size)
        return;
    if (u->m.dpi > 0.0f) {
        UnloadFont(u->font);
        UnloadFont(u->font_l);
    }
    m.dpi = dpi;
    u->m = m;
    u->font = load_font(m.fs, dpi);
    u->font_l = load_font(m.fl, dpi);
    float labels = 0;
    for (int i = 0; i < BAR_BUTTONS; i++)
        labels += text_w(u, bar_label[i], FS_S);
    u->m.min_w = labels + 2 * BAR_BUTTONS * BAR_MARGIN + (BAR_BUTTONS - 1) * BAR_GAP + 2 * PAD;
    u->text_dirty = 1;
}

/* Copy src into out, cut at a UTF-8 boundary with an ellipsis so it fits
 * maxw. Widths add up (see text_w), so one pass finds the cut. */
static void fit_text(const Ui *u, const char *src, int fs, float maxw, char *out, size_t cap)
{
    snprintf(out, cap, "%s", src);
    if (text_w(u, out, fs) <= maxw)
        return;
    float room = maxw - text_w(u, ELLIPSIS, fs), w = 0.0f;
    size_t n = 0;
    while (out[n]) {
        char ch[5];
        size_t len = 1;
        while (len < 4 && ((unsigned char)out[n + len] & 0xC0) == 0x80)
            len++;
        memcpy(ch, out + n, len);
        ch[len] = 0;
        if ((w += text_w(u, ch, fs)) > room)
            break;
        n += len;
    }
    while (n > 0 && n + sizeof ELLIPSIS > cap) { /* no room for the ellipsis */
        do
            n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
    }
    if (n + sizeof ELLIPSIS <= cap)
        memcpy(out + n, ELLIPSIS, sizeof ELLIPSIS);
}

/* Word-wrap src onto at most two lines of maxw; the second is cut with an
 * ellipsis. */
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

/* Word-wrap src into out at the spaces, so lines fit maxw where a word
 * break allows; src's own line breaks stay. */
static void wrap_words(const Ui *u, const char *src, float maxw, char *out, size_t cap)
{
    snprintf(out, cap, "%s", src);
    size_t line = 0, space = 0; /* start of the current line; its last space if > line */
    for (size_t i = 0;; i++) {
        char c = out[i];
        if (c && c != ' ' && c != '\n')
            continue;
        out[i] = 0;
        int over = text_w(u, out + line, FS_S) > maxw;
        out[i] = c;
        if (over && space > line) { /* the word just ended starts a new line */
            out[space] = '\n';
            line = space + 1;
        }
        if (!c)
            return;
        if (c == '\n')
            line = i + 1;
        else
            space = i;
    }
}

/* src in body text, word-wrapped to maxw at (x, y); returns its height. */
static float para(const Ui *u, const char *src, float x, float y, float maxw, Color c)
{
    char buf[512];
    wrap_words(u, src, maxw, buf, sizeof buf);
    text(u, buf, x, y, FS_S, c);
    return text_h(u, buf);
}

/* A message in body text, wrapped to the window and centered line by line. */
static void para_center(const Ui *u, const char *src, float y, Color c)
{
    char buf[512], line[512];
    wrap_words(u, src, (float)(u->w - 2 * PAD), buf, sizeof buf);
    for (const char *s = buf;; y += (float)u->m.fs + LINE_GAP) {
        size_t n = strcspn(s, "\n");
        snprintf(line, sizeof line, "%.*s", (int)n, s);
        text_center(u, line, y, FS_S, c);
        if (!s[n])
            return;
        s += n + 1;
    }
}

/* --- widgets ------------------------------------------------------------- */
static int hovered(Rectangle r)
{
    return CheckCollisionPointRec(GetMousePosition(), r);
}

/* A button width that fits label: minw, or more for the label and a margin
 * of one text height on either side. */
static float btn_w(const Ui *u, const char *label, float minw)
{
    float w = text_w(u, label, FS_S) + 2.0f * (float)u->m.fs;
    return w > minw ? w : minw;
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
        text(u, label, r.x + (r.width - tw) / 2.0f, r.y + (r.height - px_of(u, fs)) / 2.0f, fs, C_TEXT);
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

/* Remove the last UTF-8 character of s. */
static void pop_utf8(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && ((unsigned char)s[--n] & 0xC0) == 0x80)
        ;
    s[n] = 0;
}

/* Append codepoint cp if it fits whole. key_chars keeps only what an API
 * key holds (printable ASCII, no blanks), so a pasted line break or space
 * is dropped. */
static void append_cp(char *buf, size_t cap, size_t *len, int cp, int key_chars)
{
    if (cp < ' ' || cp == 0x7f || (key_chars && (cp == ' ' || cp > '~')))
        return;
    int n = 0;
    const char *bytes = CodepointToUTF8(cp, &n);
    if (n > 0 && *len + (size_t)n < cap) {
        memcpy(buf + *len, bytes, (size_t)n);
        *len += (size_t)n;
        buf[*len] = 0;
    }
}

/* Keyboard input for a one-line field: typing, Backspace, and paste with
 * Cmd+V (macOS) or Ctrl+V. */
static void edit_line(char *buf, size_t cap, int key_chars)
{
    size_t len = strlen(buf);
    for (int cp = GetCharPressed(); cp > 0; cp = GetCharPressed())
        append_cp(buf, cap, &len, cp, key_chars);
    int mod = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
              IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    /* The press queue, not IsKeyPressed: a press and release that arrive in
     * the same event wait never show up as "pressed" in the key state. */
    for (int k = GetKeyPressed(); k > 0; k = GetKeyPressed()) {
        if (k == KEY_BACKSPACE) {
            pop_utf8(buf);
            len = strlen(buf);
        } else if (k == KEY_V && mod) {
            const char *clip = GetClipboardText();
            while (clip && *clip) {
                int step = 1;
                int cp = GetCodepointNext(clip, &step);
                append_cp(buf, cap, &len, cp, key_chars);
                clip += step > 0 ? step : 1;
            }
        }
    }
    if (IsKeyPressedRepeat(KEY_BACKSPACE))
        pop_utf8(buf);
}

/* One-line text box: s with a static caret (a blinking one would need
 * timed redraws), scrolled to its end; the placeholder, cut to fit, when s
 * is empty. */
static void draw_field(const Ui *u, Rectangle box, const char *s, const char *placeholder)
{
    char shown[SP_NAME_LEN + 2];
    float room = box.width - 16, ty = mid_y(u, box);
    DrawRectangleRounded(box, 0.3f, 6, C_PANEL);
    if (s[0]) {
        snprintf(shown, sizeof shown, "%s_", s);
        float tw = text_w(u, shown, FS_S);
        BeginScissorMode((int)box.x + 8, (int)box.y, (int)room, (int)box.height);
        text(u, shown, box.x + 8 + (tw > room ? room - tw : 0), ty, FS_S, C_TEXT);
        EndScissorMode();
    } else {
        fit_text(u, placeholder, FS_S, room, shown, sizeof shown);
        text(u, shown, box.x + 8, ty, FS_S, C_DIM);
    }
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
    u->msg[0] = u->msg_shown[0] = u->hint[0] = 0;
    u->played_on[0] = 0;
    u->text_dirty = 1;
    u->art_seq = 0;
    u->panel = PANEL_NONE;
    u->lib_scroll = u->lib_show_hits = 0;
    u->lib_seq = u->dev_seq_noted = 0;
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
static void stop_session(Ui *u)
{
    u->inst.cancel = 1;
    if (u->dev.stop)
        plat_event_signal(u->dev.stop);
    cmd_signal_stop(&u->cmd);
    art_signal_stop(u->art);
    poller_signal_stop(u->poller);
    stop_engine(u);
    cmd_stop(&u->cmd);
    art_stop(u->art);
    u->art = NULL;
    if (u->art_tex.id)
        UnloadTexture(u->art_tex);
    u->art_tex = (Texture2D){ 0 };
    poller_stop(u->poller);
    u->poller = NULL;
}

/* Check (and if needed download, silently) Soloist on the install worker;
 * the player starts it once that is done (poll_engine). No-op if it is
 * already on its way, or can't run here. */
static void start_engine(Ui *u, int force)
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
    if (!u->poller || cmd_start(&u->cmd, u->auth, u->poller, u->cfg.device_name) != 0) {
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

/* Sign-in first; everything else happens in the player. */
static void advance_signin(Ui *u)
{
    if (!auth_store_logged_in(u->auth))
        set_screen(u, SCR_LOGIN);
    else
        enter_player(u);
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

/* <app>/.env hard-wrapped to the window (a path has no spaces). */
static void wrap_env_path(Ui *u, float maxw)
{
    char env_path[CFG_PATH_LEN];
    if (plat_path_join(env_path, sizeof env_path, u->cfg.app_dir, ".env") != 0)
        snprintf(env_path, sizeof env_path, ".env");
    wrap_text(u, env_path, FS_S, maxw, u->env_wrapped, sizeof u->env_wrapped);
}

/* --- setup screen (no client id) ----------------------------------------- */
static void draw_setup(Ui *u)
{
    const Metrics *m = &u->m;
    float maxw = (float)(u->w - 2 * PAD);
    if (u->text_dirty) {
        wrap_env_path(u, maxw);
        u->text_dirty = 0;
    }
    float y = 48;
    text_center(u, "Spotify C Player", y, FS_L, C_TEXT);
    y += (float)m->fl + 28;
    y += para(u, "A Spotify client ID is needed.", PAD, y, maxw, C_TEXT) + 14;
    y += para(u, "Add this line to the file:", PAD, y, maxw, C_DIM) + 8;
    text(u, u->env_wrapped, PAD, y, FS_S, C_TEXT);
    y += text_h(u, u->env_wrapped) + 16;
    text(u, "SPOTIFY_CLIENT_ID=...", PAD, y, FS_S, C_ACCENT);
    y += (float)m->fs + 18;
    y += para(u, "Create the app at developer.spotify.com\nwith this redirect URI:", PAD, y, maxw,
              C_DIM) + 8;
    text(u, u->cfg.redirect_uri, PAD, y, FS_S, C_TEXT);
    y += (float)m->fs + 26;

    float bw = btn_w(u, "Reload", 120), bh = (float)m->fs + 22;
    Rectangle rb = { ((float)u->w - bw) / 2.0f, y, bw, bh };
    u->need_h = y + bh + 16 + (float)m->fs + PAD; /* a one-line message below */
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
        para_center(u, u->setup_msg, y + bh + 16, C_ERR);
}

/* --- move screen (app folder not writable) -------------------------------- */
static void draw_move(Ui *u)
{
    const Metrics *m = &u->m;
    float y = 120;
    text_center(u, "Spotify C Player", y, FS_L, C_TEXT);
    y += (float)m->fl + 36;
    text_center(u, "This folder is read-only.", y, FS_S, C_ERR);
    y += (float)m->fs + 18;
    text_center(u, "Move Spotify C Player to Applications", y, FS_S, C_TEXT);
    y += m->line;
    text_center(u, "(or another writable folder)", y, FS_S, C_TEXT);
    y += m->line;
    text_center(u, "and open it again.", y, FS_S, C_TEXT);
    u->need_h = y + (float)m->fs + PAD;
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
static void open_again(Ui *u)
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

/* Our PKCE login for the Web API. */
static void draw_login(Ui *u)
{
    const Metrics *m = &u->m;
    text_center(u, "Spotify C Player", 120, FS_L, C_TEXT);
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
            u->login_msg = cancelled ? "Sign-in cancelled." : auth_login_strerror(rc);
        }
    }

    float y = 120 + (float)m->fl + 48;
    text_center(u, "Sign in to control your music.", y, FS_S, C_TEXT);
    y += (float)m->fs + 26;
    float bw = btn_w(u, "Sign in with Spotify", 200), bh = (float)m->fs + 30;
    Rectangle rb = { ((float)u->w - bw) / 2.0f, y, bw, bh };
    if (!u->login_th) {
        if (button(u, W_SIGNIN, rb, "Sign in with Spotify", FS_S, 1))
            start_login(u);
        /* A client ID fixed in .env applies without a restart. */
        float rw = btn_w(u, "Reload .env", 120), rh = (float)m->fs + 14;
        Rectangle rr = { ((float)u->w - rw) / 2.0f, y + bh + 8, rw, rh };
        u->need_h = rr.y + rh + 12 + (float)m->fs + PAD;
        if (button(u, W_RELOAD, rr, "Reload .env", FS_S, 0)) {
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
            return;
        }
        if (u->login_msg)
            para_center(u, u->login_msg, rr.y + rh + 12, C_ERR);
        return;
    }
    text_center(u, u->login_cancel ? "Cancelling..." : "Waiting for browser...", mid_y(u, rb), FS_S,
                C_TEXT);
    float by = y + bh + 8;
    plat_mutex_lock(u->mu);
    int has_url = u->login_url[0] != 0;
    plat_mutex_unlock(u->mu);
    if (has_url && !u->login_cancel) {
        float ow = btn_w(u, "Open sign-in page again", 200);
        Rectangle ro = { ((float)u->w - ow) / 2.0f, by, ow, m->row };
        if (button(u, W_OPEN_URL, ro, "Open sign-in page again", FS_S, 0))
            open_again(u);
        by += m->row + 8;
    }
    float xw = btn_w(u, "Cancel", 100);
    Rectangle rc = { ((float)u->w - xw) / 2.0f, by, xw, m->row };
    u->need_h = by + m->row + 12 + (float)m->fs + PAD;
    if (button(u, W_CANCEL, rc, "Cancel", FS_S, 0))
        u->login_cancel = 1;
    if (u->login_msg)
        para_center(u, u->login_msg, by + m->row + 12, C_ERR);
}

/* --- Soloist key screen -------------------------------------------------- */
#define SOLOIST_KEY_URL "https://developer.spotify.com/dashboard/soloist"

/* Back to the player, which kept running behind this screen. */
static void leave_key(Ui *u)
{
    memset(u->key_input, 0, sizeof u->key_input);
    u->setup_msg = NULL;
    set_screen(u, SCR_PLAYER);
}

/* '*' for all but the last 4 characters: enough to recognise the key. */
static void mask_secret(const char *s, char *out, size_t cap)
{
    size_t n = strlen(s), keep = n > 4 ? 4 : 0, i = 0;
    for (; i + keep < n && i + 1 < cap; i++)
        out[i] = '*';
    snprintf(out + i, cap - i, "%s", s + n - keep);
}

/* Read SOLOIST_API_KEY from .env (or the environment) again. Only the key
 * changes; the AuthStore keeps its own copy of the rest. */
static void reload_key(Ui *u)
{
    Config c;
    if (config_load(&c) == 0) {
        memcpy(u->cfg.soloist_key, c.soloist_key, sizeof u->cfg.soloist_key);
        memset(c.soloist_key, 0, sizeof c.soloist_key);
    }
}

/* Save the pasted key into <app>/.env (if any), then read .env again. */
static void save_key(Ui *u)
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

static void draw_key(Ui *u)
{
    const Metrics *m = &u->m;
    float maxw = (float)(u->w - 2 * PAD);
    if (u->text_dirty) {
        wrap_env_path(u, maxw);
        u->text_dirty = 0;
    }
    /* The one field on this screen always has the keyboard. */
    edit_line(u->key_input, sizeof u->key_input, 1);
    int enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);

    float y = 48;
    text_center(u, "Spotify C Player", y, FS_L, C_TEXT);
    y += (float)m->fl + 28;
    y += para(u, "To play music on this computer, it needs\na Spotify Soloist API key (optional).",
              PAD, y, maxw, C_TEXT) + 16;
    text(u, "1. Generate a key at", PAD, y, FS_S, C_DIM);
    y += m->line;
    text(u, "developer.spotify.com/dashboard/soloist", PAD, y, FS_S, C_TEXT);
    y += (float)m->fs + 8;
    Rectangle ro = { PAD, y, btn_w(u, "Open the page", 120), (float)m->fs + 14 };
    if (button(u, W_OPEN_KEY, ro, "Open the page", FS_S, 0) && plat_open_url(SOLOIST_KEY_URL) != 0)
        u->setup_msg = "Could not open the browser.";
    y += ro.height + 12;
    text(u, "2. Paste it here and press Save:", PAD, y, FS_S, C_DIM);
    y += (float)m->fs + 8;
    float bw = fmaxf(btn_w(u, "Save", 72), btn_w(u, "Reload", 72)), gap = 6;
    Rectangle box = { PAD, y, maxw - bw - gap, m->row };
    Rectangle rb = { PAD + box.width + gap, y, bw, m->row };
    char masked[SP_NAME_LEN];
    mask_secret(u->key_input, masked, sizeof masked);
    draw_field(u, box, masked, "Paste the key (it stays hidden)");
    /* Empty box: re-read a key added to .env by hand. */
    if (button(u, W_RELOAD, rb, u->key_input[0] ? "Save" : "Reload", FS_S, 1) || enter) {
        save_key(u);
        if (u->screen != SCR_KEY)
            return;
    }
    y += m->row + 12;
    y += para(u, "Or put SOLOIST_API_KEY=... in this file\nand press Reload:", PAD, y, maxw, C_DIM) + 6;
    text(u, u->env_wrapped, PAD, y, FS_S, C_TEXT);
    y += text_h(u, u->env_wrapped) + 16;

    float sw = btn_w(u, "Back", 100);
    Rectangle rs = { ((float)u->w - sw) / 2.0f, y, sw, m->row };
    u->need_h = y + m->row + 12 + (float)m->fs + PAD;
    if (button(u, W_BACK, rs, "Back", FS_S, 0)) {
        leave_key(u);
        return;
    }
    if (u->setup_msg)
        para_center(u, u->setup_msg, y + m->row + 12, C_ERR);
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
static void note_other(Ui *u, const char *name)
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
static int local_ready(Ui *u)
{
    SoloistState st = soloist_state(u->sol);
    return u->sol && u->paired && (st == SOLOIST_READY || st == SOLOIST_PAIRING);
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

static void refresh_player_text(Ui *u, float maxw, float status_w, float hint_w)
{
    const PlaybackState *s = &u->st;
    if (s->active) {
        fit_text(u, s->track.name[0] ? s->track.name : "Unknown title", FS_L, maxw, u->title, sizeof u->title);
        fit_text(u, s->track.artists, FS_S, maxw, u->artists, sizeof u->artists);
        fit_text(u, s->track.album, FS_S, maxw, u->album, sizeof u->album);
    } else {
        fit_text(u, "Nothing playing", FS_L, maxw, u->title, sizeof u->title);
        fit_text(u, "Press play, or pick a device with Devices.",
                 FS_S, maxw, u->artists, sizeof u->artists);
        u->album[0] = 0;
    }
    sp_fmt_time(u->t_dur, sizeof u->t_dur, s->track.duration_ms);
    u->pos_sec = -1;

    fit_two_lines(u, u->msg_shown, FS_S, status_w, u->status, sizeof u->status);
    fit_two_lines(u, u->hint, FS_S, hint_w, u->hint_fit, sizeof u->hint_fit);
    u->text_dirty = 0;
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
    int err = u->cmd_err ? u->cmd_err : u->poll_err;
    if (err) {
        snprintf(out, cap, "%s", sp_strerror(err));
        return 1;
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

/* Playlists and track search over the player area; rows reuse button(). */
static void draw_library(Ui *u, Rectangle area)
{
    sync_library(u);
    if (IsKeyPressed(KEY_ESCAPE)) {
        u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    edit_line(u->query, sizeof u->query, 0);
    if ((IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) && u->query[0]) {
        cmd_post_str(&u->cmd, CMD_SEARCH, 0, u->query);
        u->lib_show_hits = 1;
        u->lib_scroll = 0;
    }

    float bw = btn_w(u, "Playlists", 72), row_h = u->m.row, gap = 6;
    Rectangle box = { area.x, area.y, area.width - bw - gap, row_h };
    Rectangle back = { box.x + box.width + gap, area.y, bw, row_h };
    draw_field(u, box, u->query, "Type to search tracks, Enter");
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
        if (hits) /* artists may be long: they get at most about half the row */
            fit_text(u, u->lib.hits[i].artists, FS_S, r.width * 0.45f, sub, sizeof sub);
        else
            snprintf(sub, sizeof sub, "%d tracks", u->lib.pl[i].track_count);
        if (button(u, W_LIB_ROW + i, r, NULL, 0, 0) && uri[0])
            cmd_post_str(&u->cmd, CMD_PLAY_URI, hits, uri);
        float sw = text_w(u, sub, FS_S);
        fit_text(u, name, FS_S, r.width - sw - 28, fit, sizeof fit);
        text(u, fit, r.x + 8, mid_y(u, r), FS_S, C_TEXT);
        text(u, sub, r.x + r.width - 8 - sw, mid_y(u, r), FS_S, C_DIM);
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
    float row_h = u->m.row, gap = 6, bw = btn_w(u, "Refresh", 72);
    Rectangle refresh = { area.x + area.width - bw, area.y, bw, row_h };
    text(u, "Play on", area.x, mid_y(u, refresh), FS_S, C_TEXT);
    if (button(u, W_DEV_REFRESH, refresh, "Refresh", FS_S, 0))
        cmd_post(&u->cmd, CMD_DEVICES, 0);

    float y = area.y + row_h + 10;
    if (soloist_supported() && !u->cfg.soloist_key[0]) {
        /* The only way to the key screen: nobody is nagged for it. */
        Rectangle r = { area.x, y, area.width, row_h };
        if (button(u, W_LIB_ROW + SP_MAX_DEVICES, r, "Enable playback on this computer", FS_S, 0)) {
            u->setup_msg = NULL;
            set_screen(u, SCR_KEY);
            return;
        }
        y += row_h + gap;
    }
    if (u->lib.ndev == 0) {
        para(u, u->lib.devs_loaded ? "No devices. Open Spotify on a phone or computer."
                                   : "Loading devices...", area.x, y + 4, area.width, C_DIM);
        return;
    }
    /* Our own device first (once paired), then Spotify's order. */
    int order[SP_MAX_DEVICES], n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < u->lib.ndev; i++)
            if ((u->paired && strcmp(u->lib.devs[i].name, u->cfg.device_name) == 0) == !pass)
                order[n++] = i;
    for (int k = 0; k < n && y + row_h <= area.y + area.height; k++, y += row_h + gap) {
        int i = order[k];
        const Device *d = &u->lib.devs[i];
        Rectangle r = { area.x, y, area.width, row_h };
        if (button(u, W_LIB_ROW + i, r, NULL, 0, d->is_active) && d->id[0]) {
            cmd_post_str(&u->cmd, CMD_TRANSFER, 0, d->id);
            u->panel = PANEL_NONE;
        }
        int ours = u->paired && strcmp(d->name, u->cfg.device_name) == 0;
        const char *sub = d->is_active ? "active" : ours ? "" : d->type;
        char fit[SP_NAME_LEN];
        float sw = text_w(u, sub, FS_S);
        fit_text(u, ours ? "This computer" : d->name, FS_S, r.width - sw - 28, fit, sizeof fit);
        text(u, fit, r.x + 8, mid_y(u, r), FS_S, C_TEXT);
        text(u, sub, r.x + r.width - 8 - sw, mid_y(u, r), FS_S, d->is_active ? C_TEXT : C_DIM);
    }
}

/* The text size: a pick applies at once, so the window is its own preview,
 * and is saved to .env as UI_FONT_SIZE. */
static void draw_settings(Ui *u, Rectangle area)
{
    if (IsKeyPressed(KEY_ESCAPE)) {
        u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    float row_h = u->m.row, gap = 6;
    text(u, "Text size", area.x, mid_y(u, (Rectangle){ area.x, area.y, area.width, row_h }), FS_S,
         C_TEXT);
    float y = area.y + row_h + 10;
    for (int i = 0; i < CFG_FONT_SIZES && y + row_h <= area.y + area.height; i++, y += row_h + gap) {
        Rectangle r = { area.x, y, area.width, row_h };
        int on = i == u->m.size;
        if (button(u, W_LIB_ROW + i, r, NULL, FS_S, on) && !on) {
            u->cfg.font_size = i; /* the next frame remakes the fonts (apply_font) */
            u->size_unsaved =
                config_set_env_value(&u->cfg, "UI_FONT_SIZE", config_font_size_value(i)) != 0;
        }
        text(u, font_sizes[i].label, r.x + 8, mid_y(u, r), FS_S, C_TEXT);
    }
    if (u->size_unsaved)
        para(u, "Could not save this in .env: it applies until the app quits.", area.x, y + 4,
             area.width, C_ERR);
}

/* Player rows below the art, as offsets from its bottom edge. */
typedef struct {
    float title, artists, album, seek, times, transport, modes, volume, end;
} PlayerRows;

static PlayerRows player_rows(const Metrics *m)
{
    PlayerRows r;
    r.title = 14;
    r.artists = r.title + (float)m->fl + 6;
    r.album = r.artists + m->line;
    r.seek = r.album + (float)m->fs + 12;
    r.times = r.seek + 12;
    r.transport = r.times + (float)m->fs + 12;
    r.modes = r.transport + 56;
    r.volume = r.modes + m->row + 12; /* the "Vol" label; its bar is centered on it */
    r.end = r.volume + (float)m->fs;
    return r;
}

/* Two lines of body text: the status, or the pairing tip. */
static float two_lines(const Metrics *m)
{
    return 2.0f * (float)m->fs + LINE_GAP;
}

/* The player's height without the art: padding, the rows below the art,
 * the status lines and the bottom bar. */
static float player_reserve(const Metrics *m)
{
    return PAD + player_rows(m).end + 4 + two_lines(m) + 4 + m->bar + PAD;
}

static void draw_player(Ui *u)
{
    uint64_t now = plat_now_ms();
    PlaybackState snap;

    /* Only minimized counts as background: a visible window, focused or
     * not, keeps its 1 Hz progress tick. */
    int bg = IsWindowMinimized();
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
    uint64_t cseq = u->cmd.err_seq;
    int cerr = u->cmd.err;
    char played_on[SP_NAME_LEN], other[SP_NAME_LEN] = "";
    memcpy(played_on, u->cmd.played_on, sizeof played_on);
    int fresh = u->cmd.lib_seq != u->dev_seq_noted;
    u->dev_seq_noted = u->cmd.lib_seq;
    if (fresh) /* a fresh device list names another device? */
        for (int i = 0; i < u->cmd.lib.ndev && !other[0]; i++)
            if (u->cmd.lib.devs[i].id[0] && strcmp(u->cmd.lib.devs[i].name, u->cfg.device_name))
                memcpy(other, u->cmd.lib.devs[i].name, sizeof other);
    plat_mutex_unlock(u->cmd.mu);
    note_other(u, other);
    if (cseq != u->cmd_seq) {
        u->cmd_seq = cseq;
        u->cmd_err = cerr;
        memcpy(u->played_on, played_on, sizeof u->played_on);
        u->text_dirty = 1;
        if (cerr != SP_OK)
            resync(u);
    }
    int perr = poller_last_error(u->poller);
    if (perr != u->poll_err) {
        u->poll_err = perr;
        u->text_dirty = 1;
    }
    /* auth.c forgets a revoked refresh token; only a new sign-in helps. */
    if ((perr == SP_ERR_AUTH || u->cmd_err == SP_ERR_AUTH) && !auth_store_logged_in(u->auth)) {
        stop_session(u);
        u->login_msg = "Session expired. Please sign in again.";
        advance_signin(u);
        return;
    }
    poll_engine(u);
    plat_mutex_lock(u->cmd.mu);
    u->cmd.local = local_ready(u);
    plat_mutex_unlock(u->cmd.mu);

    char msg[sizeof u->msg];
    int red = status_message(u, msg, sizeof msg);
    if (strcmp(msg, u->msg_shown) != 0 || red != u->status_red) {
        memcpy(u->msg_shown, msg, sizeof u->msg_shown);
        u->status_red = red;
        u->text_dirty = 1;
    }
    /* The one pairing UI: a dismissible tip while an unpaired Soloist is
     * discoverable. */
    char hint[sizeof u->hint] = "";
    if (u->sol && !u->paired && !u->hint_hidden)
        snprintf(hint, sizeof hint, "Tip: in Spotify on %s, choose '%s' to play on this computer.",
                 u->other_dev[0] ? u->other_dev : "your phone or computer", u->cfg.device_name);
    if (strcmp(hint, u->hint) != 0) {
        memcpy(u->hint, hint, sizeof u->hint);
        u->text_dirty = 1;
    }

    /* Layout (logical pixels). The art square takes the slack height. */
    const Metrics *m = &u->m;
    PlayerRows rows = player_rows(m);
    float w = (float)u->w, h = (float)u->h, cw = w - 2 * PAD;
    float hint_h = two_lines(m) + 6, art_min = 2 * m->row + 6; /* Restart and Change key fit */
    u->need_h = player_reserve(m) + hint_h + art_min;
    float art = h - player_reserve(m) - (u->hint[0] ? hint_h : 0.0f);
    if (art > cw)
        art = cw;
    if (art < art_min)
        art = art_min;
    /* Bottom bar: Devices and Library on the left, Settings and Log out on
     * the right; each button is its label plus a margin that narrows to fit. */
    float lw[BAR_BUTTONS], labels = 0;
    for (int i = 0; i < BAR_BUTTONS; i++) {
        lw[i] = text_w(u, bar_label[i], FS_S);
        labels += lw[i];
    }
    float margin = (cw - labels - (BAR_BUTTONS - 1) * BAR_GAP) / (2 * BAR_BUTTONS);
    margin = fmaxf(BAR_MARGIN, fminf(margin, roundf(0.8f * (float)m->fs)));
    float by = h - PAD - m->bar;
    Rectangle devices = { PAD, by, lw[0] + 2 * margin, m->bar };
    Rectangle library = { devices.x + devices.width + BAR_GAP, by, lw[1] + 2 * margin, m->bar };
    Rectangle logout = { w - PAD - lw[3] - 2 * margin, by, lw[3] + 2 * margin, m->bar };
    Rectangle settings = { logout.x - BAR_GAP - lw[2] - 2 * margin, by, lw[2] + 2 * margin, m->bar };
    float status_y = by - two_lines(m) - 4; /* up to two lines */
    float xw = (float)m->fs + 8;              /* the tip's close button */
    if (u->text_dirty)
        refresh_player_text(u, cw, cw, cw - xw - 6);

    /* Paired, but Soloist is down (stopped, failed to start or update). */
    int sol_down = u->paired && !u->sol && !u->inst.th && u->eng_err[0];
    text(u, u->status, PAD, status_y, FS_S, u->status_red ? C_ERR : C_DIM);
    if (u->hint[0]) {
        float hy = status_y - hint_h;
        text(u, u->hint_fit, PAD, hy, FS_S, C_DIM);
        Rectangle rx = { w - PAD - xw, hy, xw, (float)m->fs + 6 };
        if (button(u, W_HINT_CLOSE, rx, "x", FS_S, 0))
            u->hint_hidden = 1; /* Soloist stays discoverable */
    }
    if (button(u, W_LOGOUT, logout, "Log out", FS_S, 0)) {
        stop_session(u); /* Soloist first: logout deletes its pairing */
        auth_store_logout(u->auth);
        u->reinstalled = 0;
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
    if (button(u, W_SETTINGS, settings, "Settings", FS_S, u->panel == PANEL_SETTINGS)) {
        u->panel = u->panel == PANEL_SETTINGS ? PANEL_NONE : PANEL_SETTINGS;
        u->size_unsaved = 0;
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
    if (u->panel == PANEL_SETTINGS) {
        draw_settings(u, panel);
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
    if (sol_down) {
        /* Only on request: an automatic restart could loop forever. */
        float bw = fmaxf(btn_w(u, "Restart Soloist", 140), btn_w(u, "Change key", 140));
        float ry = y + (art - art_min) / 2.0f;
        Rectangle rr = { (w - bw) / 2.0f, ry, bw, m->row };
        Rectangle rk = { (w - bw) / 2.0f, ry + m->row + 6, bw, m->row };
        if (button(u, W_RESTART, rr, "Restart Soloist", FS_S, 1)) {
            reload_key(u); /* a key fixed in .env takes effect */
            start_engine(u, 0);
            return;
        }
        if (button(u, W_CHANGE_KEY, rk, "Change key", FS_S, 0)) {
            u->setup_msg = NULL;
            set_screen(u, SCR_KEY);
            return;
        }
    }
    y += art; /* the rows below are offsets from the art's bottom edge */

    text(u, u->title, PAD, y + rows.title, FS_L, C_TEXT);
    text(u, u->artists, PAD, y + rows.artists, FS_S, C_TEXT);
    text(u, u->album, PAD, y + rows.album, FS_S, C_DIM);

    PlaybackState *s = &u->st;
    int dur = s->track.duration_ms;
    int can_seek = s->active && dur > 0;
    Rectangle bar = { PAD, y + rows.seek, cw, 6 };
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
        sp_fmt_time(u->t_pos, sizeof u->t_pos, pos);
    }
    text(u, u->t_pos, PAD, y + rows.times, FS_S, C_DIM);
    text(u, u->t_dur, w - PAD - text_w(u, u->t_dur, FS_S), y + rows.times, FS_S, C_DIM);

    float ty = y + rows.transport;
    Rectangle rprev = { w / 2.0f - 28 - 16 - 44, ty + 2, 44, 44 };
    Rectangle rplay = { w / 2.0f - 28, ty, 56, 48 };
    Rectangle rnext = { w / 2.0f + 28 + 16, ty + 2, 44, 44 };
    if (button(u, W_PREV, rprev, NULL, 0, 0)) {
        u->pend.mask &= ~PEND_SEEK;
        cmd_post(&u->cmd, CMD_PREV, 0);
    }
    if (button(u, W_PLAY, rplay, NULL, 0, 1)) {
        int play = !(s->active && s->is_playing);
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

    static const char *const repeat_label[] = { "Repeat: off", "Repeat: all", "Repeat: one" };
    float half = (cw - 8) / 2.0f;
    Rectangle rshuf = { PAD, y + rows.modes, half, m->row };
    Rectangle rrep = { PAD + half + 8, y + rows.modes, half, m->row };
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

    float vy = y + rows.volume, vx = PAD + text_w(u, "Vol", FS_S) + 14;
    text(u, "Vol", PAD, vy, FS_S, C_DIM);
    Rectangle rvol = { vx, vy + ((float)m->fs - 6) / 2.0f, w - PAD - vx, 6 };
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
    SetWindowMinSize(mw, mh);
    if (u->w < mw || u->h < mh)
        SetWindowSize(u->w > mw ? u->w : mw, u->h > mh ? u->h : mh);
}

static void frame(Ui *u)
{
    apply_font(u); /* remade only when the size setting or the window scale changed */
    int w = GetScreenWidth(), h = GetScreenHeight();
    if (w != u->w || h != u->h) {
        u->w = w;
        u->h = h;
        u->text_dirty = 1;
    }
    BeginDrawing();
    ClearBackground(C_BG);
    switch (u->screen) {
    case SCR_MOVE:   draw_move(u); break;
    case SCR_SETUP:  draw_setup(u); break;
    case SCR_LOGIN:  draw_login(u); break;
    case SCR_KEY:    draw_key(u); break;
    case SCR_PLAYER: draw_player(u); break;
    }
    fit_window(u);
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
    /* ALWAYS_RUN: raylib otherwise parks a minimized window in its own
     * glfwWaitEvents loop that our wakeups can't leave, so a quit signal
     * would be ignored until restore. Event waiting still keeps a
     * minimized window idle. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT |
                   FLAG_WINDOW_ALWAYS_RUN);
    /* A new window shows the art WIN_ART tall at the chosen text size. */
    Metrics m0 = metrics_for(u->cfg.font_size);
    InitWindow(WIN_W, (int)ceilf(player_reserve(&m0) + WIN_ART), "Spotify C Player");
    if (!IsWindowReady()) {
        plat_mutex_free(u->mu);
        return 1;
    }
    SetExitKey(KEY_NULL);
    SetTextLineSpacing(LINE_GAP);
    EnableEventWaiting();
    /* Quit signals end the loop like closing the window does, so the
     * cleanup below always runs; the wake breaks glfwWaitEvents. */
    (void)plat_quit_watch(ui_wake, NULL); /* not fatal: signals then just kill us */
    apply_font(u); /* the window's minimum size follows from it (fit_window) */

    int rc = 0;
    if (u->cfg.read_only)
        set_screen(u, SCR_MOVE);
    else if (!u->cfg.client_id[0])
        set_screen(u, SCR_SETUP);
    else if (enter_app(u) != 0)
        rc = 1;

    while (rc == 0 && !WindowShouldClose() && !plat_quit_requested())
        frame(u);

    /* Every thread that could call ui_wake is joined before the window goes. */
    stop_session(u);
    login_join(u);
    auth_store_free(u->auth);
    (void)plat_quit_watch(NULL, NULL); /* no wake-ups once GLFW is gone */
    plat_mutex_free(u->mu);
    UnloadFont(u->font);
    UnloadFont(u->font_l);
    CloseWindow();
    return rc;
}
