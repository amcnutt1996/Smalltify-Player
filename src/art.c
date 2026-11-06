/* Album art loader: one worker thread, one pending request (latest wins),
 * bytes kept in memory only. Two buffers: `ready` is filled by the worker,
 * `taken` is the one art_take handed out, so the UI's pointer stays valid
 * until its next art_take regardless of what the worker does meanwhile. */
#include "art.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "model.h"
#include "platform.h"

#define ART_MAX_BYTES (2u * 1024u * 1024u) /* album art is ~5-60 KB; cap junk */
/* Nothing to do until art_request/art_stop signal: effectively no timeout
 * (plat_event_wait computes its deadline in 64 bits). */
#define ART_IDLE_WAIT_MS 0xFFFFFFFFu

typedef struct {
    unsigned char *data;
    size_t len;
} ArtBuf;

struct ArtLoader {
    void (*on_ready)(void *ud);
    void *ud;
    plat_thread *thread;
    plat_event *wake;
    plat_mutex *lock;
    /* Guarded by lock. */
    int quit;
    int has_pending;
    char pending[SP_URL_LEN];
    char current[SP_URL_LEN]; /* last URL requested and accepted */
    ArtBuf ready;             /* newest bytes not yet taken */
    uint64_t ready_seq;
    /* Owned by the art_take caller's thread. */
    ArtBuf taken;
};

static void buf_free(ArtBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
}

/* Publish bytes (NULL/0 = clear) unless a newer request superseded url. */
static void publish(ArtLoader *a, const char *url, unsigned char *data, size_t len)
{
    int fire = 0;

    plat_mutex_lock(a->lock);
    if (!a->quit && !a->has_pending && strcmp(a->current, url) == 0) {
        buf_free(&a->ready);
        a->ready.data = data;
        a->ready.len = len;
        a->ready_seq++;
        data = NULL;
        fire = 1;
    }
    plat_mutex_unlock(a->lock);
    free(data);
    if (fire && a->on_ready)
        a->on_ready(a->ud);
}

static void fetch(ArtLoader *a, HttpClient *http, const char *url)
{
    HttpResponse r;
    unsigned char *copy = NULL;
    size_t len = 0;

    if (url[0] && http_request(http, "GET", url, NULL, NULL, &r) == 0) {
        if (r.status == 200 && r.body && r.len > 0) {
            /* Take over the receive buffer: no copy needed. */
            copy = (unsigned char *)r.body;
            len = r.len;
            r.body = NULL;
            r.len = 0;
        }
        http_response_free(&r);
    }
    /* Failure publishes "no art" so a stale cover never lingers. */
    publish(a, url, copy, len);
}

static void worker(void *arg)
{
    ArtLoader *a = arg;
    HttpClient *http = http_client_new(NULL);
    /* Junk larger than any cover aborts mid-download, not after it. */
    http_client_set_max_body(http, ART_MAX_BYTES);

    for (;;) {
        char url[SP_URL_LEN];
        int have = 0;

        plat_event_wait(a->wake, ART_IDLE_WAIT_MS);
        plat_mutex_lock(a->lock);
        if (a->quit) {
            plat_mutex_unlock(a->lock);
            break;
        }
        if (a->has_pending) {
            memcpy(url, a->pending, sizeof url);
            a->has_pending = 0;
            have = 1;
        }
        plat_mutex_unlock(a->lock);
        if (have && http)
            fetch(a, http, url);
    }
    http_client_free(http);
}

ArtLoader *art_start(void (*on_ready)(void *ud), void *ud)
{
    ArtLoader *a = calloc(1, sizeof *a);

    if (!a)
        return NULL;
    a->on_ready = on_ready;
    a->ud = ud;
    a->wake = plat_event_new();
    a->lock = plat_mutex_new();
    if (a->wake && a->lock)
        a->thread = plat_thread_start(worker, a);
    if (!a->thread) {
        plat_event_free(a->wake);
        plat_mutex_free(a->lock);
        free(a);
        return NULL;
    }
    return a;
}

void art_request(ArtLoader *a, const char *url)
{
    if (!a)
        return;
    if (!url)
        url = "";
    plat_mutex_lock(a->lock);
    /* current already reflects the newest pending request, so one compare
     * covers both "same as current" and "same as pending". */
    if (strcmp(a->current, url) != 0) {
        snprintf(a->current, sizeof a->current, "%s", url);
        snprintf(a->pending, sizeof a->pending, "%s", url);
        a->has_pending = 1;
        plat_event_signal(a->wake);
    }
    plat_mutex_unlock(a->lock);
}

int art_take(ArtLoader *a, const unsigned char **data, size_t *len, uint64_t *seq)
{
    int got = 0;

    if (!a || !data || !len || !seq)
        return 0;
    plat_mutex_lock(a->lock);
    if (a->ready_seq != *seq) {
        buf_free(&a->taken);
        a->taken = a->ready;
        a->ready.data = NULL;
        a->ready.len = 0;
        *seq = a->ready_seq;
        got = 1;
    }
    plat_mutex_unlock(a->lock);
    if (got) {
        *data = a->taken.data;
        *len = a->taken.len;
    }
    return got;
}

void art_signal_stop(ArtLoader *a)
{
    if (!a)
        return;
    plat_mutex_lock(a->lock);
    a->quit = 1;
    plat_mutex_unlock(a->lock);
    plat_event_signal(a->wake);
}

void art_stop(ArtLoader *a)
{
    if (!a)
        return;
    art_signal_stop(a);
    plat_thread_join(a->thread);
    plat_event_free(a->wake);
    plat_mutex_free(a->lock);
    buf_free(&a->ready);
    buf_free(&a->taken);
    free(a);
}
