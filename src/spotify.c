/* Spotify Web API calls. Every sp_* function is a thin layer over
 * sp_request(), which owns URL building, status -> SP_ERR_* mapping and JSON
 * parsing, so error handling stays identical across endpoints. */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"
#include "spotify.h"

#define SP_URL_MAX 1024

/* ======================================================================
 * Shared helpers: request + JSON field access (reuse these in every sp_*)
 * ====================================================================== */

/* Bounded copy that never cuts a UTF-8 sequence in half, so truncated
 * titles still render cleanly. dst is always NUL-terminated. */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        /* src[n] is the first dropped byte; if it continues a sequence,
         * drop that sequence's lead and continuation bytes too. */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Copy obj[key] if it is a string, else "" (missing/null tolerated). */
static void json_str(char *dst, size_t cap, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    copy_utf8(dst, cap, cJSON_IsString(v) ? v->valuestring : NULL);
}

/* obj[key] as int, def if missing/null/not a number; clamped to int. */
static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    double d;

    if (!cJSON_IsNumber(v))
        return def;
    d = v->valuedouble;
    if (d >= (double)INT_MAX)
        return INT_MAX;
    if (d <= (double)INT_MIN)
        return INT_MIN;
    return (int)d;
}

static int json_bool(const cJSON *obj, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

/* Spotify reports why a player call failed in {"error":{"reason":...}}. */
static int body_reason_is(const cJSON *root, const char *reason)
{
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(err, "reason");
    return cJSON_IsString(r) && strcmp(r->valuestring, reason) == 0;
}

static int map_status(long status, const char *path, const cJSON *body)
{
    if (status >= 200 && status < 300)
        return SP_OK;
    switch (status) {
    case 401:
        return SP_ERR_AUTH;
    case 403:
        return SP_ERR_PREMIUM;
    case 404:
        /* Player endpoints answer 404 when no device is active; elsewhere a
         * 404 is a bad ID unless Spotify says otherwise. */
        if (body_reason_is(body, "NO_ACTIVE_DEVICE") || strncmp(path, "/me/player", 10) == 0)
            return SP_ERR_NODEVICE;
        return SP_ERR_OTHER;
    case 429:
        return SP_ERR_RATE;
    default:
        return SP_ERR_OTHER;
    }
}

/* One Web API round-trip. path is relative to SP_API ("/me/player?...").
 * json_body (may be NULL) is sent as application/json. On SP_OK, *out (if
 * non-NULL) receives the parsed root for the caller to cJSON_Delete, or
 * NULL for 204/empty bodies. Pass out NULL when the response is unused. */
static int sp_request(HttpClient *c, const char *method, const char *path,
                      const char *json_body, cJSON **out)
{
    char url[SP_URL_MAX];
    HttpResponse r;
    cJSON *root = NULL;
    int n, rc;

    if (out)
        *out = NULL;
    n = snprintf(url, sizeof url, "%s%s", SP_API, path);
    if (n < 0 || (size_t)n >= sizeof url)
        return SP_ERR_OTHER;

    if (http_request(c, method, url, json_body ? "application/json" : NULL,
                     json_body, &r) != 0 || r.status == 0) {
        http_response_free(&r);
        return SP_ERR_NET;
    }
    if (r.body && r.len > 0)
        root = cJSON_ParseWithLength(r.body, r.len);
    rc = map_status(r.status, path, root);
    if (rc == SP_OK && out && r.status != 204 && r.len > 0) {
        if (!root)
            rc = SP_ERR_PARSE;
        else {
            *out = root;
            root = NULL;
        }
    }
    cJSON_Delete(root);
    http_response_free(&r);
    return rc;
}

const char *sp_strerror(int err)
{
    switch (err) {
    case SP_OK:           return "OK";
    case SP_ERR_NET:      return "Cannot reach Spotify. Check your internet connection.";
    case SP_ERR_AUTH:     return "Not logged in to Spotify (or the session expired). Please log in again.";
    case SP_ERR_NODEVICE: return "No active Spotify device. Start playback on a device or pick one first.";
    case SP_ERR_PREMIUM:  return "Spotify Premium is required for playback control (or this action is not allowed right now).";
    case SP_ERR_RATE:     return "Spotify is rate limiting requests. Please wait a moment and try again.";
    case SP_ERR_PARSE:    return "Unexpected response from Spotify.";
    case SP_ERR_OTHER:    return "Spotify request failed.";
    default:              return "Unknown error.";
    }
}

/* ======================================================================
 * State (now playing)
 * ====================================================================== */

/* Smallest image at least 64 px wide: enough for the UI, cheapest to load. */
static void pick_image(char *dst, size_t cap, const cJSON *images)
{
    const cJSON *img, *best = NULL;
    int best_w = INT_MAX;

    dst[0] = '\0';
    cJSON_ArrayForEach(img, images) {
        int w = json_int(img, "width", -1);
        if (w >= 64 && w < best_w && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(img, "url"))) {
            best = img;
            best_w = w;
        }
    }
    if (best)
        json_str(dst, cap, best, "url");
}

