/* Plain data structures shared by every module. Fixed-size buffers: no heap
 * ownership, safe to copy by value between threads. Strings are always
 * NUL-terminated (truncated if the source is longer). */
#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>

#define SP_ID_LEN   64
#define SP_URI_LEN  96
#define SP_NAME_LEN 256
#define SP_URL_LEN  256

typedef struct {
    char id[SP_ID_LEN];
    char uri[SP_URI_LEN];
    char name[SP_NAME_LEN];
    char artists[SP_NAME_LEN]; /* "Artist A, Artist B" */
    char album[SP_NAME_LEN];
    char art_url[SP_URL_LEN];  /* smallest image >= 300px (else largest >= 64px), "" if none */
    int  duration_ms;
} Track;

typedef enum { REPEAT_OFF = 0, REPEAT_CONTEXT = 1, REPEAT_TRACK = 2 } RepeatMode;

typedef struct {
    int        active;        /* 0 = nothing playing anywhere (HTTP 204) */
    int        is_playing;
    int        progress_ms;
    int        shuffle;
    RepeatMode repeat;
    int        volume;        /* 0-100, -1 if unknown */
    char       device_id[SP_ID_LEN];
    char       device_name[SP_NAME_LEN];
    Track      track;
    uint64_t   fetched_at_ms; /* plat_now_ms() when fetched; for local progress interpolation */
} PlaybackState;

typedef struct {
    char id[SP_ID_LEN];
    char name[SP_NAME_LEN];
    char type[32];            /* "Computer", "Smartphone", ... */
    int  is_active;
    int  volume;              /* 0-100, -1 if unknown */
} Device;

typedef struct {
    char id[SP_ID_LEN];
    char uri[SP_URI_LEN];
    char name[SP_NAME_LEN];
    int  track_count;
} Playlist;

#endif
