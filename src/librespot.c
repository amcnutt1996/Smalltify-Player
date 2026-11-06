/* Supervises the bundled librespot (Spotify Connect playback engine).
 *
 * Login: since Spotify's August 2026 change, access tokens minted for a
 * third-party client ID (even with the `streaming` scope) pass the access
 * point handshake but are rejected by login5 (INVALID_CREDENTIALS), so
 * librespot 0.8.0 cannot play with our Web API token (see
 * mopidy/mopidy-spotify#437). librespot therefore signs in once through its
 * own OAuth flow (`--enable-oauth`, Spotify's client ID, redirect on
 * 127.0.0.1:5588) and keeps reusable credentials in data/librespot; later
 * starts use that cache without a browser. Passing --access-token would
 * override the cache, so we never pass ours. */
#include "librespot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define LINE_MAX_LEN 1024

struct Librespot {
    plat_proc   *proc;
    plat_thread *reader;
    plat_mutex  *lock;            /* guards status */
    char         status[LINE_MAX_LEN];
};

static void set_status(Librespot *l, const char *s, size_t n)
{
    plat_mutex_lock(l->lock);
    if (n >= sizeof l->status)
        n = sizeof l->status - 1;
    memcpy(l->status, s, n);
    l->status[n] = '\0';
    plat_mutex_unlock(l->lock);
}

/* Keep lines a user can act on: warnings, errors, sign-in URL, login and
 * connection state. env_logger lines look like
 * "[2026-11-06T13:27:55Z INFO  librespot_core::session] Authenticated as ..."
 * and are reduced to their message ("error: ..." for WARN/ERROR). */
static void on_line(Librespot *l, char *line, size_t n)
{
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
        n--;
    line[n] = '\0';
    if (n == 0)
        return;
    const char *msg = line, *level = "";
    if (line[0] == '[') {
        const char *close = strstr(line, "] ");
        if (!close)
            return;
        if (strstr(line, " DEBUG ") || strstr(line, " TRACE "))
            return;
        if (strstr(line, " WARN "))
            level = "warning: ";
        else if (strstr(line, " ERROR "))
            level = "error: ";
        msg = close + 2;
    }
    char out[LINE_MAX_LEN];
    size_t ln = strlen(level), mn = strlen(msg);
    if (ln + mn >= sizeof out)
        mn = sizeof out - 1 - ln;
    memcpy(out, level, ln);
    memcpy(out + ln, msg, mn);
    set_status(l, out, ln + mn);
}

/* Drains the pipe (a full pipe would stall librespot) and tracks the last
 * meaningful line. Nothing is logged: output stays in memory. */
static void reader_main(void *arg)
{
    Librespot *l = arg;
    char buf[4096], line[LINE_MAX_LEN];
    size_t len = 0;
    int overflow = 0;
    long n;
    while ((n = plat_proc_read(l->proc, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                if (!overflow)
                    on_line(l, line, len);
                len = 0;
                overflow = 0;
            } else if (len + 1 < sizeof line) {
                line[len++] = ch;
            } else {
                overflow = 1; /* drop over-long lines rather than show half */
            }
        }
    }
    if (len && !overflow)
        on_line(l, line, len);
    char last[LINE_MAX_LEN];
    librespot_status(l, last, sizeof last);
    char msg[LINE_MAX_LEN];
    int k = snprintf(msg, sizeof msg, "librespot stopped: %s", last);
    if (k > 0)
        set_status(l, msg, (size_t)k);
}

Librespot *librespot_start(const Config *cfg, AuthStore *auth)
{
    (void)auth; /* see the file comment: our Web API token cannot log it in */
    if (plat_mkdir(cfg->data_dir) != 0 || plat_mkdir(cfg->librespot_cache) != 0)
        return NULL;
    Librespot *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->lock = plat_mutex_new();
    if (!l->lock) {
        free(l);
        return NULL;
    }
    snprintf(l->status, sizeof l->status, "starting librespot");
    const char *argv[] = {
        cfg->librespot_path,
        "--name", CFG_DEVICE_NAME,
        "--device-type", "computer",
        "--cache", cfg->librespot_cache,   /* credentials + volume only... */
        "--disable-audio-cache",           /* ...no audio files on disk */
        "--disable-discovery",             /* no mDNS: account-bound device */
        "--bitrate", "160",
        "--enable-oauth",                  /* used only while the cache is empty */
        NULL,
    };
    l->proc = plat_proc_spawn(argv);
    if (!l->proc) {
        plat_mutex_free(l->lock);
        free(l);
        return NULL;
    }
    l->reader = plat_thread_start(reader_main, l);
    if (!l->reader) {
        plat_proc_kill(l->proc);
        plat_mutex_free(l->lock);
        free(l);
        return NULL;
    }
    return l;
}

int librespot_running(Librespot *l)
{
    return l && plat_proc_alive(l->proc);
}

void librespot_status(Librespot *l, char *buf, size_t cap)
{
    if (!cap)
        return;
    if (!l) {
        buf[0] = '\0';
        return;
    }
    plat_mutex_lock(l->lock);
    snprintf(buf, cap, "%s", l->status);
    plat_mutex_unlock(l->lock);
}

void librespot_stop(Librespot *l)
{
    if (!l)
        return;
    /* Terminate first so the reader sees EOF, join it, then free the proc. */
    plat_proc_terminate(l->proc);
    plat_thread_join(l->reader);
    plat_proc_kill(l->proc);
    plat_mutex_free(l->lock);
    free(l);
}