/* "A, B, C" from an artists array, truncated like any other field but never
 * ending in a dangling separator. */
static void join_artists(char *dst, size_t cap, const cJSON *artists)
{
    const cJSON *a;
    size_t len = 0;

    dst[0] = '\0';
    cJSON_ArrayForEach(a, artists) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(a, "name");
        size_t sep = len > 0 ? 2 : 0;
        size_t added;

        if (!cJSON_IsString(name) || !name->valuestring[0])
            continue;
        if (len + sep + 1 >= cap)
            break;
        memcpy(dst + len, ", ", sep);
        copy_utf8(dst + len + sep, cap - len - sep, name->valuestring);
        added = strlen(dst + len + sep);
        if (added == 0) {
            dst[len] = '\0';
            break;
        }
        len += sep + added;
        if (name->valuestring[added] != '\0')
            break; /* truncated: anything appended after it would mislead */
    }
}

static void parse_item(const cJSON *item, Track *t)
{
    const cJSON *images;

    memset(t, 0, sizeof *t);
    if (!cJSON_IsObject(item))
        return;
    json_str(t->id, sizeof t->id, item, "id");
    json_str(t->uri, sizeof t->uri, item, "uri");
    json_str(t->name, sizeof t->name, item, "name");
    t->duration_ms = json_int(item, "duration_ms", 0);

    if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(item, "show"))) {
        /* Podcast episode: the show stands in for album, publisher for artist. */
        const cJSON *show = cJSON_GetObjectItemCaseSensitive(item, "show");
        json_str(t->album, sizeof t->album, show, "name");
        json_str(t->artists, sizeof t->artists, show, "publisher");
        images = cJSON_GetObjectItemCaseSensitive(item, "images");
        if (!cJSON_GetArraySize(images))
            images = cJSON_GetObjectItemCaseSensitive(show, "images");
    } else {
        const cJSON *album = cJSON_GetObjectItemCaseSensitive(item, "album");
        json_str(t->album, sizeof t->album, album, "name");
        join_artists(t->artists, sizeof t->artists,
                     cJSON_GetObjectItemCaseSensitive(item, "artists"));
        images = cJSON_GetObjectItemCaseSensitive(album, "images");
    }
    pick_image(t->art_url, sizeof t->art_url, images);
}

static RepeatMode parse_repeat(const cJSON *root)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "repeat_state");
    if (cJSON_IsString(v)) {
        if (!strcmp(v->valuestring, "context"))
            return REPEAT_CONTEXT;
        if (!strcmp(v->valuestring, "track"))
            return REPEAT_TRACK;
    }
    return REPEAT_OFF;
}

/* root = /me/player response. Leaves fetched_at_ms to the caller. On
 * SP_ERR_PARSE out is reset to "nothing playing". */
static int parse_playback(const cJSON *root, PlaybackState *out)
{
    const cJSON *dev = cJSON_GetObjectItemCaseSensitive(root, "device");

    memset(out, 0, sizeof *out);
    out->volume = -1;
    if (!cJSON_IsObject(root))
        return SP_ERR_PARSE;
    out->active = 1;
    out->is_playing = json_bool(root, "is_playing");
    out->progress_ms = json_int(root, "progress_ms", 0);
    out->shuffle = json_bool(root, "shuffle_state");
    out->repeat = parse_repeat(root);
    out->volume = json_int(dev, "volume_percent", -1);
    json_str(out->device_id, sizeof out->device_id, dev, "id");
    json_str(out->device_name, sizeof out->device_name, dev, "name");
    parse_item(cJSON_GetObjectItemCaseSensitive(root, "item"), &out->track);
    return SP_OK;
}

int sp_parse_playback(const char *json, PlaybackState *out)
{
    cJSON *root;
    int rc;

    if (!out)
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_playback(root, out);
    cJSON_Delete(root);
    return rc;
}

int sp_get_playback(HttpClient *c, PlaybackState *out)
{
    cJSON *root;
    int rc;

    if (!out)
        return SP_ERR_OTHER;
    rc = sp_request(c, "GET", "/me/player?additional_types=episode", NULL, &root);
    if (rc != SP_OK)
        return rc;
    if (root) {
        rc = parse_playback(root, out);
        cJSON_Delete(root);
    } else {
        memset(out, 0, sizeof *out); /* 204: nothing playing anywhere */
        out->volume = -1;
    }
    out->fetched_at_ms = plat_now_ms();
    return rc;
}

/* ======================================================================
 * Playback control: build on sp_request()
 * ====================================================================== */

