/* App configuration + token persistence. Everything lives in the app folder:
 *   <exe_dir>/.env                 SPOTIFY_CLIENT_ID=...   (user-edited)
 *   <exe_dir>/librespot[.exe]      bundled playback engine
 *   <exe_dir>/data/token.json      OAuth tokens (owner-only perms)
 *   <exe_dir>/data/librespot/      librespot credential cache (no audio cache)
 * Nothing is ever written outside <exe_dir>. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define CFG_PATH_LEN 1024
#define CFG_REDIRECT_PORT 8888
#define CFG_DEVICE_NAME "Spotify C Player"

typedef struct {
    char client_id[128];             /* "" if not configured yet */
    char app_dir[CFG_PATH_LEN];
    char data_dir[CFG_PATH_LEN];
    char token_path[CFG_PATH_LEN];
    char librespot_path[CFG_PATH_LEN];
    char librespot_cache[CFG_PATH_LEN];
    char redirect_uri[128];          /* http://127.0.0.1:8888/callback */
} Config;

typedef struct {
    char    access[2048];
    char    refresh[1024];
    int64_t expires_at;              /* unix seconds */
} Token;

/* Fill paths and read .env. Env var SPOTIFY_CLIENT_ID overrides .env.
 * Creates data_dir. Returns 0 ok, -1 on fatal error (paths). A missing
 * client id is NOT an error: callers check client_id[0]. */
int config_load(Config *c);

/* 0 ok, -1 missing/corrupt. */
int config_load_token(const Config *c, Token *t);
int config_save_token(const Config *c, const Token *t);
/* Remove token file + librespot cache (logout). */
int config_clear_login(const Config *c);

/* Look up key in .env text (KEY=VALUE lines, '#' comments, optional
 * quotes, surrounding whitespace, CRLF, UTF-8 BOM; the last assignment
 * wins; unknown keys are ignored). Pure, exposed for tests.
 * 0 found (out filled), -1 absent or value longer than cap-1. */
int config_env_lookup(const char *text, const char *key, char *out, size_t cap);

#endif
