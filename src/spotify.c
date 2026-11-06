/* Spotify Web API calls. Every sp_* function is a thin layer over
 * sp_request(), which owns URL building, status -> SP_ERR_* mapping and JSON
 * parsing, so error handling stays identical across endpoints. */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"
#include "spotify.h"

#define SP_URL_MAX 2048 /* fits SP_LIBRARY_MAX encoded URIs */

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

/* Spotify says why a call failed in {"error":{"reason":...,"message":...}}. */
static int body_error_is(const cJSON *root, const char *key, const char *value)
{
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(err, key);
    return cJSON_IsString(r) && strcmp(r->valuestring, value) == 0;
}

static int map_status(long status, const char *path, const cJSON *body)
{
    if (status >= 200 && status < 300)
        return SP_OK;
    switch (status) {
    case 401:
        return SP_ERR_AUTH;
    case 403:
        /* A sign-in from before a scope was added; other 403s are Premium or
         * content restrictions. */
        if (body_error_is(body, "message", "Insufficient client scope"))
            return SP_ERR_SCOPE;
        return SP_ERR_PREMIUM;
    case 404:
        /* Player endpoints answer 404 when no device is active; elsewhere a
         * 404 is a bad ID unless Spotify says otherwise. */
        if (body_error_is(body, "reason", "NO_ACTIVE_DEVICE") || strncmp(path, "/me/player", 10) == 0)
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
    case SP_ERR_SCOPE:    return "Spotify needs your permission for this. Please sign in again.";
    default:              return "Unknown error.";
    }
}

/* ======================================================================
 * State (now playing)
 * ====================================================================== */

/* Smallest image at least ART_SHARP_W wide (the GUI slot is ~330 pt, so
 * smaller covers look blurry); if none is that big, the largest image of at
 * least ART_MIN_W. Spotify usually offers 640/300/64, so this picks 300.
 * unsized_ok: with no such image, the first one without a width (custom
 * playlist covers come that way). */
#define ART_SHARP_W 300
#define ART_MIN_W   64
static void pick_image(char *dst, size_t cap, const cJSON *images, int unsized_ok)
{
    const cJSON *img, *sharp = NULL, *fallback = NULL, *unsized = NULL;
    int sharp_w = INT_MAX, fallback_w = 0;

    dst[0] = '\0';
    cJSON_ArrayForEach(img, images) {
        int w = json_int(img, "width", -1);
        if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(img, "url")))
            continue;
        if (w < 0 && !unsized)
            unsized = img;
        if (w < ART_MIN_W)
            continue;
        if (w >= ART_SHARP_W && w < sharp_w) {
            sharp = img;
            sharp_w = w;
        } else if (w < ART_SHARP_W && w > fallback_w) {
            fallback = img;
            fallback_w = w;
        }
    }
    if (sharp || fallback)
        json_str(dst, cap, sharp ? sharp : fallback, "url");
    else if (unsized_ok && unsized)
        json_str(dst, cap, unsized, "url");
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
    pick_image(t->art_url, sizeof t->art_url, images, 0);
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
    /* market=from_token: Spotify then omits the per-track available_markets
     * lists, which are most of the payload of every poll. */
    rc = sp_request(c, "GET", "/me/player?additional_types=episode&market=from_token", NULL, &root);
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
 * Playing something: sp_play_uri, sp_play_smart, the queue
 * ====================================================================== */

int sp_play_body(const char *context_uri, const char *item_uri, char *out, size_t cap)
{
    int has_ctx = context_uri && context_uri[0];
    int has_item = item_uri && item_uri[0];
    cJSON *body, *sub;
    int ok;

    if (!out || cap == 0 || cap > INT_MAX)
        return -1;
    out[0] = '\0';
    if (!has_ctx && !has_item)
        return -1;
    body = cJSON_CreateObject();
    if (!body)
        return -1;
    /* Built with cJSON so URIs are escaped; children belong to body as soon
     * as they exist, so no failure path leaks. */
    if (has_ctx) {
        ok = cJSON_AddStringToObject(body, "context_uri", context_uri) != NULL;
        if (ok && has_item)
            ok = (sub = cJSON_AddObjectToObject(body, "offset")) != NULL &&
                 cJSON_AddStringToObject(sub, "uri", item_uri) != NULL;
    } else {
        ok = (sub = cJSON_AddArrayToObject(body, "uris")) != NULL &&
             cJSON_AddItemToArray(sub, cJSON_CreateString(item_uri));
    }
    ok = ok && cJSON_PrintPreallocated(body, out, (int)cap, 0);
    cJSON_Delete(body);
    if (!ok)
        out[0] = '\0';
    return ok ? 0 : -1;
}

