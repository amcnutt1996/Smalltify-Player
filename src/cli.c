/* Command-line front end. Commands print human-readable output on stdout
 * and problems on stderr; the return value is the process exit code. */
#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auth.h"
#include "http.h"
#include "platform.h"
#include "soloist.h"
#include "spotify.h"

/* Returned by helpers that already printed their own message. */
#define CLI_REPORTED 1

static int bad_usage(const char *name)
{
    for (const CliCommand *c = cli_commands; c->name; c++)
        if (!strcmp(c->name, name)) {
            fprintf(stderr, "usage: " CLI_PROG " %s\n", c->usage);
            break;
        }
    return 1;
}

/* Set by SIGINT: cancels a login. auth_store_login takes a
 * `const volatile int *`, which sig_atomic_t is on every supported OS. */
static volatile sig_atomic_t g_interrupted;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static void print_missing_client_id(const Config *cfg)
{
    fprintf(stderr,
            "SPOTIFY_CLIENT_ID is not set.\n"
            "  1. Create an app at https://developer.spotify.com/dashboard\n"
            "  2. Add the redirect URI %s\n"
            "  3. Put SPOTIFY_CLIENT_ID=<your client id> in %s%c.env\n"
            "     (or set the SPOTIFY_CLIENT_ID environment variable)\n",
            cfg->redirect_uri, cfg->app_dir, PLAT_SEP);
}

/* Shared setup for commands that talk to Spotify: an AuthStore and, when
 * http is non-NULL, an HttpClient that authenticates through it. Prints a
 * friendly hint and returns -1 when not configured or not logged in.
 * Release with auth_store_free/http_client_free (both NULL-safe). */
static int open_session(const Config *cfg, AuthStore **auth, HttpClient **http)
{
    *auth = NULL;
    if (http)
        *http = NULL;
    if (!cfg->client_id[0]) {
        print_missing_client_id(cfg);
        return -1;
    }
    *auth = auth_store_new(cfg);
    if (!*auth) {
        fprintf(stderr, "error: out of memory\n");
        return -1;
    }
    if (!auth_store_logged_in(*auth)) {
        fprintf(stderr, "Not logged in. Run: " CLI_PROG " login\n");
        auth_store_free(*auth);
        *auth = NULL;
        return -1;
    }
    if (http) {
        HttpAuth ha = auth_store_http(*auth);
        *http = http_client_new(&ha);
        if (!*http) {
            fprintf(stderr, "error: cannot create HTTP client\n");
            auth_store_free(*auth);
            *auth = NULL;
            return -1;
        }
    }
    return 0;
}

static int cmd_login(const Config *cfg, int argc, char **argv)
{
    if (argc != 1)
        return bad_usage(argv[0]);
    if (!cfg->client_id[0]) {
        print_missing_client_id(cfg);
        return 1;
    }
    AuthStore *auth = auth_store_new(cfg);
    if (!auth) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    printf("Opening Spotify sign-in in your browser (Ctrl-C to cancel)...\n");
    g_interrupted = 0;
    void (*prev)(int) = signal(SIGINT, on_sigint);
    int rc = auth_store_login(auth, &g_interrupted);
    signal(SIGINT, prev == SIG_ERR ? SIG_DFL : prev);
    auth_store_free(auth);
    if (rc != 0) {
        if (g_interrupted)
            fprintf(stderr, "Login cancelled.\n");
        else
            fprintf(stderr, "Login failed: %s\n", auth_login_strerror(rc));
        return 1;
    }
    printf("Logged in. Token saved to %s\n", cfg->token_path);
    return 0;
}

static int cmd_logout(const Config *cfg, int argc, char **argv)
{
    if (argc != 1)
        return bad_usage(argv[0]);
    AuthStore *auth = auth_store_new(cfg);
    if (!auth) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    int had = auth_store_logged_in(auth);
    auth_store_logout(auth);
    auth_store_free(auth);
    printf(had ? "Logged out (removed the saved token and the Soloist pairing).\n"
               : "Not logged in.\n");
    return 0;
}

