/* Spotify Soloist: Spotify's official headless Connect client, run as the
 * local device CFG_DEVICE_NAME. Spotify forbids redistributing it, so it is
 * downloaded at runtime from Spotify's CDN into cfg->soloist_dir. Builds
 * expire SOLOIST_EXPIRY_DAYS after their build date (then exit with code
 * 10). It pairs once through Spotify Connect discovery (the user picks the
 * device in an official Spotify app on the same network) and keeps that
 * session in cfg->soloist_data; we then control it through the Web API. */
#ifndef SOLOIST_H
#define SOLOIST_H

#include <stdint.h>

#include "config.h"

#define SOLOIST_EXPIRY_DAYS 90
#define SOLOIST_RENEW_DAYS  7  /* re-download when this few days are left */

/* --- install / update ---------------------------------------------------- */
/* 1 if the binary runs (`--version`) and its build expires in more than
 * SOLOIST_RENEW_DAYS days. Runs the binary once (~40 ms); not a UI-thread
 * call. */
int soloist_installed_ok(const Config *cfg);

/* percent: 0-100, or -1 while the download size is unknown. */
typedef void (*SoloistProgressFn)(void *ud, int percent);
/* Download this platform's build (progress may be NULL), replace
 * cfg->soloist_dir with it, check that it runs, delete the archive. Blocks;
 * returns -1 early once *cancel becomes nonzero (cancel may be NULL).
 * 0 ok; -1 with a user-facing message in err (platforms without a build
 * included). Soloist must not be running. */
int soloist_install(const Config *cfg, SoloistProgressFn progress, void *ud,
                    const volatile int *cancel, char *err, size_t cap);

/* Pure helpers (exposed for tests). Days since 1970-01-01 of the
 * "(YYYYMMDD)" build date in `soloist --version` output, -1 if absent. */
long soloist_build_day(const char *version);
/* Whole days until a build from build_day expires, at unix time now
 * (counted from midnight UTC of the build date: errs early). */
long soloist_days_left(long build_day, int64_t now);

/* --- daemon -------------------------------------------------------------- */
typedef enum {
    SOLOIST_STARTING,
    SOLOIST_PAIRING,   /* logged "waiting for login": pick the device in a
                        * Spotify app. Stays set until stop; the Web API
                        * device list confirms when pairing is done. */
    SOLOIST_READY,     /* running with a stored session */
    SOLOIST_STOPPED,   /* exited; soloist_status says why */
    SOLOIST_EXPIRED    /* exited with code 10: reinstall, then start again */
} SoloistState;

typedef struct Soloist Soloist;

/* Spawn `soloist -n CFG_DEVICE_NAME -k <key> -D <data> -C <cache> -z 100`.
 * Call from the main thread (Linux ties the child to the spawning thread).
 * NULL if the key is missing, the binary is missing or the spawn fails.
 * on_change(ud) runs on the reader thread whenever the state or status
 * changes, including its exit (on_change may be NULL). Neither the argv
 * nor the key is ever logged. */
Soloist     *soloist_start(const Config *cfg, void (*on_change)(void *ud), void *ud);
SoloistState soloist_state(Soloist *s); /* NULL -> SOLOIST_STOPPED */
/* Short status for display ("starting", "waiting for pairing", "ready",
 * the last warning/error, or why it stopped). "" for NULL. */
void         soloist_status(Soloist *s, char *buf, size_t cap);
void         soloist_stop(Soloist *s); /* NULL-safe */

#endif