int sp_play_uri(HttpClient *c, const char *device_id, const char *context_uri,
                const char *item_uri)
{
    char path[SP_URL_MAX], json[1024];
    int rc;

    if (sp_play_body(context_uri, item_uri, json, sizeof json) != 0)
        return SP_ERR_OTHER;
    rc = with_device(path, sizeof path, "/me/player/play", device_id);
    return rc != SP_OK ? rc : sp_request(c, "PUT", path, json, NULL);
}

int sp_pick_device(const Device *devs, int n, const char *local_name, int local_first, int *where)
{
    int local = -1, active = -1, other = -1;
    for (int i = 0; i < n; i++) {
        if (!devs[i].id[0])
            continue; /* listed but not remotely controllable */
        if (local_name && local < 0 && strcmp(devs[i].name, local_name) == 0)
            local = i;
        else if (devs[i].is_active && active < 0)
            active = i;
        else if (other < 0)
            other = i;
    }
    int w = SP_ON_OTHER, pick = other;
    if (local >= 0 && (local_first || active < 0)) {
        w = SP_ON_LOCAL;
        pick = local;
    } else if (active >= 0) {
        w = SP_ON_ACTIVE;
        pick = active;
    } else if (local >= 0) {
        w = SP_ON_LOCAL;
        pick = local;
    }
    if (where)
        *where = w;
    return pick;
}

int sp_play_smart(HttpClient *c, const char *context_uri, const char *item_uri,
                  const char *local_name, int local_first, Device *on, int *where)
{
    Device devs[SP_MAX_DEVICES];
    int n = 0, w = SP_ON_ACTIVE, rc;
    int uris = context_uri || item_uri; /* else resume */
    if (!local_first) {
        rc = uris ? sp_play_uri(c, NULL, context_uri, item_uri) : sp_play(c, NULL);
        if (rc != SP_ERR_NODEVICE) {
            if (where)
                *where = SP_ON_ACTIVE;
            if (on)
                memset(on, 0, sizeof *on);
            return rc;
        }
    }
    rc = sp_get_devices(c, devs, SP_MAX_DEVICES, &n);
    if (rc != SP_OK)
        return rc;
    int i = sp_pick_device(devs, n, local_name, local_first, &w);
    if (i < 0)
        return SP_ERR_NODEVICE;
    if (uris)
        rc = sp_play_uri(c, devs[i].id, context_uri, item_uri);
    else if (devs[i].is_active || w == SP_ON_LOCAL)
        rc = sp_play(c, devs[i].id);
    else
        rc = sp_transfer(c, devs[i].id, 1); /* resume where the account left off */
    if (on)
        *on = devs[i];
    if (where)
        *where = w;
    return rc;
}

int sp_queue_add(HttpClient *c, const char *uri)
{
    char path[SP_URL_MAX];

    if (sp_uris_path("/me/player/queue?uri=", &uri, 1, path, sizeof path) != 0)
        return SP_ERR_OTHER;
    return sp_request(c, "POST", path, NULL, NULL);
}

/* ======================================================================
 * You (/me) and your library (/me/library)
 * ====================================================================== */

_Static_assert(SP_URI_LEN >= sizeof "spotify:user::collection" + SP_ID_LEN - 1,
               "User.liked_uri must fit any user id");

static int parse_me(const cJSON *root, User *out)
{
    memset(out, 0, sizeof *out);
    json_str(out->id, sizeof out->id, root, "id");
    if (!out->id[0])
        return SP_ERR_PARSE;
    json_str(out->name, sizeof out->name, root, "display_name");
    snprintf(out->liked_uri, sizeof out->liked_uri, "spotify:user:%s:collection", out->id);
    return SP_OK;
}

