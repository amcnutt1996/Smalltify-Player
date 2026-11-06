/* Supervises the bundled librespot binary (<exe_dir>/librespot): spawn,
 * read its output on a background thread, kill on exit. It shows up as
 * Spotify Connect device CFG_DEVICE_NAME. librespot signs in through its
 * own OAuth once (see librespot.c) and caches credentials in
 * cfg->librespot_cache. */
#ifndef LIBRESPOT_H
#define LIBRESPOT_H

#include "config.h"
#include "auth.h"

typedef struct Librespot Librespot;

/* NULL if binary missing or spawn fails. */
Librespot *librespot_start(const Config *cfg, AuthStore *auth);
int        librespot_running(Librespot *l);
/* Copy last meaningful output line (errors/status) for display. */
void       librespot_status(Librespot *l, char *buf, size_t cap);
/* While librespot waits for its one-time OAuth redirect: copy the
 * authorize URL and return 1 (librespot already opened it in the default
 * browser; show it as a fallback). 0 otherwise (not needed, done, exited,
 * or cap too small). Cleared once librespot authenticates or exits. */
int        librespot_login_url(Librespot *l, char *buf, size_t cap);
/* 1 if cached librespot credentials exist (no browser sign-in needed). */
int        librespot_has_credentials(const Config *cfg);
void       librespot_stop(Librespot *l); /* NULL-safe */

#endif
