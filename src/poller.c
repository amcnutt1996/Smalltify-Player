#include "poller.h"

#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "platform.h"
#include "spotify.h"

/* One quick re-fetch after a kick-triggered fetch: Spotify often reports the
 * pre-command state for a moment after a playback command. */
#define POLL_SETTLE_MS      300
#define POLL_BACKOFF_MIN_MS 2000
#define POLL_BACKOFF_MAX_MS 30000
/* Progress drift tolerated before it counts as a seek/jump. */
#define POLL_JUMP_MS        1500
/* While playing in the foreground the UI ticks every POLL_PLAYING_MS from a
 * local timer (progress is interpolated), but the network is asked only this
 * often, plus once right after the current track should end. */
#define POLL_FETCH_PLAYING_MS 5000

struct Poller {
    AuthStore   *auth;
    void       (*on_update)(void *ud);
    void        *ud;
    plat_mutex  *mu;
    plat_event  *ev;
    plat_thread *th;
    /* Everything below is guarded by mu. */
    PlaybackState state;
    uint64_t      seq;
    int           last_error;
    int           background;
    int           kicked;
    int           stop;
};

static int iabs(int v) { return v < 0 ? -v : v; }

/* Next whole second of interpolated progress, so the shown time steps
 * exactly once per tick. */
static uint64_t next_tick(uint64_t now, int progress_ms, uint64_t at)
{
    uint64_t pos = (uint64_t)(progress_ms > 0 ? progress_ms : 0) + (now > at ? now - at : 0);
    return now + POLL_PLAYING_MS - pos % POLL_PLAYING_MS;
}

/* Anything the UI shows, except progress that merely advanced as expected. */
static int state_changed(const PlaybackState *a, const PlaybackState *b)
{
    if (a->active != b->active || a->is_playing != b->is_playing ||
        a->shuffle != b->shuffle || a->repeat != b->repeat ||
        a->volume != b->volume ||
        strcmp(a->track.uri, b->track.uri) != 0 ||
        strcmp(a->device_id, b->device_id) != 0 ||
        strcmp(a->device_name, b->device_name) != 0)
        return 1;
    int expected = a->progress_ms;
    if (a->is_playing && b->fetched_at_ms > a->fetched_at_ms)
        expected += (int)(b->fetched_at_ms - a->fetched_at_ms);
    return iabs(b->progress_ms - expected) > POLL_JUMP_MS;
}