int sp_parse_me(const char *json, User *out)
{
    cJSON *root;
    int rc;

    if (!out)
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_me(root, out);
    cJSON_Delete(root);
    return rc;
}

int sp_get_me(HttpClient *c, User *out)
{
    cJSON *root;
    int rc;

    if (!out)
        return SP_ERR_OTHER;
    memset(out, 0, sizeof *out);
    rc = sp_request(c, "GET", "/me", NULL, &root);
    if (rc != SP_OK)
        return rc;
    rc = parse_me(root, out);
    cJSON_Delete(root);
    return rc;
}

int sp_uris_path(const char *prefix, const char *const *uris, int n, char *out, size_t cap)
{
    size_t len;

    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    if (!prefix || !uris || n < 1 || n > SP_LIBRARY_MAX || (len = strlen(prefix)) >= cap)
        return -1;
    memcpy(out, prefix, len + 1);
    for (int i = 0; i < n; i++) {
        if (!uris[i] || !uris[i][0] || (i > 0 && len + 1 >= cap))
            goto fail;
        if (i > 0)
            out[len++] = ',';
        if (http_urlencode(uris[i], out + len, cap - len) != 0)
            goto fail;
        len += strlen(out + len);
    }
    return 0;
fail:
    out[0] = '\0';
    return -1;
}

int sp_library_set(HttpClient *c, const char *const *uris, int n, int save)
{
    char path[SP_URL_MAX];

    if (sp_uris_path("/me/library?uris=", uris, n, path, sizeof path) != 0)
        return SP_ERR_OTHER;
    return sp_request(c, save ? "PUT" : "DELETE", path, NULL, NULL);
}

/* root = an array of booleans, one per URI asked about. */
static int parse_contains(const cJSON *root, int *saved, int n)
{
    const cJSON *v;
    int i = 0;

    memset(saved, 0, (size_t)n * sizeof *saved);
    if (!cJSON_IsArray(root) || cJSON_GetArraySize(root) < n)
        return SP_ERR_PARSE;
    cJSON_ArrayForEach(v, root) {
        if (i >= n)
            break;
        saved[i++] = cJSON_IsTrue(v);
    }
    return SP_OK;
}

int sp_parse_contains(const char *json, int *saved, int n)
{
    cJSON *root;
    int rc;

    if (!saved || n < 1)
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_contains(root, saved, n);
    cJSON_Delete(root);
    return rc;
}

int sp_library_contains(HttpClient *c, const char *const *uris, int n, int *saved)
{
    char path[SP_URL_MAX];
    cJSON *root;
    int rc;

    if (!saved || sp_uris_path("/me/library/contains?uris=", uris, n, path, sizeof path) != 0)
        return SP_ERR_OTHER;
    rc = sp_request(c, "GET", path, NULL, &root);
    if (rc != SP_OK)
        return rc;
    rc = parse_contains(root, saved, n);
    cJSON_Delete(root);
    return rc;
}

/* ======================================================================
 * Items: one parser for every kind, shared by lists and search
 * ====================================================================== */