static void print_playback(const PlaybackState *st)
{
    static const char *const repeat_names[] = { "off", "context", "track" };
    const Track *t = &st->track;
    char pos[16], dur[16];

    if (!st->active) {
        printf("Nothing playing.\n");
        return;
    }
    printf("%s %s", st->is_playing ? "\xE2\x96\xB6" : "\xE2\x8F\xB8", /* ▶ / ⏸ */
           t->name[0] ? t->name : "Unknown");
    if (t->artists[0])
        printf(" \xE2\x80\x94 %s", t->artists); /* em dash */
    printf("\n");
    if (t->album[0])
        printf("  %s\n", t->album);

    sp_fmt_time(pos, sizeof pos, st->progress_ms);
    sp_fmt_time(dur, sizeof dur, t->duration_ms);
    printf("  %s / %s", pos, dur);
    if (st->device_name[0])
        printf("  on %s", st->device_name);
    if (st->shuffle && st->repeat != REPEAT_OFF)
        printf("  (shuffle, repeat: %s)", repeat_names[st->repeat]);
    else if (st->shuffle)
        printf("  (shuffle)");
    else if (st->repeat != REPEAT_OFF)
        printf("  (repeat: %s)", repeat_names[st->repeat]);
    printf("\n");
}

static int cmd_now(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    PlaybackState st;
    int rc;

    if (argc != 1)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    rc = sp_get_playback(http, &st);
    http_client_free(http);
    auth_store_free(auth);
    if (rc != SP_OK) {
        fprintf(stderr, "%s\n", sp_strerror(rc));
        return 1;
    }
    print_playback(&st);
    return 0;
}

/* Releases the session and turns an SP_* code into an exit code. */
static int finish(AuthStore *auth, HttpClient *http, int rc)
{
    http_client_free(http);
    auth_store_free(auth);
    if (rc == SP_OK)
        return 0;
    if (rc != CLI_REPORTED)
        fprintf(stderr, "%s\n", sp_strerror(rc));
    return 1;
}