/* path + "?device_id=<encoded>" when device_id is set, so every player
 * command can target a specific device the same way. */
static int with_device(char *buf, size_t cap, const char *path, const char *device_id)
{
    char enc[SP_ID_LEN * 3];
    int n;

    if (!device_id || !device_id[0])
        n = snprintf(buf, cap, "%s", path);
    else if (http_urlencode(device_id, enc, sizeof enc) != 0)
        return SP_ERR_OTHER;
    else
        n = snprintf(buf, cap, "%s?device_id=%s", path, enc);
    return (n < 0 || (size_t)n >= cap) ? SP_ERR_OTHER : SP_OK;
}

int sp_play(HttpClient *c, const char *device_id)
{
    char path[SP_URL_MAX];
    int rc = with_device(path, sizeof path, "/me/player/play", device_id);
    return rc != SP_OK ? rc : sp_request(c, "PUT", path, NULL, NULL);
}

int sp_pause(HttpClient *c)
{
    return sp_request(c, "PUT", "/me/player/pause", NULL, NULL);
}

int sp_next(HttpClient *c)
{
    return sp_request(c, "POST", "/me/player/next", NULL, NULL);
}

int sp_prev(HttpClient *c)
{
    return sp_request(c, "POST", "/me/player/previous", NULL, NULL);
}

int sp_seek(HttpClient *c, int position_ms)
{
    char path[64];
    snprintf(path, sizeof path, "/me/player/seek?position_ms=%d", position_ms < 0 ? 0 : position_ms);
    return sp_request(c, "PUT", path, NULL, NULL);
}

int sp_set_volume(HttpClient *c, int percent)
{
    char path[64];
    percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    snprintf(path, sizeof path, "/me/player/volume?volume_percent=%d", percent);
    return sp_request(c, "PUT", path, NULL, NULL);
}

int sp_set_shuffle(HttpClient *c, int on)
{
    return sp_request(c, "PUT", on ? "/me/player/shuffle?state=true"
                                   : "/me/player/shuffle?state=false", NULL, NULL);
}

int sp_set_repeat(HttpClient *c, RepeatMode mode)
{
    static const char *const paths[] = {
        "/me/player/repeat?state=off",
        "/me/player/repeat?state=context",
        "/me/player/repeat?state=track",
    };
    if ((int)mode < REPEAT_OFF || mode > REPEAT_TRACK)
        return SP_ERR_OTHER;
    return sp_request(c, "PUT", paths[mode], NULL, NULL);
}

/* ======================================================================
 * Devices: build on sp_request() + json_str()/json_int()
 * ====================================================================== */

/* Devices beyond cap are dropped; *count is the number stored. */
static int parse_devices(const cJSON *root, Device *out, int cap, int *count)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *d;
    int n = 0;

    *count = 0;
    if (!cJSON_IsArray(list))
        return SP_ERR_PARSE;
    cJSON_ArrayForEach(d, list) {
        Device *dev;

        if (n >= cap)
            break;
        if (!cJSON_IsObject(d))
            continue;
        dev = &out[n++];
        json_str(dev->id, sizeof dev->id, d, "id");
        json_str(dev->name, sizeof dev->name, d, "name");
        json_str(dev->type, sizeof dev->type, d, "type");
        dev->is_active = json_bool(d, "is_active");
        dev->volume = json_int(d, "volume_percent", -1);
    }
    *count = n;
    return SP_OK;
}