static const cJSON *field(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

/* obj's "type", or def when it has none we know (it may be missing). */
static ItemKind kind_of(const cJSON *obj, ItemKind def)
{
    static const struct { const char *type; ItemKind kind; } kinds[] = {
        { "track", ITEM_TRACK }, { "episode", ITEM_EPISODE }, { "album", ITEM_ALBUM },
        { "artist", ITEM_ARTIST }, { "playlist", ITEM_PLAYLIST }, { "show", ITEM_SHOW },
    };
    const cJSON *t = field(obj, "type");

    for (size_t i = 0; cJSON_IsString(t) && i < sizeof kinds / sizeof kinds[0]; i++)
        if (!strcmp(t->valuestring, kinds[i].type))
            return kinds[i].kind;
    return def;
}

/* The uri of the first entry of list that has one ("" if none). */
static void first_uri(char *dst, size_t cap, const cJSON *list)
{
    const cJSON *e;

    dst[0] = '\0';
    cJSON_ArrayForEach(e, list) {
        if (cJSON_IsString(field(e, "uri"))) {
            json_str(dst, cap, e, "uri");
            return;
        }
    }
}

/* Fill it from one Spotify object; def is its kind unless "type" says
 * otherwise. -1 (it zeroed) if obj is not an object (a null entry) or has
 * neither a URI nor a name: Spotify's placeholder for an unavailable
 * episode is all nulls. */
static int parse_entity(const cJSON *obj, ItemKind def, Item *it)
{
    const cJSON *parent = NULL, *images = field(obj, "images");

    memset(it, 0, sizeof *it);
    if (!cJSON_IsObject(obj))
        return -1;
    json_str(it->uri, sizeof it->uri, obj, "uri");
    json_str(it->name, sizeof it->name, obj, "name");
    if (!it->uri[0] && !it->name[0])
        return -1;
    it->kind = kind_of(obj, def);
    it->count = -1;
    json_str(it->id, sizeof it->id, obj, "id");
    switch (it->kind) {
    case ITEM_TRACK:
        parent = field(obj, "album");
        images = field(parent, "images");
        join_artists(it->sub, sizeof it->sub, field(obj, "artists"));
        first_uri(it->artist_uri, sizeof it->artist_uri, field(obj, "artists"));
        break;
    case ITEM_EPISODE:
        parent = field(obj, "show"); /* only in full episode objects */
        if (!cJSON_GetArraySize(images))
            images = field(parent, "images");
        json_str(it->sub, sizeof it->sub, parent, "name");
        json_str(it->date, sizeof it->date, obj, "release_date");
        break;
    case ITEM_ALBUM:
        join_artists(it->sub, sizeof it->sub, field(obj, "artists"));
        first_uri(it->artist_uri, sizeof it->artist_uri, field(obj, "artists"));
        json_str(it->date, sizeof it->date, obj, "release_date");
        it->count = json_int(obj, "total_tracks", -1);
        break;
    case ITEM_ARTIST:
        break;
    case ITEM_PLAYLIST:
        json_str(it->sub, sizeof it->sub, field(obj, "owner"), "display_name");
        /* "tracks" was renamed "items" in Feb 2026; playlists you neither own
         * nor collaborate on may come with neither. */
        it->count = json_int(field(obj, "items"), "total", -1);
        if (it->count < 0)
            it->count = json_int(field(obj, "tracks"), "total", -1);
        break;
    case ITEM_SHOW:
        json_str(it->sub, sizeof it->sub, obj, "publisher");
        it->count = json_int(obj, "total_episodes", -1);
        break;
    }
    json_str(it->album, sizeof it->album, parent, "name");
    json_str(it->album_uri, sizeof it->album_uri, parent, "uri");
    pick_image(it->art_url, sizeof it->art_url, images, 1);
    it->duration_ms = json_int(obj, "duration_ms", 0);
    /* Local files only play on the device that has them; with a market,
     * Spotify adds is_playable for content it can't play in yours. */
    it->playable = it->uri[0] && !json_bool(obj, "is_local") &&
                   !cJSON_IsFalse(field(obj, "is_playable"));
    return 0;
}

/* ======================================================================
 * Lists (sp_get_list): one table row per SpList
 * ====================================================================== */

enum { PAGE_OFFSET, PAGE_AFTER, PAGE_BEFORE, PAGE_NONE };

typedef struct {
    const char *pre, *post; /* path = pre + id + post, then the paging query */
    int         needs_id;
    const char *wrap;       /* key of the paging object in the body; NULL = the body */
    const char *entity;     /* key of the object in each entry; NULL = the entry */
    const char *old_entity; /* its name before Feb 2026, tried when entity is absent */
    ItemKind    kind;       /* unless the object's "type" says otherwise */
    int         paging;     /* PAGE_* */
    int         max;        /* Spotify's largest limit for it */
} ListSpec;

/* market=from_token: Spotify adds is_playable and leaves out the per-item
 * country lists. */
static const ListSpec LISTS[SP_LIST_COUNT] = {
    [SP_LIST_LIKED]     = { "/me/tracks", "?market=from_token", 0, NULL, "track", NULL,
                            ITEM_TRACK, PAGE_OFFSET, SP_PAGE_MAX },
    [SP_LIST_PLAYLISTS] = { "/me/playlists", "", 0, NULL, NULL, NULL, ITEM_PLAYLIST, PAGE_OFFSET,
                            SP_PAGE_MAX },
    [SP_LIST_ALBUMS]    = { "/me/albums", "?market=from_token", 0, NULL, "album", NULL,
                            ITEM_ALBUM, PAGE_OFFSET, SP_PAGE_MAX },
    [SP_LIST_ARTISTS]   = { "/me/following", "?type=artist", 0, "artists", NULL, NULL,
                            ITEM_ARTIST, PAGE_AFTER, SP_PAGE_MAX },
    [SP_LIST_SHOWS]     = { "/me/shows", "", 0, NULL, "show", NULL, ITEM_SHOW, PAGE_OFFSET,
                            SP_PAGE_MAX },
    [SP_LIST_EPISODES]  = { "/me/episodes", "?market=from_token", 0, NULL, "episode", NULL,
                            ITEM_EPISODE, PAGE_OFFSET, SP_PAGE_MAX },
    [SP_LIST_RECENT]    = { "/me/player/recently-played", "", 0, NULL, "track", NULL,
                            ITEM_TRACK, PAGE_BEFORE, SP_PAGE_MAX },
    [SP_LIST_QUEUE]     = { "/me/player/queue", "", 0, NULL, NULL, NULL, ITEM_TRACK, PAGE_NONE, 0 },
    [SP_LIST_PLAYLIST]  = { "/playlists/", "/items?market=from_token&additional_types=track,episode",
                            1, NULL, "item", "track", ITEM_TRACK, PAGE_OFFSET, SP_PAGE_MAX },
    [SP_LIST_ALBUM]     = { "/albums/", "/tracks?market=from_token", 1, NULL, NULL, NULL,
                            ITEM_TRACK, PAGE_OFFSET, SP_PAGE_MAX },
    [SP_LIST_ARTIST]    = { "/artists/", "/albums?include_groups=album,single,compilation"
                            "&market=from_token", 1, NULL, NULL, NULL, ITEM_ALBUM, PAGE_OFFSET,
                            SP_ARTIST_PAGE_MAX },
    [SP_LIST_SHOW]      = { "/shows/", "/episodes?market=from_token", 1, NULL, NULL, NULL,
                            ITEM_EPISODE, PAGE_OFFSET, SP_PAGE_MAX },
};

/* /me/player/queue: {"currently_playing": item|null, "queue": [items]}. */
static int parse_queue(const cJSON *root, SpPage *page, Item *out, int cap, int *count)
{
    const cJSON *queue = field(root, "queue"), *e;
    int n = 0;

    if (!cJSON_IsArray(queue))
        return SP_ERR_PARSE;
    page->now_playing = parse_entity(field(root, "currently_playing"), ITEM_TRACK, &out[0]) == 0;
    n = page->now_playing;
    cJSON_ArrayForEach(e, queue) {
        if (n >= cap)
            break;
        if (parse_entity(e, ITEM_TRACK, &out[n]) == 0)
            n++;
    }
    page->total = page->now_playing + cJSON_GetArraySize(queue);
    page->more = 0;
    *count = n;
    return SP_OK;
}

/* root = one response of list `which`. page is only written on SP_OK. */
static int parse_list(SpList which, const cJSON *root, SpPage *page, Item *out, int cap, int *count)
{
    const ListSpec *ls = &LISTS[which];
    const cJSON *pg = ls->wrap ? field(root, ls->wrap) : root;
    const cJSON *items = field(pg, "items"), *e, *last = NULL;
    SpPage next = *page;
    int n = 0, used = 0;

    *count = 0;
    next.restricted = next.now_playing = 0;
    if (which == SP_LIST_QUEUE) {
        int rc = parse_queue(root, &next, out, cap, count);
        if (rc == SP_OK)
            *page = next;
        return rc;
    }
    if (!cJSON_IsArray(items))
        return SP_ERR_PARSE;
    cJSON_ArrayForEach(e, items) {
        const cJSON *obj = ls->entity ? field(e, ls->entity) : e;

        if (n >= cap)
            break;
        used++;
        if (!cJSON_IsObject(obj) && ls->old_entity)
            obj = field(e, ls->old_entity);
        if (parse_entity(obj, ls->kind, &out[n]) != 0)
            continue; /* null: removed or unavailable */
        if (json_bool(e, "is_local")) /* playlist entries say it outside the item too */
            out[n].playable = 0;
        last = obj;
        n++;
    }
    int size = cJSON_GetArraySize(items), cut = used < size; /* cap stopped it */
    next.total = json_int(pg, "total", -1);
    /* Entries left over by cap start the next page. */
    next.more = cut || cJSON_IsString(field(pg, "next"));
    if (ls->paging == PAGE_OFFSET) {
        /* Spotify drops unavailable entries after paging, so a page can be
         * short; like its "next", the following one starts a full limit on. */
        next.offset += cut ? used : json_int(pg, "limit", size);
    } else {
        /* "after" is the last artist listed; "before" a time Spotify gives. */
        if (ls->paging == PAGE_AFTER && cut && last)
            json_str(next.cursor, sizeof next.cursor, last, "id");
        else
            json_str(next.cursor, sizeof next.cursor, field(pg, "cursors"),
                     ls->paging == PAGE_AFTER ? "after" : "before");
        next.more = next.more && next.cursor[0];
    }
    *page = next;
    *count = n;
    return SP_OK;
}

int sp_parse_list(SpList which, const char *json, SpPage *page, Item *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || !page || cap <= 0 || !out || (unsigned)which >= SP_LIST_COUNT)
        return SP_ERR_OTHER;
    *count = 0;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_list(which, root, page, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_get_list(HttpClient *c, SpList which, const char *id, SpPage *page,
                Item *out, int cap, int *count)
{
    char path[SP_URL_MAX], enc[SP_ID_LEN * 3] = "", cur[SP_CURSOR_LEN * 3] = "";
    const char *ckey = "";
    const ListSpec *ls;
    cJSON *root;
    int limit, n, rc;
    char sep;

    if (!count || !page || cap <= 0 || !out || (unsigned)which >= SP_LIST_COUNT ||
        page->offset < 0)
        return SP_ERR_OTHER;
    *count = 0;
    ls = &LISTS[which];
    if (ls->needs_id) {
        const char *bare = id ? strrchr(id, ':') : NULL; /* "spotify:album:ID" -> "ID" */
        bare = bare ? bare + 1 : id;
        if (!bare || !bare[0] || http_urlencode(bare, enc, sizeof enc) != 0)
            return SP_ERR_OTHER;
    }
    if ((ls->paging == PAGE_AFTER || ls->paging == PAGE_BEFORE) && page->cursor[0]) {
        if (http_urlencode(page->cursor, cur, sizeof cur) != 0)
            return SP_ERR_OTHER;
        ckey = ls->paging == PAGE_AFTER ? "&after=" : "&before=";
    }
    limit = cap < ls->max ? cap : ls->max;
    sep = strchr(ls->post, '?') ? '&' : '?';
    if (ls->paging == PAGE_NONE)
        n = snprintf(path, sizeof path, "%s%s%s", ls->pre, enc, ls->post);
    else if (ls->paging == PAGE_OFFSET && page->offset > 0)
        n = snprintf(path, sizeof path, "%s%s%s%climit=%d&offset=%d", ls->pre, enc, ls->post,
                     sep, limit, page->offset);
    else
        n = snprintf(path, sizeof path, "%s%s%s%climit=%d%s%s", ls->pre, enc, ls->post, sep,
                     limit, ckey, cur);
    if (n < 0 || (size_t)n >= sizeof path)
        return SP_ERR_OTHER;

    rc = sp_request(c, "GET", path, NULL, &root);
    if (rc == SP_ERR_PREMIUM && which == SP_LIST_PLAYLIST) {
        /* Plain 403 "Forbidden": since Feb 2026 only your own and
         * collaborative playlists list their items. */
        page->total = -1;
        page->more = page->now_playing = 0;
        page->restricted = 1;
        return SP_OK;
    }
    if (rc != SP_OK)
        return rc;
    if (!root) { /* 204: nothing to list */
        page->total = 0;
        page->more = page->restricted = page->now_playing = 0;
        return SP_OK;
    }
    rc = parse_list(which, root, page, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

/* ======================================================================
 * Search
 * ====================================================================== */

/* In SP_SEARCH_* bit order: the result key and the type parameter. */
static const struct { const char *key, *type; ItemKind kind; } SEARCH_KINDS[] = {
    { "tracks", "track", ITEM_TRACK },       { "albums", "album", ITEM_ALBUM },
    { "artists", "artist", ITEM_ARTIST },    { "playlists", "playlist", ITEM_PLAYLIST },
    { "shows", "show", ITEM_SHOW },          { "episodes", "episode", ITEM_EPISODE },
};
#define SEARCH_KINDS_N ((int)(sizeof SEARCH_KINDS / sizeof SEARCH_KINDS[0]))

/* Every kind pages together: the next page starts a full limit on. Results
 * beyond cap are dropped (sp_search asks for at most cap unless cap is
 * smaller than the number of kinds). */
static int parse_search(const cJSON *root, SpPage *page, Item *out, int cap, int *count)
{
    SpPage next = *page;
    int n = 0, kinds = 0, step = 0;

    *count = 0;
    next.total = -1;
    next.more = next.restricted = next.now_playing = 0;
    for (int k = 0; k < SEARCH_KINDS_N; k++) {
        const cJSON *pg = field(root, SEARCH_KINDS[k].key);
        const cJSON *items = field(pg, "items"), *e;
        int size = cJSON_GetArraySize(items), total = json_int(pg, "total", -1);
        int limit = json_int(pg, "limit", size);

        if (!cJSON_IsArray(items))
            continue;
        kinds++;
        cJSON_ArrayForEach(e, items) {
            if (n >= cap)
                break;
            if (parse_entity(e, SEARCH_KINDS[k].kind, &out[n]) == 0)
                n++; /* playlists and shows have null results */
        }
        if (total >= 0)
            next.total = (next.total < 0 ? 0 : next.total) + total;
        if (cJSON_IsString(field(pg, "next")))
            next.more = 1;
        step = limit > step ? limit : step;
    }
    if (!kinds)
        return SP_ERR_PARSE;
    next.offset += step;
    *page = next;
    *count = n;
    return SP_OK;
}

int sp_parse_search(const char *json, SpPage *page, Item *out, int cap, int *count)
{
    cJSON *root;
    int rc;

    if (!count || !page || cap <= 0 || !out)
        return SP_ERR_OTHER;
    root = json ? cJSON_Parse(json) : NULL;
    rc = parse_search(root, page, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

int sp_search(HttpClient *c, const char *query, unsigned types, SpPage *page,
              Item *out, int cap, int *count)
{
    char enc[SP_NAME_LEN * 3], path[SP_URL_MAX], list[64] = "";
    size_t len = 0;
    cJSON *root;
    int kinds = 0, limit, n, rc;

    if (!count || !page || !query || !query[0] || cap <= 0 || !out || page->offset < 0)
        return SP_ERR_OTHER;
    *count = 0;
    for (int k = 0; k < SEARCH_KINDS_N; k++) {
        if (types & (1u << k)) {
            n = snprintf(list + len, sizeof list - len, "%s%s", kinds ? "," : "",
                         SEARCH_KINDS[k].type);
            len += (size_t)n; /* every name together fits list */
            kinds++;
        }
    }
    if (!kinds || http_urlencode(query, enc, sizeof enc) != 0)
        return SP_ERR_OTHER;
    limit = cap / kinds;
    limit = limit < 1 ? 1 : limit > SP_SEARCH_LIMIT ? SP_SEARCH_LIMIT : limit;
    /* No market=from_token: on /search it needs the user-read-private scope. */
    if (page->offset > 0)
        n = snprintf(path, sizeof path, "/search?type=%s&limit=%d&offset=%d&q=%s", list, limit,
                     page->offset, enc);
    else
        n = snprintf(path, sizeof path, "/search?type=%s&limit=%d&q=%s", list, limit, enc);
    if (n < 0 || (size_t)n >= sizeof path)
        return SP_ERR_OTHER;
    rc = sp_request(c, "GET", path, NULL, &root);
    if (rc != SP_OK)
        return rc;
    rc = parse_search(root, page, out, cap, count);
    cJSON_Delete(root);
    return rc;
}

void sp_fmt_time(char *out, size_t cap, int ms)
{
    int s = ms > 0 ? ms / 1000 : 0;
    if (s >= 3600)
        snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}
