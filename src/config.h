/* App configuration + token persistence. Everything lives in the app folder
 * (plat_app_dir: the exe's directory, or Contents/Resources in a macOS
 * bundle):
 *   <app>/.env                 SPOTIFY_CLIENT_ID=..., SOLOIST_API_KEY=...,
 *                              UI_FONT_SIZE=... (user-edited)
 *   <app>/soloist/             Spotify Soloist, downloaded at runtime
 *   <app>/data/token.json      OAuth tokens (owner-only perms)
 *   <app>/data/soloist/        Soloist device state + paired session
 *   <app>/data/soloist-cache/  Soloist playback cache (100 MB cap)
 *   <app>/data/soloist-home/   Soloist's HOME/TMPDIR, so the OS caches it
 *                              writes stay here too (a cache: kept on logout)
 * Nothing is ever written outside <app>. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define CFG_PATH_LEN 1024
#define CFG_REDIRECT_PORT 8888
#define CFG_DEVICE_NAME "Spotify C Player" /* prefix of Config.device_name */
#define CFG_DEVICE_NAME_LEN 64

typedef struct {
    char client_id[128];             /* "" if not configured yet */
    char soloist_key[256];           /* "" if not configured; secret, never log */
    int  read_only;                  /* app folder not writable (e.g. a translocated
                                        or read-only-volume .app): data can't be saved */
    int  font_size;                  /* CFG_FONT_*, from UI_FONT_SIZE= */
    char app_dir[CFG_PATH_LEN];
    char env_path[CFG_PATH_LEN];     /* <app>/.env */
    char data_dir[CFG_PATH_LEN];
    char token_path[CFG_PATH_LEN];
    char soloist_dir[CFG_PATH_LEN];  /* <app>/soloist */
    char soloist_path[CFG_PATH_LEN]; /* <app>/soloist/soloist[.exe] */
    char soloist_data[CFG_PATH_LEN]; /* <app>/data/soloist */
    char soloist_cache[CFG_PATH_LEN];/* <app>/data/soloist-cache */
    char soloist_home[CFG_PATH_LEN]; /* <app>/data/soloist-home */
    char redirect_uri[128];          /* http://127.0.0.1:8888/callback */
    char device_name[CFG_DEVICE_NAME_LEN]; /* "Spotify C Player (<host>)": our Connect
                                        device, unique per computer */
    char paired_path[CFG_PATH_LEN];  /* <app>/data/soloist-paired: marker */
    char name_path[CFG_PATH_LEN];    /* <app>/data/device-name: device_name as paired */
} Config;

typedef struct {
    char    access[2048];
    char    refresh[1024];
    int64_t expires_at;              /* unix seconds */
    char    scope[1024];             /* scopes Spotify granted, space-separated;
                                        "" for tokens saved before it was recorded */
} Token;

/* Fill paths and read .env. The env vars SPOTIFY_CLIENT_ID and
 * SOLOIST_API_KEY override their .env lines. Creates data_dir. Returns 0
 * ok, -1 on fatal error (paths). A missing client id or key is NOT an
 * error: callers check client_id[0] and soloist_key[0]. */
int config_load(Config *c);

/* Pure, exposed for tests: CFG_DEVICE_NAME + " (" + host + ")", the host
 * cut at its first '.', control characters dropped and shortened (at a
 * UTF-8 boundary) so the whole name fits cap; just CFG_DEVICE_NAME when
 * host is empty. */
void config_device_name(const char *host, char *out, size_t cap);

/* The Soloist device was paired once (marker file data/soloist-paired). */
int  config_paired(const Config *c);
/* Create the marker and save device_name to data/device-name, so the
 * Connect identity survives a hostname change (on), or delete both (off).
 * 0 ok, -1 error. */
int  config_set_paired(const Config *c, int on);
/* Soloist has run here before (its -D dir holds a device id), so it may
 * hold a stored session even without the marker (older builds). */
int  config_soloist_used(const Config *c);

/* 0 ok, -1 missing/corrupt. */
int config_load_token(const Config *c, Token *t);
int config_save_token(const Config *c, const Token *t);
/* Remove the token file, Soloist's paired session and the paired marker
 * (logout). */
int config_clear_login(const Config *c);

/* Look up key in .env text (KEY=VALUE lines, '#' comments, optional
 * quotes, surrounding whitespace, CRLF, UTF-8 BOM; the last assignment
 * wins; unknown keys are ignored). Pure, exposed for tests.
 * 0 found (out filled), -1 absent or value longer than cap-1. */
int config_env_lookup(const char *text, const char *key, char *out, size_t cap);

/* Pure, exposed for tests: text with every line that assigns key replaced
 * by "key=value" (its line ending kept), or that line appended if there is
 * none; every other line stays verbatim. malloc'd; NULL if out of memory. */
char *config_env_set(const char *text, const char *key, const char *value);

/* Write key=value into <app>/.env through config_env_set: an atomic
 * replace with owner-only perms, created if missing. value must be
 * printable ASCII without blanks, quotes or a leading '#', so it reads back
 * unchanged. Does not reload c. Never logs the value. 0 ok, -1 error. */
int config_set_env_value(const Config *c, const char *key, const char *value);

/* GUI text size: UI_FONT_SIZE= small, default, large or extra-large in
 * .env; like the other settings, an environment variable of that name wins. */
enum { CFG_FONT_SMALL, CFG_FONT_DEFAULT, CFG_FONT_LARGE, CFG_FONT_XLARGE, CFG_FONT_SIZES };
/* Pure, exposed for tests: the CFG_FONT_* that value names, ASCII case
 * ignored; CFG_FONT_DEFAULT for NULL, "" and anything else. */
int config_font_size(const char *value);
/* The UI_FONT_SIZE= value for a CFG_FONT_* (NULL if out of range), which
 * config_set_env_value accepts and config_font_size reads back. */
const char *config_font_size_value(int size);

#endif
