/* Spotify Web API calls. Each parses JSON (cJSON) into model.h structs.
 * Return codes: SP_OK, or negative SP_ERR_*. Pass the calling thread's own
 * HttpClient (created with auth_store_http()). */
#ifndef SPOTIFY_H
#define SPOTIFY_H

#include "http.h"
#include "model.h"

#ifndef SP_API /* a test build may point it at a local fake */
#define SP_API "https://api.spotify.com/v1"
#endif

enum {
    SP_OK           = 0,
    SP_ERR_NET      = -1, /* transport failure */
    SP_ERR_AUTH     = -2, /* 401 after refresh / not logged in */
    SP_ERR_NODEVICE = -3, /* 404 NO_ACTIVE_DEVICE */
    SP_ERR_PREMIUM  = -4, /* 403 premium required / restricted */
    SP_ERR_RATE     = -5, /* 429 */
    SP_ERR_PARSE    = -6,
    SP_ERR_OTHER    = -7,
    SP_ERR_SCOPE    = -8  /* 403 "Insufficient client scope": the sign-in predates a
                             permission this call needs (auth_store_needs_reconsent);
                             signing in again fixes it */
};
const char *sp_strerror(int err);

/* --- state (now playing) ------------------------------------------------ */
/* out->active = 0 when nothing is playing (204). */
int sp_get_playback(HttpClient *c, PlaybackState *out);
/* Parse a /me/player JSON body (no network; used by tests). Leaves
 * fetched_at_ms 0. SP_OK, or SP_ERR_PARSE if json is not a JSON object. */
int sp_parse_playback(const char *json, PlaybackState *out);

/* --- playback control (device_id NULL = currently active device) -------- */
int sp_play(HttpClient *c, const char *device_id);
int sp_pause(HttpClient *c);
int sp_next(HttpClient *c);
int sp_prev(HttpClient *c);
int sp_seek(HttpClient *c, int position_ms);
int sp_set_volume(HttpClient *c, int percent);
int sp_set_shuffle(HttpClient *c, int on);
int sp_set_repeat(HttpClient *c, RepeatMode mode);

/* --- devices ------------------------------------------------------------ */
#define SP_MAX_DEVICES 32 /* enough for any account; bounds stack arrays */
/* *count = devices stored; extra devices beyond cap are dropped. */
int sp_get_devices(HttpClient *c, Device *out, int cap, int *count);
/* Parse a /me/player/devices body (no network; used by tests). SP_OK, or
 * SP_ERR_PARSE if json is not an object with a "devices" array. */
int sp_parse_devices(const char *json, Device *out, int cap, int *count);
int sp_transfer(HttpClient *c, const char *device_id, int play);
/* Find device by exact name (e.g. cfg->device_name). SP_OK and fills out. */
int sp_find_device(HttpClient *c, const char *name, Device *out);

/* --- playing something ---------------------------------------------------- */
/* Play context_uri (an album, playlist, artist or show, or User.liked_uri for
 * Liked Songs) from item_uri, a track or episode in it (NULL = from the
 * start; Spotify takes it for albums, playlists and Liked Songs), or with
 * context_uri NULL just item_uri. "" counts as NULL. device_id NULL = the
 * active device. SP_ERR_OTHER when both URIs are NULL. */
int sp_play_uri(HttpClient *c, const char *device_id, const char *context_uri,
                const char *item_uri);
/* Pure, exposed for tests: the JSON body sp_play_uri sends,
 * {"context_uri":C,"offset":{"uri":I}} or {"uris":[I]}. 0 ok, -1 when both
 * URIs are NULL/"" or it does not fit in cap. */
int sp_play_body(const char *context_uri, const char *item_uri, char *out, size_t cap);
/* Where sp_play_smart played. */
enum { SP_ON_ACTIVE, SP_ON_LOCAL, SP_ON_OTHER };
/* Pure, exposed for tests: the index in devs to play on, or -1, and in
 * *where the reason. Candidates need an id. local_first: our own device
 * (named local_name; NULL = none) wins over the active one, else the
 * active one wins; then any other available device (Spotify's order). */