int sp_parse_devices(const char *json, Device *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || cap < 0 || (cap > 0 && !out))
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_devices(root, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_get_devices(HttpClient *c, Device *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || cap < 0 || (cap > 0 && !out))
        return SP_ERR_OTHER;
    *count = 0;
    rc = sp_request(c, "GET", "/me/player/devices", NULL, &root);
    if (rc != SP_OK)
        return rc;
    rc = parse_devices(root, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_find_device(HttpClient *c, const char *name, Device *out)
{
    Device devs[SP_MAX_DEVICES];
    int n, i, rc;

    if (!name || !out)
        return SP_ERR_OTHER;
    rc = sp_get_devices(c, devs, SP_MAX_DEVICES, &n);
    if (rc != SP_OK)
        return rc;
    for (i = 0; i < n; i++) {
        if (strcmp(devs[i].name, name) == 0) {
            *out = devs[i];
            return SP_OK;
        }
    }
    return SP_ERR_NODEVICE;
}

int sp_transfer(HttpClient *c, const char *device_id, int play)
{
    cJSON *body = cJSON_CreateObject();
    cJSON *ids;
    char *json = NULL;
    int rc = SP_ERR_OTHER;

    if (!device_id || !device_id[0] || !body) {
        cJSON_Delete(body);
        return SP_ERR_OTHER;
    }
    /* Built with cJSON so ids are escaped; the array is owned by body as
     * soon as it exists, so no failure path leaks. */
    ids = cJSON_AddArrayToObject(body, "device_ids");
    if (ids && cJSON_AddItemToArray(ids, cJSON_CreateString(device_id)) &&
        cJSON_AddBoolToObject(body, "play", play ? 1 : 0))
        json = cJSON_PrintUnformatted(body);
    if (json)
        rc = sp_request(c, "PUT", "/me/player", json, NULL);
    cJSON_free(json);
    cJSON_Delete(body);
    return rc;
}

/* ======================================================================
 * Extras (playlists, search, play URI): build on sp_request()
 * ====================================================================== */

/* Playlists beyond cap are dropped; *count is the number stored. */
static int parse_playlists(const cJSON *root, Playlist *out, int cap, int *count)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "items");
    const cJSON *p;
    int n = 0;

    *count = 0;
    if (!cJSON_IsArray(list))
        return SP_ERR_PARSE;
    cJSON_ArrayForEach(p, list) {
        Playlist *pl;

        if (n >= cap)
            break;
        if (!cJSON_IsObject(p))
            continue; /* Spotify returns null for unavailable playlists */
        pl = &out[n++];
        json_str(pl->id, sizeof pl->id, p, "id");
        json_str(pl->uri, sizeof pl->uri, p, "uri");
        json_str(pl->name, sizeof pl->name, p, "name");
        /* "tracks" was renamed "items" in newer API responses; accept both. */
        pl->track_count = json_int(cJSON_GetObjectItemCaseSensitive(p, "tracks"), "total", -1);
        if (pl->track_count < 0)
            pl->track_count = json_int(cJSON_GetObjectItemCaseSensitive(p, "items"), "total", 0);
    }
    *count = n;
    return SP_OK;
}

/* Search results reuse parse_item() so tracks look identical to playback. */
static int parse_search(const cJSON *root, Track *out, int cap, int *count)
{
    const cJSON *tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(tracks, "items");
    const cJSON *t;
    int n = 0;

    *count = 0;
    if (!cJSON_IsArray(list))
        return SP_ERR_PARSE;
    cJSON_ArrayForEach(t, list) {
        if (n >= cap)
            break;
        if (!cJSON_IsObject(t))
            continue;
        parse_item(t, &out[n++]);
    }
    *count = n;
    return SP_OK;
}

int sp_parse_playlists(const char *json, Playlist *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || cap < 0 || (cap > 0 && !out))
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_playlists(root, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_parse_search(const char *json, Track *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || cap < 0 || (cap > 0 && !out))
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_search(root, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_get_playlists(HttpClient *c, Playlist *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || cap < 0 || (cap > 0 && !out))
        return SP_ERR_OTHER;
    *count = 0;
    rc = sp_request(c, "GET", "/me/playlists?limit=50", NULL, &root);
    if (rc != SP_OK)
        return rc;
    rc = parse_playlists(root, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_search_tracks(HttpClient *c, const char *query, Track *out, int cap, int *count)
{
    char enc[SP_NAME_LEN * 3];
    char path[SP_URL_MAX];
    cJSON *root;
    int n, rc;

    if (!count || !query || !query[0] || cap <= 0 || !out)
        return SP_ERR_OTHER;
    *count = 0;
    if (http_urlencode(query, enc, sizeof enc) != 0)
        return SP_ERR_OTHER;
    n = snprintf(path, sizeof path, "/search?type=track&limit=%d&q=%s", cap > 50 ? 50 : cap, enc);
    if (n < 0 || (size_t)n >= sizeof path)
        return SP_ERR_OTHER;
    rc = sp_request(c, "GET", path, NULL, &root);
    if (rc != SP_OK)
        return rc;
    rc = parse_search(root, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_play_uri(HttpClient *c, const char *device_id, const char *uri, int is_track)
{
    char path[SP_URL_MAX];
    cJSON *body;
    char *json = NULL;
    int rc;

    if (!uri || !uri[0])
        return SP_ERR_OTHER;
    rc = with_device(path, sizeof path, "/me/player/play", device_id);
    if (rc != SP_OK)
        return rc;
    body = cJSON_CreateObject();
    if (!body)
        return SP_ERR_OTHER;
    if (is_track) {
        cJSON *uris = cJSON_AddArrayToObject(body, "uris");
        if (uris && cJSON_AddItemToArray(uris, cJSON_CreateString(uri)))
            json = cJSON_PrintUnformatted(body);
    } else if (cJSON_AddStringToObject(body, "context_uri", uri)) {
        json = cJSON_PrintUnformatted(body);
    }
    rc = json ? sp_request(c, "PUT", path, json, NULL) : SP_ERR_OTHER;
    cJSON_free(json);
    cJSON_Delete(body);
    return rc;
}
