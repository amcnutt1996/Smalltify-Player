/* Command-line front end. Commands print human-readable output on stdout
 * and problems on stderr; the return value is the process exit code. */
#include "cli.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "http.h"
#include "librespot.h"
#include "platform.h"

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
