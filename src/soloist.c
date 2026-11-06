/* Spotify Soloist supervisor and installer (see soloist.h). */
#include "soloist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "platform.h"

#define STATUS_LEN    512
#define LINE_MAX_LEN  1024
#define VERSION_LEN   512
#define ARCHIVE_MAX   (64u * 1024u * 1024u) /* the macOS build is ~11 MB */
#define EXIT_EXPIRED  10                    /* documented daemon exit code */
#define SECS_PER_DAY  86400

/* Published builds; none exists for Intel Macs or Windows on ARM. */
#define SOLOIST_CDN "https://soloist-builds.spotifycdn.com/soloist_release_"
#if defined(__APPLE__) && defined(__aarch64__)
#define SOLOIST_PKG "osx_arm64.tar.gz"
#elif defined(__linux__) && defined(__x86_64__)
#define SOLOIST_PKG "x86_64.tar.gz"
#elif defined(__linux__) && defined(__aarch64__)
#define SOLOIST_PKG "arm64.tar.gz"
#elif defined(__linux__) && defined(__arm__)
#define SOLOIST_PKG "arm32.tar.gz"
#elif defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
#define SOLOIST_PKG "windows_x86_64.zip"
#endif

/* --- build date / expiry ------------------------------------------------- */
/* Days since 1970-01-01 of a proleptic Gregorian date (H. Hinnant). */
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153L * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

long soloist_build_day(const char *version)
{
    /* "soloist 1.3.10.50 build 1793966524 (20261106) (g19412df6ea) (...)":
     * the date is the only parenthesized group of exactly 8 digits. */
    for (const char *p = strchr(version, '('); p; p = strchr(p + 1, '(')) {
        int i = 0;
        long v = 0;
        while (i < 8 && p[1 + i] >= '0' && p[1 + i] <= '9')
            v = v * 10 + (p[1 + i++] - '0');
        if (i != 8 || p[9] != ')')
            continue;
        long y = v / 10000;
        int m = (int)(v / 100 % 100), d = (int)(v % 100);
        if (y < 2000 || m < 1 || m > 12 || d < 1 || d > 31)
            continue;
        return days_from_civil(y, m, d);
    }
    return -1;
}

long soloist_days_left(long build_day, int64_t now)
{
    int64_t today = now >= 0 ? now / SECS_PER_DAY : (now - (SECS_PER_DAY - 1)) / SECS_PER_DAY;
    return build_day + SOLOIST_EXPIRY_DAYS - (long)today;
}

/* Soloist (through the OS networking stack) writes caches, cookies and
 * temp files under HOME/TMPDIR (macOS: ~/Library/Caches/soloist,
 * ~/Library/HTTPStorages/soloist). Point them all at
 * <app>/data/soloist-home so nothing lands outside the app folder. */
#define HOME_ENV_LEN (CFG_PATH_LEN + 32)
typedef struct {
    char        buf[6][HOME_ENV_LEN];
    const char *env[7];
} HomeEnv;

