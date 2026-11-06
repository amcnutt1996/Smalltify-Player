/* Spotify Soloist: Spotify's official headless Connect client, run as the
 * local device cfg->device_name. Spotify forbids redistributing it, so it is
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
/* 1 if Spotify publishes a build for this OS/CPU. */
int soloist_supported(void);
/* 1 unless a download is due: the binary is missing, doesn't run or has
 * expired, or it is within SOLOIST_RENEW_DAYS of expiry and the CDN was
 * not asked in the last day. Runs the binary once (~40 ms); not a
 * UI-thread call. */
int soloist_up_to_date(const Config *cfg);

/* percent: 0-100, or -1 while the download size is unknown. */
typedef void (*SoloistProgressFn)(void *ud, int percent);
/* Download this platform's build (progress may be NULL) into
 * <soloist_dir>.new, check that it runs, and swap it in only if it is
 * newer than the installed one (the working build is never removed
 * first). Records the check time (data/soloist-check). Blocks; returns -1
 * early once *cancel becomes nonzero (cancel may be NULL). 0 ok (new build,
 * or the same still-valid one kept); -1 with a user-facing message in err
 * (platforms without a build included). Soloist must not be running. */
int soloist_install(const Config *cfg, SoloistProgressFn progress, void *ud,
                    const volatile int *cancel, char *err, size_t cap);

/* Pure, exposed for tests: should we download, given the installed
 * build's day (-1 = none/broken), the last check (unix time, 0 = never)
 * and now? */
int soloist_need_download(long installed_day, int64_t last_check, int64_t now);
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

/* Spawn `soloist -n <cfg->device_name> -k <key> -D <data> -C <cache> -z 100`.
 * Call from the main thread (Linux ties the child to the spawning thread).
 * NULL if the key is missing, the binary is missing or the spawn fails.
 * on_change(ud) runs on the reader thread whenever the state or status
 * changes, including its exit (on_change may be NULL). Neither the argv
 * nor the key is ever logged. HOME, CFFIXED_USER_HOME and TMPDIR (Win32:
 * USERPROFILE, APPDATA, LOCALAPPDATA, TEMP, TMP) point into
 * cfg->soloist_home so its OS-level caches stay in the app folder. */
Soloist     *soloist_start(const Config *cfg, void (*on_change)(void *ud), void *ud);
SoloistState soloist_state(Soloist *s); /* NULL -> SOLOIST_STOPPED */
/* Short status for display ("starting", "waiting for pairing", "ready",
 * the last warning/error, or why it stopped). "" for NULL. */
void         soloist_status(Soloist *s, char *buf, size_t cap);
void         soloist_stop(Soloist *s); /* NULL-safe */

#endif