static void poll_thread(void *arg)
{
    Poller *p = arg;
    HttpAuth ha = auth_store_http(p->auth);
    HttpClient *http = http_client_new(&ha);
    PlaybackState cur;
    unsigned backoff = 0;
    int kick_fetch = 0;     /* the coming fetch was requested by poller_kick */

    for (;;) {
        /* Cadence counts from the request start so latency doesn't stretch it. */
        uint64_t started = plat_now_ms();
        int rc = http ? sp_get_playback(http, &cur) : SP_ERR_OTHER;
        uint64_t fetched = plat_now_ms();
        int notify = 0;

        plat_mutex_lock(p->mu);
        if (p->stop) {
            plat_mutex_unlock(p->mu);
            break;
        }
        if (p->last_error != rc)
            notify = 1; /* GUI shows the error state */
        p->last_error = rc;
        if (rc == SP_OK) {
            if (cur.fetched_at_ms == 0)
                cur.fetched_at_ms = fetched;
            /* While playing, publish every fetch: it re-anchors the
             * progress the UI interpolates between fetches. */
            if (state_changed(&p->state, &cur) || (cur.active && cur.is_playing)) {
                p->state = cur;
                p->seq++;
                notify = 1;
            } else {
                p->state.fetched_at_ms = cur.fetched_at_ms;
                p->state.progress_ms = cur.progress_ms;
            }
        }
        int playing = p->state.active && p->state.is_playing;
        int prog = p->state.progress_ms, dur = p->state.track.duration_ms;
        uint64_t at = p->state.fetched_at_ms;
        plat_mutex_unlock(p->mu);

        if (notify && p->on_update)
            p->on_update(p->ud);

        int settle = 0;
        if (rc == SP_OK) {
            backoff = 0;
            settle = kick_fetch;
        } else if (rc == SP_ERR_NET || rc == SP_ERR_RATE) {
            backoff = backoff ? backoff * 2 : POLL_BACKOFF_MIN_MS;
            if (backoff > POLL_BACKOFF_MAX_MS)
                backoff = POLL_BACKOFF_MAX_MS;
        } else {
            backoff = 0;
            playing = 0; /* auth/other errors: idle rate, never hammer */
        }
        kick_fetch = 0;

        /* Fetch again right after the track should end instead of up to
         * POLL_FETCH_PLAYING_MS late; once past the end, at most 1/s. */
        uint64_t end_due = UINT64_MAX;
        if (playing && dur > 0)
            end_due = dur > prog ? at + (uint64_t)(dur - prog) + POLL_SETTLE_MS
                                 : fetched + POLL_PLAYING_MS;
        uint64_t tick = next_tick(fetched, prog, at);

        /* Sleep until the deadline, waking the UI on each tick; wake-ups for
         * background changes just recompute it, kicks and stop end the wait. */
        for (;;) {
            plat_mutex_lock(p->mu);
            int stop = p->stop, kicked = p->kicked, bg = p->background;
            p->kicked = 0;
            plat_mutex_unlock(p->mu);
            if (stop)
                goto out;
            if (kicked) {
                kick_fetch = 1;
                break;
            }
            int ticking = playing && !bg;
            uint64_t due;
            if (backoff)
                due = started + backoff;
            else if (settle)
                due = fetched + POLL_SETTLE_MS; /* timed from when the state arrived */
            else if (ticking)
                due = started + POLL_FETCH_PLAYING_MS < end_due ? started + POLL_FETCH_PLAYING_MS : end_due;
            else
                due = started + POLL_IDLE_MS;
            uint64_t now = plat_now_ms();
            if (now >= due)
                break;
            if (ticking && now >= tick) {
                if (p->on_update)
                    p->on_update(p->ud);
                tick = next_tick(now, prog, at);
                continue;
            }
            uint64_t until = ticking && tick < due ? tick : due;
            plat_event_wait(p->ev, (unsigned)(until - now));
        }
    }
out:
    http_client_free(http);
}

Poller *poller_start(AuthStore *auth, void (*on_update)(void *ud), void *ud)
{
    Poller *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->auth = auth;
    p->on_update = on_update;
    p->ud = ud;
    p->last_error = SP_OK;
    p->state.volume = -1;
    p->mu = plat_mutex_new();
    p->ev = plat_event_new();
    if (p->mu && p->ev)
        p->th = plat_thread_start(poll_thread, p);
    if (!p->th) {
        if (p->ev)
            plat_event_free(p->ev);
        if (p->mu)
            plat_mutex_free(p->mu);
        free(p);
        return NULL;
    }
    return p;
}

int poller_snapshot(Poller *p, PlaybackState *out, uint64_t *seq)
{
    int fresh = 0;
    plat_mutex_lock(p->mu);
    if (!seq || p->seq > *seq) {
        *out = p->state;
        if (seq)
            *seq = p->seq;
        fresh = 1;
    }
    plat_mutex_unlock(p->mu);
    return fresh;
}

void poller_kick(Poller *p)
{
    plat_mutex_lock(p->mu);
    p->kicked = 1;
    plat_mutex_unlock(p->mu);
    plat_event_signal(p->ev);
}

void poller_set_background(Poller *p, int background)
{
    plat_mutex_lock(p->mu);
    p->background = background != 0;
    plat_mutex_unlock(p->mu);
    plat_event_signal(p->ev);
}

int poller_last_error(Poller *p)
{
    plat_mutex_lock(p->mu);
    int e = p->last_error;
    plat_mutex_unlock(p->mu);
    return e;
}

/* Returns as soon as the thread notices the flag. If a request is in flight
 * it cannot be interrupted, so this waits for it: at most the curl total
 * timeout (15 s, http.c). A 401 refresh or 429 Retry-After inside http.c
 * retries once, so the rare worst case is one more request on top. */
void poller_signal_stop(Poller *p)
{
    if (!p)
        return;
    plat_mutex_lock(p->mu);
    p->stop = 1;
    plat_mutex_unlock(p->mu);
    plat_event_signal(p->ev);
}

void poller_stop(Poller *p)
{
    if (!p)
        return;
    poller_signal_stop(p);
    plat_thread_join(p->th);
    plat_event_free(p->ev);
    plat_mutex_free(p->mu);
    free(p);
}
