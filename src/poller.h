/* Background thread that fetches playback state and hands snapshots to the
 * UI under a mutex. Adaptive interval: fast while playing, slow while
 * paused/idle, so the app sleeps most of the time. */
#ifndef POLLER_H
#define POLLER_H

#include <stdint.h>
#include "auth.h"
#include "model.h"

#define POLL_PLAYING_MS 1000
#define POLL_IDLE_MS    5000

typedef struct Poller Poller;

/* on_update (may be NULL) is called from the poller thread after each
 * snapshot change -- the GUI uses it to wake its event loop. */
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
