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

/* What an Item is (Spotify's "type" field). */
typedef enum {
    ITEM_TRACK,
    ITEM_EPISODE,
    ITEM_ALBUM,
    ITEM_ARTIST,
    ITEM_PLAYLIST,
    ITEM_SHOW
} ItemKind;

/* One row of any list: your library, search results, the queue, or the
 * contents of a playlist, album, artist or show (sp_get_list, sp_search).
 * Fields a kind doesn't have are "" (text), 0 (duration_ms) or -1 (count). */
typedef struct {
    ItemKind kind;
    char id[SP_ID_LEN];          /* "" for local files */
    char uri[SP_URI_LEN];        /* play, queue or save it with this */
    char name[SP_NAME_LEN];
    char sub[SP_NAME_LEN];       /* second line: artists (track, album), show
                                    (episode, if Spotify sent it), publisher
                                    (show; Spotify stopped sending it in Feb 2026),
                                    owner (playlist), "" (artist) */
    char album[SP_NAME_LEN];     /* track: its album; episode: its show */
    char album_uri[SP_URI_LEN];  /* the same as a URI (open it with SP_LIST_ALBUM
                                    or SP_LIST_SHOW) */
    char artist_uri[SP_URI_LEN]; /* track, album: the first artist (SP_LIST_ARTIST) */
    char art_url[SP_URL_LEN];    /* cover or photo, picked like Track.art_url; "" if none */
    char date[16];               /* album, episode: release date, "YYYY[-MM[-DD]]" */
    int  duration_ms;            /* track, episode */
    int  count;                  /* album: tracks; playlist: items; show: episodes;
                                    -1 unknown (Spotify shows no contents of a
                                    playlist you neither own nor collaborate on) */
    int  playable;               /* 0: Spotify can't play it (a local file, not
                                    available in your country, or removed) */
} Item;

/* The signed-in account (sp_get_me). */
typedef struct {
    char id[SP_ID_LEN];          /* Spotify user id */
    char name[SP_NAME_LEN];      /* display name; may be "" */
    char liked_uri[SP_URI_LEN];  /* Liked Songs as a playable context:
                                    "spotify:user:<id>:collection" */
} User;

#endif