int sp_pick_device(const Device *devs, int n, const char *local_name, int local_first, int *where);
/* Play context_uri/item_uri as sp_play_uri does (both NULL = resume) on the
 * device sp_pick_device picks from the device list; with local_first unset,
 * an active device is tried first without listing. An inactive device gets
 * the URIs, or a transfer with play=true (resume). on and where (both may
 * be NULL) say where. SP_ERR_NODEVICE when there is no device at all. */
int sp_play_smart(HttpClient *c, const char *context_uri, const char *item_uri,
                  const char *local_name, int local_first, Device *on, int *where);
/* Add a track or episode to the end of the active device's queue. */
int sp_queue_add(HttpClient *c, const char *uri);

/* --- you -------------------------------------------------------------------- */
/* The signed-in account (GET /me): its id names the Liked Songs context. */
int sp_get_me(HttpClient *c, User *out);
/* Pure, exposed for tests: parse a /me body. SP_ERR_PARSE without an id. */
int sp_parse_me(const char *json, User *out);

/* --- your library: the heart button, saved albums and shows, followed artists */
#define SP_LIBRARY_MAX 40 /* URIs per call (Spotify's limit) */
/* Save (save=1) or remove (save=0) n URIs (1..SP_LIBRARY_MAX) in one call
 * (PUT/DELETE /me/library): tracks (Liked Songs), albums, episodes, shows,
 * playlists, and artists (follow/unfollow). Needs user-library-modify, plus
 * user-follow-modify for artists; an older sign-in gets SP_ERR_SCOPE. */
int sp_library_set(HttpClient *c, const char *const *uris, int n, int save);
/* saved[i] = 1 if uris[i] is saved (followed, for artists), else 0
 * (GET /me/library/contains). Artist URIs need user-follow-read. */
int sp_library_contains(HttpClient *c, const char *const *uris, int n, int *saved);
/* Pure, exposed for tests: prefix + the n URIs (1..SP_LIBRARY_MAX), each
 * percent-encoded, joined by ','; e.g. "/me/library?uris=spotify%3Atrack%3Aa".
 * The library calls and sp_queue_add ("/me/player/queue?uri=") build their
 * paths with it. 0 ok, -1 on a bad n, an empty URI, or too long for cap. */
int sp_uris_path(const char *prefix, const char *const *uris, int n, char *out, size_t cap);
/* Pure, exposed for tests: parse a /me/library/contains body (an array of
 * booleans) into saved[0..n-1]. SP_ERR_PARSE if it has fewer than n. */
int sp_parse_contains(const char *json, int *saved, int n);

/* --- lists: your library, the queue, and what's inside an item ------------ */
typedef enum {
    SP_LIST_LIKED,     /* Liked Songs, newest first (play them with User.liked_uri) */
    SP_LIST_PLAYLISTS, /* your playlists, owned and followed */
    SP_LIST_ALBUMS,    /* saved albums */
    SP_LIST_ARTISTS,   /* followed artists (cursor paging; needs user-follow-read) */
    SP_LIST_SHOWS,     /* saved podcasts */
    SP_LIST_EPISODES,  /* saved episodes */
    SP_LIST_RECENT,    /* recently played tracks, newest first (cursor paging;
                          needs user-read-recently-played; Spotify lists no episodes) */
    SP_LIST_QUEUE,     /* what plays now, then up next (one page) */
    SP_LIST_PLAYLIST,  /* id: a playlist's tracks and episodes (see SpPage.restricted) */
    SP_LIST_ALBUM,     /* id: an album's tracks (without album, album_uri, art_url:
                          they are the album's own) */
    SP_LIST_ARTIST,    /* id: an artist's albums, singles and compilations
                          (SP_ARTIST_PAGE_MAX per request) */
    SP_LIST_SHOW,      /* id: a show's episodes, newest first (without album/sub) */
    SP_LIST_COUNT
} SpList;

