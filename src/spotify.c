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

/* ======================================================================
 * Devices: build on sp_request() + json_str()/json_int()
 * ====================================================================== */

/* ======================================================================
 * Extras (playlists, search, play URI): build on sp_request()
 * ====================================================================== */