static int home_env(const Config *cfg, HomeEnv *h)
{
    char tmp[CFG_PATH_LEN];
    if (plat_mkdir(cfg->data_dir) != 0 || plat_mkdir(cfg->soloist_home) != 0 ||
        plat_path_join(tmp, sizeof tmp, cfg->soloist_home, "tmp") != 0 || plat_mkdir(tmp) != 0)
        return -1;
#ifdef _WIN32
    static const char *const names[] = { "USERPROFILE", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP", "HOME" };
    const char *vals[] = { cfg->soloist_home, cfg->soloist_home, cfg->soloist_home, tmp, tmp,
                           cfg->soloist_home };
#else
    /* CFFIXED_USER_HOME: where macOS's CoreFoundation looks for ~. */
    static const char *const names[] = { "HOME", "CFFIXED_USER_HOME", "TMPDIR" };
    const char *vals[] = { cfg->soloist_home, cfg->soloist_home, tmp };
#endif
    size_t n = sizeof names / sizeof names[0];
    for (size_t i = 0; i < n; i++) {
        int k = snprintf(h->buf[i], sizeof h->buf[i], "%s=%s", names[i], vals[i]);
        if (k < 0 || (size_t)k >= sizeof h->buf[i])
            return -1;
        h->env[i] = h->buf[i];
    }
    h->env[n] = NULL;
    return 0;
}

static plat_proc *spawn_soloist(const Config *cfg, const char *const argv[])
{
    HomeEnv h;
    return home_env(cfg, &h) == 0 ? plat_proc_spawn_env(argv, h.env) : NULL;
}

/* Run `soloist --version`: its exit code (-1 if it didn't run), output
 * in out. */
static int read_version(const Config *cfg, char *out, size_t cap)
{
    out[0] = '\0';
    const char *argv[] = { cfg->soloist_path, "--version", NULL };
    plat_proc *p = spawn_soloist(cfg, argv);
    if (!p)
        return -1;
    size_t len = 0;
    char buf[256];
    long n;
    while ((n = plat_proc_read(p, buf, sizeof buf)) > 0) {
        size_t k = (size_t)n < cap - 1 - len ? (size_t)n : cap - 1 - len;
        memcpy(out + len, buf, k);
        len += k;
    }
    out[len] = '\0';
    int code = plat_proc_wait(p, 5000);
    plat_proc_kill(p);
    return code;
}

int soloist_installed_ok(const Config *cfg)
{
    char v[VERSION_LEN];
    if (read_version(cfg, v, sizeof v) != 0)
        return 0;
    long day = soloist_build_day(v);
    return day >= 0 && soloist_days_left(day, plat_unix_time()) > SOLOIST_RENEW_DAYS;
}

/* --- download ------------------------------------------------------------ */
typedef struct {
    SoloistProgressFn   fn;
    void               *ud;
    const volatile int *cancel;
    int                 last;
} Progress;

static int on_progress(void *ud, size_t now, size_t total)
{
    Progress *pr = ud;
    if (pr->cancel && *pr->cancel)
        return 1;
    int pct = total ? (int)((double)now * 100.0 / (double)total) : -1;
    if (pct != pr->last && pr->fn) { /* curl calls often; report changes only */
        pr->last = pct;
        pr->fn(pr->ud, pct);
    }
    return 0;
}

#define FAIL(...) do { snprintf(err, cap, __VA_ARGS__); goto out; } while (0)

int soloist_install(const Config *cfg, SoloistProgressFn progress, void *ud,
                    const volatile int *cancel, char *err, size_t cap)
{
#ifndef SOLOIST_PKG
    (void)cfg; (void)progress; (void)ud; (void)cancel;
    snprintf(err, cap, "Spotify publishes no Soloist build for this computer "
                       "(only macOS on Apple silicon, Windows x64 and Linux).");
    return -1;
#else
    int rc = -1;
    HttpResponse res = { 0 };
    char archive[CFG_PATH_LEN];
    archive[0] = '\0';
    Progress pr = { progress, ud, cancel, -2 };
    HttpClient *http = http_client_new(NULL);
    if (!http)
        FAIL("Out of memory.");
    http_client_set_max_body(http, ARCHIVE_MAX);
    http_client_set_progress(http, on_progress, &pr);
    if (http_request(http, "GET", SOLOIST_CDN SOLOIST_PKG, NULL, NULL, &res) != 0) {
        if (cancel && *cancel)
            FAIL("Download cancelled.");
        FAIL("Download failed. Check the network connection.");
    }
    if (res.status != 200 || res.len == 0)
        FAIL("Download failed (HTTP %ld).", res.status);
    if (cancel && *cancel)
        FAIL("Download cancelled.");

    /* Replace, don't merge: a clean folder holds only the new build. */
    if (plat_remove_tree(cfg->soloist_dir) != 0 || plat_mkdir(cfg->soloist_dir) != 0 ||
        plat_path_join(archive, sizeof archive, cfg->soloist_dir, "download-" SOLOIST_PKG) != 0 ||
        plat_write_file_atomic(archive, res.body, res.len) != 0)
        FAIL("Could not write to %s.", cfg->soloist_dir);
    http_response_free(&res);
    int untarred = plat_untar(archive, cfg->soloist_dir);
    if (plat_remove_tree(archive) != 0)
        FAIL("Could not delete %s.", archive);
    archive[0] = '\0';
    if (untarred != 0)
        FAIL("Could not unpack Soloist (the system's tar failed).");

    char v[VERSION_LEN];
    long day;
    if (read_version(cfg, v, sizeof v) != 0 || (day = soloist_build_day(v)) < 0)
        FAIL("The downloaded Soloist does not run on this computer.");
    if (soloist_days_left(day, plat_unix_time()) <= 0)
        FAIL("Spotify's current Soloist build has already expired.");
    rc = 0;
out:
    if (archive[0])
        (void)plat_remove_tree(archive); /* failure path: best effort */
    http_response_free(&res);
    http_client_free(http);
    return rc;
#endif
}

/* --- daemon -------------------------------------------------------------- */
struct Soloist {
    plat_proc   *proc;
    plat_thread *reader;
    void       (*on_change)(void *ud);
    void        *ud;
    plat_mutex  *lock;            /* guards state, status */
    SoloistState state;
    char         status[STATUS_LEN];
};

/* state < 0 keeps the state. Notifies without the lock, so the callback
 * may query us. */
static void set(Soloist *s, int state, const char *text)
{
    plat_mutex_lock(s->lock);
    int diff = strcmp(s->status, text) != 0 || (state >= 0 && (int)s->state != state);
    if (state >= 0)
        s->state = (SoloistState)state;
    snprintf(s->status, sizeof s->status, "%s", text);
    plat_mutex_unlock(s->lock);
    if (diff && s->on_change)
        s->on_change(s->ud);
}

static int starts(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Lines look like "2026-11-06 11:55:57.392: waiting for login — ...". */
static void on_line(Soloist *s, char *line, size_t n)
{
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
        n--;
    line[n] = '\0';
    const char *msg = line;
    if (n > 25 && line[4] == '-' && line[10] == ' ' && line[23] == ':' && line[24] == ' ')
        msg = line + 25;
    if (!msg[0])
        return;
    /* The version banner and expiry countdown: soloist_installed_ok has
     * already checked both; the device id line precedes the real state. */
    if (starts(msg, "soloist ") || starts(msg, "client expires in ") || starts(msg, "ready,"))
        return;
    if (starts(msg, "waiting for login")) {
        set(s, SOLOIST_PAIRING, "waiting for pairing");
        return;
    }
    if (!strcmp(msg, "running")) {
        /* Logged after "waiting for login" too: only a stored session
         * makes it ready. */
        plat_mutex_lock(s->lock);
        int starting = s->state == SOLOIST_STARTING;
        plat_mutex_unlock(s->lock);
        if (starting)
            set(s, SOLOIST_READY, "ready");
        return;
    }
    set(s, -1, msg);
}

/* Drains the pipe (a full pipe would stall Soloist) and tracks the state.
 * Nothing is logged: output stays in memory. */
static void reader_main(void *arg)
{
    Soloist *s = arg;
    char buf[4096], line[LINE_MAX_LEN];
    size_t len = 0;
    int overflow = 0;
    long n;
    while ((n = plat_proc_read(s->proc, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            char ch = buf[i];
            if (ch == '\n') {
                if (!overflow)
                    on_line(s, line, len);
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
        on_line(s, line, len);
    /* EOF: it exited, or closed its output, which is as fatal. A natural
     * exit is reaped within moments; a stop in progress reaps it itself. */
    int code = plat_proc_wait(s->proc, 2000);
    char last[STATUS_LEN], msg[STATUS_LEN + 32];
    soloist_status(s, last, sizeof last);
    if (code == EXIT_EXPIRED) {
        set(s, SOLOIST_EXPIRED, "Soloist build expired");
        return;
    }
    if (code > 0)
        snprintf(msg, sizeof msg, "Soloist stopped (exit %d): %s", code, last);
    else
        snprintf(msg, sizeof msg, "Soloist stopped: %s", last);
    set(s, SOLOIST_STOPPED, msg);
}

Soloist *soloist_start(const Config *cfg, void (*on_change)(void *ud), void *ud)
{
    if (!cfg->soloist_key[0] || plat_mkdir(cfg->data_dir) != 0 ||
        plat_mkdir(cfg->soloist_data) != 0 || plat_mkdir(cfg->soloist_cache) != 0)
        return NULL;
    Soloist *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->on_change = on_change;
    s->ud = ud;
    s->lock = plat_mutex_new();
    if (!s->lock) {
        free(s);
        return NULL;
    }
    s->state = SOLOIST_STARTING;
    snprintf(s->status, sizeof s->status, "starting");
    /* The key can only go in argv (Soloist's documented interface). */
    const char *argv[] = {
        cfg->soloist_path,
        "-n", CFG_DEVICE_NAME,
        "-k", cfg->soloist_key,
        "-D", cfg->soloist_data,   /* device id + paired session */
        "-C", cfg->soloist_cache,
        "-z", "100",               /* MB: the smallest cap Soloist allows */
        NULL,
    };
    s->proc = spawn_soloist(cfg, argv);
    if (!s->proc) {
        plat_mutex_free(s->lock);
        free(s);
        return NULL;
    }
    s->reader = plat_thread_start(reader_main, s);
    if (!s->reader) {
        plat_proc_kill(s->proc);
        plat_mutex_free(s->lock);
        free(s);
        return NULL;
    }
    return s;
}

SoloistState soloist_state(Soloist *s)
{
    if (!s)
        return SOLOIST_STOPPED;
    plat_mutex_lock(s->lock);
    SoloistState st = s->state;
    plat_mutex_unlock(s->lock);
    return st;
}

void soloist_status(Soloist *s, char *buf, size_t cap)
{
    if (!cap)
        return;
    if (!s) {
        buf[0] = '\0';
        return;
    }
    plat_mutex_lock(s->lock);
    snprintf(buf, cap, "%s", s->status);
    plat_mutex_unlock(s->lock);
}

void soloist_stop(Soloist *s)
{
    if (!s)
        return;
    /* Terminate first so the reader sees EOF, join it, then free the proc.
     * SIGTERM is Soloist's normal shutdown (it logs "shutting down"). */
    plat_proc_terminate(s->proc);
    plat_thread_join(s->reader);
    plat_proc_kill(s->proc);
    plat_mutex_free(s->lock);
    free(s);
}
