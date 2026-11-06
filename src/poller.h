/* Background thread that fetches playback state and hands snapshots to the
 * UI under a mutex. While playing in the foreground it wakes the UI once per
 * second of progress (no request) and fetches every 5 s plus right after the
 * track ends; paused, idle or in the background it fetches every 5 s. */
#ifndef POLLER_H
#define POLLER_H

#include <stdint.h>
#include "auth.h"
#include "model.h"

#define POLL_PLAYING_MS 1000 /* UI progress tick while playing (no fetch) */
#define POLL_IDLE_MS    5000

typedef struct Poller Poller;

/* on_update (may be NULL) is called from the poller thread after each
 * snapshot change and on every progress tick -- the GUI uses it to wake its
 * event loop. */
Poller *poller_start(AuthStore *auth, void (*on_update)(void *ud), void *ud);
/* Copy the latest state. Returns 1 if newer than *seq (and updates *seq),
 * else 0. Pass seq NULL to always copy. */
int     poller_snapshot(Poller *p, PlaybackState *out, uint64_t *seq);
/* Fetch again soon (call after a playback command). */
void    poller_kick(Poller *p);
/* Background/minimized: 1 = use idle interval even while playing. */
void    poller_set_background(Poller *p, int background);
/* Last error (SP_ERR_*) from fetching, SP_OK if fine. */
int     poller_last_error(Poller *p);
void    poller_stop(Poller *p); /* joins thread, frees; NULL-safe */

#endif
