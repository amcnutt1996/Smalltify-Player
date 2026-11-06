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
#include "librespot.h"
#include "platform.h"
#include "spotify.h"

#define CLI_PROG "spotify-c-player"

/* Set by SIGINT: cancels a login and ends `engine`. auth_store_login takes
 * a `const volatile int *`, which sig_atomic_t is on every supported OS. */
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
    (void)argc; (void)argv;
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
        fprintf(stderr, g_interrupted ? "Login cancelled.\n" : "Login failed.\n");
        return 1;
    }
    printf("Logged in. Token saved to %s\n", cfg->token_path);
    return 0;
}

static int cmd_logout(const Config *cfg, int argc, char **argv)
{
    (void)argc; (void)argv;
    AuthStore *auth = auth_store_new(cfg);
    if (!auth) {
        fprintf(stderr, "error: out of memory\n");
        return 1;
    }
    auth_store_logout(auth);
    auth_store_free(auth);
    printf("Logged out (removed saved token and playback credentials).\n");
    return 0;
}

/* m:ss, or h:mm:ss for long episodes. */
static void fmt_time(char *out, size_t cap, int ms)
{
    int s = ms > 0 ? ms / 1000 : 0;
    if (s >= 3600)
        snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, cap, "%d:%02d", s / 60, s % 60);
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

    fmt_time(pos, sizeof pos, st->progress_ms);
    fmt_time(dur, sizeof dur, t->duration_ms);
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

    (void)argc; (void)argv;
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

/* Resume on the active device; with none active, fall back to our own
 * engine if it is online so `play` works right after `engine` starts. */
static int play_or_fallback(HttpClient *http)
{
    Device d;
    int rc = sp_play(http, NULL);

    if (rc != SP_ERR_NODEVICE)
        return rc;
    rc = sp_find_device(http, CFG_DEVICE_NAME, &d);
    if (rc == SP_OK && d.id[0])
        return sp_play(http, d.id);
    if (rc != SP_OK && rc != SP_ERR_NODEVICE)
        return rc;
    fprintf(stderr, "No active device. Start one with: " CLI_PROG " engine (or open the app)\n");
    return CLI_REPORTED;
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

/* Names may contain spaces, so unquoted words are joined back together. */
static int cmd_transfer(const Config *cfg, int argc, char **argv)
{
    AuthStore *auth;
    HttpClient *http;
    Device d[SP_MAX_DEVICES];
    const Device *hit = NULL;
    char want[SP_NAME_LEN];
    size_t len = 0;
    int n, rc;

    if (argc < 2)
        return bad_usage(argv[0]);
    want[0] = '\0';
    for (int i = 1; i < argc; i++) {
        int w = snprintf(want + len, sizeof want - len, "%s%s", i > 1 ? " " : "", argv[i]);
        if (w < 0 || (size_t)w >= sizeof want - len)
            return bad_usage(argv[0]);
        len += (size_t)w;
    }
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

/* Ends `engine` on stdin EOF (Ctrl-D / closed pipe). */
static void stdin_watch(void *arg)
{
    int ch;
    while ((ch = getchar()) != EOF) {
    }
    plat_event_signal(arg);
}

static int cmd_engine(const Config *cfg, int argc, char **argv)
{
    (void)argc; (void)argv;
    AuthStore *auth;
    if (open_session(cfg, &auth, NULL) != 0)
        return 1;
    plat_event *eof = plat_event_new();
    Librespot *ls = eof ? librespot_start(cfg, auth) : NULL;
    if (!ls) {
        fprintf(stderr, "error: cannot start %s\n", cfg->librespot_path);
        plat_event_free(eof);
        auth_store_free(auth);
        return 1;
    }
    printf("Running \"%s\" as a Spotify Connect device. Ctrl-C or EOF to stop.\n",
           CFG_DEVICE_NAME);
    fflush(stdout);
    g_interrupted = 0;
    void (*prev)(int) = signal(SIGINT, on_sigint);
    /* Blocking stdin read has no portable cancel; on Ctrl-C this thread is
     * left parked in getchar() and ends with the process. */
    plat_thread *watch = plat_thread_start(stdin_watch, eof);

    char last[1024] = "", cur[1024];
    int saw_eof = 0;
    for (;;) {
        librespot_status(ls, cur, sizeof cur);
        if (strcmp(cur, last) != 0) {
            printf("librespot: %s\n", cur);
            fflush(stdout);
            snprintf(last, sizeof last, "%s", cur);
        }
        if (g_interrupted || !librespot_running(ls))
            break;
        /* Wakes at once on EOF; the 500 ms tick picks up Ctrl-C and status. */
        if (plat_event_wait(eof, 500)) {
            saw_eof = 1;
            break;
        }
    }
    int crashed = !g_interrupted && !saw_eof;
    librespot_stop(ls);
    signal(SIGINT, prev == SIG_ERR ? SIG_DFL : prev);
    if (saw_eof || !watch) {
        plat_thread_join(watch);
        plat_event_free(eof);
    }
    /* else the watcher still blocks in getchar() holding `eof`; both are
     * reclaimed at process exit. */
    auth_store_free(auth);
    return crashed ? 1 : 0;
}

const CliCommand cli_commands[] = {
    { "login",  "login",  cmd_login  },
    { "logout", "logout", cmd_logout },
    { "engine", "engine", cmd_engine },
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
    { NULL, NULL, NULL },
};

int cli_dispatch(const Config *cfg, int argc, char **argv)
{
    for (const CliCommand *c = cli_commands; c->name; c++)
        if (!strcmp(c->name, argv[0]))
            return c->run(cfg, argc, argv);
    fprintf(stderr, "unknown command '%s'\n\n", argv[0]);
    cli_usage(CLI_PROG);
    return 2;
}

void cli_usage(const char *prog)
{
    printf("Usage: %s [command]\n"
           "Without a command the player window opens.\n\n"
           "Commands:\n", prog);
    for (const CliCommand *c = cli_commands; c->name; c++)
        printf("  %s\n", c->usage);
}
