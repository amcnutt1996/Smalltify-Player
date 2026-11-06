/* Spotify Web API calls. Each parses JSON (cJSON) into model.h structs.
 * Return codes: SP_OK, or negative SP_ERR_*. Pass the calling thread's own
 * HttpClient (created with auth_store_http()). */
#ifndef SPOTIFY_H
#define SPOTIFY_H

#include "http.h"
#include "model.h"

#define SP_API "https://api.spotify.com/v1"

enum {
    SP_OK           = 0,
    SP_ERR_NET      = -1, /* transport failure */
    SP_ERR_AUTH     = -2, /* 401 after refresh / not logged in */
    SP_ERR_NODEVICE = -3, /* 404 NO_ACTIVE_DEVICE */
    SP_ERR_PREMIUM  = -4, /* 403 premium required / restricted */
    SP_ERR_RATE     = -5, /* 429 */
    SP_ERR_PARSE    = -6,
    SP_ERR_OTHER    = -7
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
/* Find device by exact name (e.g. CFG_DEVICE_NAME). SP_OK and fills out. */
int sp_find_device(HttpClient *c, const char *name, Device *out);

/* --- extras ------------------------------------------------------------- */
int sp_get_playlists(HttpClient *c, Playlist *out, int cap, int *count);
int sp_search_tracks(HttpClient *c, const char *query, Track *out, int cap, int *count);
/* Play a context (playlist/album URI) or, if is_track, a single track URI. */
int sp_play_uri(HttpClient *c, const char *device_id, const char *uri, int is_track);
/* Play uri (NULL = resume) on the active device. With none active and
 * fallback_name set, play on the device with that exact name instead.
 * SP_ERR_NODEVICE when neither exists. */
int sp_play_fallback(HttpClient *c, const char *uri, int is_track, const char *fallback_name);
/* Parse /me/playlists and /search?type=track bodies (no network; used by
 * tests). SP_OK, or SP_ERR_PARSE if the expected items array is missing. */
int sp_parse_playlists(const char *json, Playlist *out, int cap, int *count);
int sp_parse_search(const char *json, Track *out, int cap, int *count);

/* --- formatting --------------------------------------------------------- */
/* Track/episode time as m:ss, or h:mm:ss from one hour; negative = 0:00. */
void sp_fmt_time(char *out, size_t cap, int ms);

#endif