#define SP_PAGE_MAX        50 /* list entries per request (Spotify's limit) */
#define SP_ARTIST_PAGE_MAX 10 /* SP_LIST_ARTIST's limit (Spotify answers 400 above it) */
#define SP_CURSOR_LEN      64

/* A position in a list. Zero it for the first page. On SP_OK, sp_get_list and
 * sp_search move it to the next page, so calling again while `more` is set
 * walks the whole list; on an error it is left as it was (retry with it). */
typedef struct {
    int  offset;                /* in/out: first entry of the page (offset-paged lists) */
    char cursor[SP_CURSOR_LEN]; /* in/out: SP_LIST_ARTISTS/SP_LIST_RECENT position, "" = start */
    int  total;                 /* out: entries in the whole list, -1 unknown (search
                                   totals have been rough since Feb 2026) */
    int  more;                  /* out: another page follows */
    int  restricted;            /* out, SP_LIST_PLAYLIST: Spotify refuses the items of a
                                   playlist you neither own nor collaborate on (since
                                   Feb 2026), so the page is empty; it still plays as a
                                   context */
    int  now_playing;           /* out, SP_LIST_QUEUE: out[0] is what plays now, the
                                   rest is up next */
} SpPage;

/* One page of list `which` into out (at most cap Items; the request asks for
 * min(cap, SP_PAGE_MAX or SP_ARTIST_PAGE_MAX)). id: the playlist, album,
 * artist or show, as a bare ID or a Spotify URI; ignored by the other lists.
 * Removed or unavailable entries (null, or a placeholder without name and
 * URI) are skipped, so count can be below a full page while more is set;
 * local files stay, with playable 0. SP_ERR_SCOPE when the sign-in lacks
 * the list's permission, else as the other calls. */
int sp_get_list(HttpClient *c, SpList which, const char *id, SpPage *page,
                Item *out, int cap, int *count);
/* Pure, exposed for tests: parse a response body of list `which` and move
 * page on like sp_get_list. Entries beyond cap are left for the next page
 * (SP_LIST_RECENT drops them: its cursor is a time). Takes old and new (Feb
 * 2026) field names. SP_OK, or SP_ERR_PARSE if json is not that list's shape. */
int sp_parse_list(SpList which, const char *json, SpPage *page, Item *out, int cap, int *count);

/* --- search ---------------------------------------------------------------- */
enum {
    SP_SEARCH_TRACK    = 1 << 0,
    SP_SEARCH_ALBUM    = 1 << 1,
    SP_SEARCH_ARTIST   = 1 << 2,
    SP_SEARCH_PLAYLIST = 1 << 3,
    SP_SEARCH_SHOW     = 1 << 4,
    SP_SEARCH_EPISODE  = 1 << 5,
    SP_SEARCH_ALL      = (1 << 6) - 1
};
#define SP_SEARCH_LIMIT 10 /* results per kind and request (Spotify's maximum since Feb 2026) */
/* Search the catalog for the kinds in types (SP_SEARCH_* bits). Each kind
 * gets an equal share of cap, at most SP_SEARCH_LIMIT, and out lists them in
 * bit order: tracks, albums, artists, playlists, shows, episodes (Item.kind
 * tells them apart). page as for sp_get_list (offset paging; more = some
 * kind has more). SP_ERR_OTHER for an empty query or no known type. */
int sp_search(HttpClient *c, const char *query, unsigned types, SpPage *page,
              Item *out, int cap, int *count);
/* Pure, exposed for tests: parse a /search body (any mix of kinds) in that
 * order; null results are skipped. SP_ERR_PARSE without any result kind. */
int sp_parse_search(const char *json, SpPage *page, Item *out, int cap, int *count);

/* --- formatting --------------------------------------------------------- */
/* Track/episode time as m:ss, or h:mm:ss from one hour; negative = 0:00. */
void sp_fmt_time(char *out, size_t cap, int ms);

#endif
