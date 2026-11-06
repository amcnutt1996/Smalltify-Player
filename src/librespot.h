/* Supervises the bundled librespot binary (<exe_dir>/librespot): spawn with
 * our access token, read its output on a background thread, kill on exit.
 * It shows up as Spotify Connect device CFG_DEVICE_NAME. */
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
void       librespot_stop(Librespot *l); /* NULL-safe */

#endif