/* Whole-string decimal in [lo, hi]; 0 ok. */
static int parse_int(const char *s, long lo, long hi, int *out)
{
    char *end;
    long v;

    if (!s || !isdigit((unsigned char)s[0]))
        return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

/* "90" or "1:30" -> milliseconds; 0 ok. */
static int parse_position(const char *s, int *ms)
{
    const char *colon = strchr(s, ':');
    int min, sec;

    if (!colon) {
        if (parse_int(s, 0, INT_MAX / 1000, &sec) != 0)
            return -1;
        *ms = sec * 1000;
        return 0;
    }
    char mbuf[16];
    size_t mlen = (size_t)(colon - s);
    if (mlen == 0 || mlen >= sizeof mbuf || strlen(colon + 1) != 2)
        return -1;
    memcpy(mbuf, s, mlen);
    mbuf[mlen] = '\0';
    if (parse_int(mbuf, 0, INT_MAX / 60000 - 1, &min) != 0 ||
        parse_int(colon + 1, 0, 59, &sec) != 0)
        return -1;
    *ms = (min * 60 + sec) * 1000;
    return 0;
}

/* Our device name, for the play fallback (set by cli_dispatch). */
static const char *g_device_name;

/* Start playback (both URIs NULL = resume; see sp_play_uri) on the active
 * device; with none active, on our own engine if it is online (so `play`
 * works right after `engine` starts), else on any available device of the
 * account. */
static int play_with_fallback(HttpClient *http, const char *context_uri, const char *item_uri)
{
    Device on;
    int where;
    int rc = sp_play_smart(http, context_uri, item_uri, g_device_name, 0, &on, &where);
    if (rc == SP_OK && where == SP_ON_OTHER)
        printf("Playing on %s\n", on.name);
    if (rc != SP_ERR_NODEVICE)
        return rc;
    fprintf(stderr, "No device available. Open Spotify on any device, or run: " CLI_PROG
                    " engine (or open the app)\n");
    return CLI_REPORTED;
}

static int play_or_fallback(HttpClient *http)
{
    return play_with_fallback(http, NULL, NULL);
}

/* Names and queries may contain spaces, so unquoted words are joined back
 * together. 0 ok, -1 empty or too long. */
static int join_args(char *out, size_t cap, int argc, char **argv)
{
    size_t len = 0;

    out[0] = '\0';
    for (int i = 0; i < argc; i++) {
        int w = snprintf(out + len, cap - len, "%s%s", i > 0 ? " " : "", argv[i]);
        if (w < 0 || (size_t)w >= cap - len)
            return -1;
        len += (size_t)w;
    }
    return len > 0 ? 0 : -1;
}

/* Commands without arguments that map 1:1 onto one sp_* call. */
static int run_simple(const Config *cfg, int argc, char **argv, int (*fn)(HttpClient *))
{
    AuthStore *auth;
    HttpClient *http;

    if (argc != 1)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    return finish(auth, http, fn(http));
}

static int cmd_play(const Config *cfg, int argc, char **argv)
{
    return run_simple(cfg, argc, argv, play_or_fallback);
}

static int cmd_pause(const Config *cfg, int argc, char **argv)
{
    return run_simple(cfg, argc, argv, sp_pause);
}

static int cmd_next(const Config *cfg, int argc, char **argv)
{
    return run_simple(cfg, argc, argv, sp_next);
}

static int cmd_prev(const Config *cfg, int argc, char **argv)
{
    return run_simple(cfg, argc, argv, sp_prev);
}

static int toggle(HttpClient *http)
{
    PlaybackState st;
    int rc = sp_get_playback(http, &st);

    if (rc != SP_OK)
        return rc;
    return st.active && st.is_playing ? sp_pause(http) : play_or_fallback(http);
}

static int cmd_toggle(const Config *cfg, int argc, char **argv)
{
    return run_simple(cfg, argc, argv, toggle);
}

static int cmd_seek(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    int ms;

    if (argc != 2 || parse_position(argv[1], &ms) != 0)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    return finish(auth, http, sp_seek(http, ms));
}

static int cmd_vol(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    PlaybackState st;
    int sign = 0, v, rc;

    if (argc != 2)
        return bad_usage(argv[0]);
    if (argv[1][0] == '+' || argv[1][0] == '-')
        sign = argv[1][0] == '+' ? 1 : -1;
    if (parse_int(argv[1] + (sign != 0), 0, 100, &v) != 0)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    if (sign) {
        /* Relative steps need the current level; sp_set_volume clamps. */
        rc = sp_get_playback(http, &st);
        if (rc == SP_OK && !st.active)
            rc = SP_ERR_NODEVICE;
        if (rc == SP_OK && st.volume < 0) {
            fprintf(stderr, "This device does not report its volume.\n");
            rc = CLI_REPORTED;
        }
        if (rc != SP_OK)
            return finish(auth, http, rc);
        v = st.volume + sign * v;
    }
    return finish(auth, http, sp_set_volume(http, v));
}

static int cmd_shuffle(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    int on;

    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off")))
        return bad_usage(argv[0]);
    on = !strcmp(argv[1], "on");
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    return finish(auth, http, sp_set_shuffle(http, on));
}

static int cmd_repeat(const Config *cfg, int argc, char **argv)
{
    static const char *const names[] = { "off", "context", "track" };
    AuthStore *auth;
    HttpClient *http;
    int m;

    if (argc != 2)
        return bad_usage(argv[0]);
    for (m = REPEAT_OFF; m <= REPEAT_TRACK && strcmp(argv[1], names[m]); m++) {
    }
    if (m > REPEAT_TRACK)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    return finish(auth, http, sp_set_repeat(http, (RepeatMode)m));
}

static int cmd_devices(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    Device d[SP_MAX_DEVICES];
    int n, rc;

    if (argc != 1)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    rc = sp_get_devices(http, d, SP_MAX_DEVICES, &n);
    if (rc == SP_OK && n == 0)
        printf("No devices found. Start one with: " CLI_PROG " engine (or open the app)\n");
    for (int i = 0; rc == SP_OK && i < n; i++) {
        printf("%c %s (%s)", d[i].is_active ? '*' : ' ', d[i].name, d[i].type);
        if (d[i].volume >= 0)
            printf(" %d%%", d[i].volume);
        putchar('\n');
    }
    return finish(auth, http, rc);
}

static int cmd_transfer(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    Device d[SP_MAX_DEVICES];
    const Device *hit = NULL;
    char want[SP_NAME_LEN];
    int n, rc;

    if (argc < 2 || join_args(want, sizeof want, argc - 1, argv + 1) != 0)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    rc = sp_get_devices(http, d, SP_MAX_DEVICES, &n);
    /* Names win over ids: a name the user typed is the likelier intent. */
    for (int i = 0; rc == SP_OK && !hit && i < n; i++)
        if (!strcmp(d[i].name, want))
            hit = &d[i];
    for (int i = 0; rc == SP_OK && !hit && i < n; i++)
        if (d[i].id[0] && !strcmp(d[i].id, want))
            hit = &d[i];
    if (rc == SP_OK && (!hit || !hit->id[0])) {
        fprintf(stderr, hit ? "Device '%s' cannot be controlled remotely.\n"
                            : "No device named '%s'. List them with: " CLI_PROG " devices\n",
                want);
        rc = CLI_REPORTED;
    }
    if (rc == SP_OK)
        rc = sp_transfer(http, hit->id, 0);
    if (rc == SP_OK)
        printf("Playback moved to %s\n", hit->name);
    return finish(auth, http, rc);
}

#define CLI_MAX_PLAYLISTS 50 /* first page of /me/playlists */
#define CLI_MAX_RESULTS   10

static int cmd_playlists(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    Playlist p[CLI_MAX_PLAYLISTS];
    int n, rc;

    if (argc != 1)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    rc = sp_get_playlists(http, p, CLI_MAX_PLAYLISTS, &n);
    if (rc == SP_OK && n == 0)
        printf("No playlists found.\n");
    for (int i = 0; rc == SP_OK && i < n; i++)
        printf("%2d. %s (%d tracks)\n", i + 1, p[i].name, p[i].track_count);
    return finish(auth, http, rc);
}

/* `playlist <n|name>`: a number picks from the `playlists` listing; anything
 * else must match a playlist name exactly. */
static int cmd_playlist(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    Playlist p[CLI_MAX_PLAYLISTS];
    const Playlist *hit = NULL;
    char want[SP_NAME_LEN];
    int n, idx = 0, rc;

    if (argc < 2 || join_args(want, sizeof want, argc - 1, argv + 1) != 0)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    rc = sp_get_playlists(http, p, CLI_MAX_PLAYLISTS, &n);
    if (rc == SP_OK && argc == 2 && parse_int(want, 1, CLI_MAX_PLAYLISTS, &idx) == 0 && idx <= n)
        hit = &p[idx - 1];
    for (int i = 0; rc == SP_OK && !hit && i < n; i++)
        if (!strcmp(p[i].name, want))
            hit = &p[i];
    if (rc == SP_OK && (!hit || !hit->uri[0])) {
        fprintf(stderr, "No playlist '%s'. List them with: " CLI_PROG " playlists\n", want);
        rc = CLI_REPORTED;
    }
    if (rc == SP_OK)
        rc = play_with_fallback(http, hit->uri, NULL);
    if (rc == SP_OK)
        printf("Playing %s\n", hit->name);
    return finish(auth, http, rc);
}

/* `search [-p N] <query...>`: list results, or play result N. */
static int cmd_search(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    Track t[CLI_MAX_RESULTS];
    char query[SP_NAME_LEN];
    int first = 1, pick = 0, n, rc;

    if (argc >= 2 && !strcmp(argv[1], "-p")) {
        if (argc < 4 || parse_int(argv[2], 1, CLI_MAX_RESULTS, &pick) != 0)
            return bad_usage(argv[0]);
        first = 3;
    }
    if (argc <= first || join_args(query, sizeof query, argc - first, argv + first) != 0)
        return bad_usage(argv[0]);
    if (open_session(cfg, &auth, &http) != 0)
        return 1;
    rc = sp_search_tracks(http, query, t, CLI_MAX_RESULTS, &n);
    if (rc == SP_OK && pick) {
        if (pick > n) {
            fprintf(stderr, "Only %d result(s) for '%s'.\n", n, query);
            rc = CLI_REPORTED;
        } else {
            rc = play_with_fallback(http, NULL, t[pick - 1].uri);
            if (rc == SP_OK)
                printf("Playing %s - %s\n", t[pick - 1].name, t[pick - 1].artists);
        }
        return finish(auth, http, rc);
    }
    if (rc == SP_OK && n == 0)
        printf("No results for '%s'.\n", query);
    for (int i = 0; rc == SP_OK && i < n; i++)
        printf("%2d. %s - %s (%s)\n", i + 1, t[i].name, t[i].artists, t[i].album);
    return finish(auth, http, rc);
}

/* Set by a quit signal or stdin EOF: cancels an engine download. */
static volatile int g_engine_stop;
/* Set once stdin hits EOF (Ctrl-D / closed pipe): ends `engine`. */
static volatile sig_atomic_t g_stdin_eof;

static void stdin_watch(void *arg)
{
    int ch;
    while ((ch = getchar()) != EOF) {
    }
    g_stdin_eof = 1;
    g_engine_stop = 1;
    plat_event_signal(arg);
}

static void wake_event(void *ev)
{
    plat_event_signal(ev);
}

static void engine_quit(void *ev)
{
    g_engine_stop = 1;
    plat_event_signal(ev);
}

static void print_missing_soloist_key(const Config *cfg)
{
    fprintf(stderr,
            "SOLOIST_API_KEY is not set.\n"
            "  1. Generate a key at https://developer.spotify.com/dashboard/soloist\n"
            "  2. Put SOLOIST_API_KEY=<your key> in %s%c.env\n"
            "     (or set the SOLOIST_API_KEY environment variable)\n",
            cfg->app_dir, PLAT_SEP);
}

static void print_progress(void *ud, int percent)
{
    (void)ud;
    if (percent >= 0) {
        printf("\rDownloading Spotify Soloist... %d%%", percent);
        fflush(stdout);
    }
}

/* 0 ok; -1 after printing why. */
static int install_engine(const Config *cfg)
{
    char err[256];
    printf("Downloading Spotify Soloist...");
    fflush(stdout);
    int rc = soloist_install(cfg, print_progress, NULL, &g_engine_stop, err, sizeof err);
    printf("\n");
    if (rc != 0) {
        fprintf(stderr, "error: %s\n", err);
        return -1;
    }
    printf("Installed in %s\n", cfg->soloist_dir);
    return 0;
}

static int cmd_engine(const Config *cfg, int argc, char **argv)
{
    int update = argc == 2 && !strcmp(argv[1], "--update");
    if (argc > 2 || (argc == 2 && !update))
        return bad_usage(argv[0]);
    if (!cfg->soloist_key[0]) {
        print_missing_soloist_key(cfg);
        return 1;
    }
    /* Ctrl-C, SIGTERM and SIGHUP all end the loop below, so Soloist is
     * stopped instead of orphaned. */
    plat_event *wake = plat_event_new();
    if (!wake || plat_quit_watch(engine_quit, wake) != 0) {
        fprintf(stderr, "error: cannot set up signal handling\n");
        plat_quit_watch(NULL, NULL);
        plat_event_free(wake);
        return 1;
    }
    /* EOF ends `engine` only from a terminal: with stdin /dev/null (nohup,
     * launchd, systemd) it would stop at once, and `engine &` would be
     * stopped by SIGTTIN. Blocking stdin read has no portable cancel; on
     * Ctrl-C this thread is left parked in getchar() and ends with the
     * process. */
    plat_thread *watch = plat_stdin_is_tty() ? plat_thread_start(stdin_watch, wake) : NULL;

    Soloist *s = NULL;
    if ((update || !soloist_up_to_date(cfg)) && install_engine(cfg) != 0)
        goto out;
    if (g_engine_stop)
        goto out;
    s = soloist_start(cfg, wake_event, wake);
    if (!s) {
        fprintf(stderr, "error: cannot start %s\n", cfg->soloist_path);
        goto out;
    }
    printf("Running \"%s\" as a Spotify Connect device. Ctrl-C%s to stop.\n",
           cfg->device_name, watch ? " or EOF" : "");
    fflush(stdout);

    char last[512] = "", cur[512];
    int hinted = 0, reinstalled = 0;
    for (;;) {
        SoloistState st = soloist_state(s);
        soloist_status(s, cur, sizeof cur);
        if (strcmp(cur, last) != 0) {
            printf("Soloist: %s\n", cur);
            snprintf(last, sizeof last, "%s", cur);
        }
        if (st == SOLOIST_PAIRING && !hinted) {
            printf("Pair once: open Spotify on a phone or computer signed in to the same "
                   "account (same network), open the device list and choose \"%s\".\n",
                   cfg->device_name);
            hinted = 1;
        }
        fflush(stdout);
        if (g_engine_stop)
            break;
        if (st == SOLOIST_EXPIRED && !reinstalled) {
            /* Expired while running: fetch the current build, start once more. */
            reinstalled = 1;
            soloist_stop(s);
            s = NULL;
            if (install_engine(cfg) != 0 || g_engine_stop)
                break;
            s = soloist_start(cfg, wake_event, wake);
            if (!s) {
                fprintf(stderr, "error: cannot start %s\n", cfg->soloist_path);
                break;
            }
            last[0] = '\0';
            continue;
        }
        if (st == SOLOIST_STOPPED || st == SOLOIST_EXPIRED)
            break;
        /* Signaled by Soloist changes (its exit included), quit signals
         * and stdin EOF: no polling. */
        plat_event_wait(wake, 3600000u);
    }
out:
    soloist_stop(s);
    plat_quit_watch(NULL, NULL); /* a later signal must not touch `wake` */
    if (g_stdin_eof || !watch) {
        plat_thread_join(watch);
        plat_event_free(wake);
    }
    /* else the watcher still blocks in getchar() holding `wake`; both are
     * reclaimed at process exit. */
    return g_engine_stop ? 0 : 1; /* Ctrl-C/EOF is a clean stop, an exit a failure */
}

const CliCommand cli_commands[] = {
    { "login",  "login",  cmd_login  },
    { "logout", "logout", cmd_logout },
    { "engine", "engine [--update]", cmd_engine },
    { "now",    "now",    cmd_now    },
    { "play",     "play",                            cmd_play     },
    { "pause",    "pause",                           cmd_pause    },
    { "toggle",   "toggle",                          cmd_toggle   },
    { "next",     "next",                            cmd_next     },
    { "prev",     "prev",                            cmd_prev     },
    { "seek",     "seek <seconds | m:ss>",           cmd_seek     },
    { "vol",      "vol <0-100 | +N | -N>",           cmd_vol      },
    { "shuffle",  "shuffle on|off",                  cmd_shuffle  },
    { "repeat",   "repeat off|context|track",        cmd_repeat   },
    { "devices",  "devices",                         cmd_devices  },
    { "transfer", "transfer <name-or-id>",           cmd_transfer },
    { "playlists", "playlists",                      cmd_playlists },
    { "playlist", "playlist <number | name>",        cmd_playlist },
    { "search",   "search [-p N] <query...>",        cmd_search   },
    { NULL, NULL, NULL },
};

int cli_dispatch(const Config *cfg, int argc, char **argv)
{
    g_device_name = cfg->device_name;
    for (const CliCommand *c = cli_commands; c->name; c++)
        if (!strcmp(c->name, argv[0]))
            return c->run(cfg, argc, argv);
    fprintf(stderr, "unknown command '%s'\n\n", argv[0]);
    cli_usage();
    return 2;
}

void cli_usage(void)
{
    printf("Usage: " CLI_PROG " [command]\n"
           "Without a command the player window opens.\n\n"
           "Commands:\n");
    for (const CliCommand *c = cli_commands; c->name; c++)
        printf("  %s\n", c->usage);
}
