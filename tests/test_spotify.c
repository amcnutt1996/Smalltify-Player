#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "spotify.h"

static const char TRACK_JSON[] =
    "{\"device\":{\"id\":\"dev1\",\"name\":\"Kitchen\",\"volume_percent\":55},"
    "\"shuffle_state\":true,\"repeat_state\":\"context\",\"progress_ms\":83000,"
    "\"is_playing\":true,\"currently_playing_type\":\"track\","
    "\"item\":{\"type\":\"track\",\"id\":\"t1\",\"uri\":\"spotify:track:t1\","
    "\"name\":\"One More Time\",\"duration_ms\":225000,"
    "\"artists\":[{\"name\":\"Daft Punk\"},{\"name\":null},{\"name\":\"Romanthony\"}],"
    "\"album\":{\"name\":\"Discovery\",\"images\":["
    "{\"url\":\"https://i/640\",\"width\":640,\"height\":640},"
    "{\"url\":\"https://i/32\",\"width\":32,\"height\":32},"
    "{\"url\":\"https://i/64\",\"width\":64,\"height\":64},"
    "{\"url\":\"https://i/300\",\"width\":300,\"height\":300}]}}}";

static const char EPISODE_JSON[] =
    "{\"device\":{\"id\":\"dev2\",\"name\":\"Phone\",\"volume_percent\":null},"
    "\"shuffle_state\":false,\"repeat_state\":\"track\",\"progress_ms\":null,"
    "\"is_playing\":false,\"currently_playing_type\":\"episode\","
    "\"item\":{\"type\":\"episode\",\"id\":\"e1\",\"uri\":\"spotify:episode:e1\","
    "\"name\":\"Episode 42\",\"duration_ms\":3600000,\"images\":[],"
    "\"show\":{\"name\":\"The Show\",\"publisher\":\"Some Network\",\"images\":["
    "{\"url\":\"https://s/640\",\"width\":640},{\"url\":\"https://s/300\",\"width\":300}]}}}";

static void test_track(void)
{
    PlaybackState s;
    assert(sp_parse_playback(TRACK_JSON, &s) == SP_OK);
    assert(s.active == 1);
    assert(s.is_playing == 1);
    assert(s.progress_ms == 83000);
    assert(s.shuffle == 1);
    assert(s.repeat == REPEAT_CONTEXT);
    assert(s.volume == 55);
    assert(!strcmp(s.device_id, "dev1"));
    assert(!strcmp(s.device_name, "Kitchen"));
    assert(!strcmp(s.track.id, "t1"));
    assert(!strcmp(s.track.uri, "spotify:track:t1"));
    assert(!strcmp(s.track.name, "One More Time"));
    /* Null artist names are skipped, not joined as empty entries. */
    assert(!strcmp(s.track.artists, "Daft Punk, Romanthony"));
    assert(!strcmp(s.track.album, "Discovery"));
    /* Smallest cover that is still sharp in the GUI slot (>= 300 px). */
    assert(!strcmp(s.track.art_url, "https://i/300"));
    assert(s.track.duration_ms == 225000);
    assert(s.fetched_at_ms == 0);
}

static void test_paused_episode(void)
{
    PlaybackState s;
    assert(sp_parse_playback(EPISODE_JSON, &s) == SP_OK);
    assert(s.active == 1);
    assert(s.is_playing == 0);
    assert(s.progress_ms == 0);
    assert(s.shuffle == 0);
    assert(s.repeat == REPEAT_TRACK);
    assert(s.volume == -1);
    assert(!strcmp(s.device_name, "Phone"));
    assert(!strcmp(s.track.name, "Episode 42"));
    assert(!strcmp(s.track.album, "The Show"));
    assert(!strcmp(s.track.artists, "Some Network"));
    /* Empty episode images fall back to the show's. */
    assert(!strcmp(s.track.art_url, "https://s/300"));
    assert(s.track.duration_ms == 3600000);
}

static void test_missing_fields(void)
{
    PlaybackState s;

    /* No images array, no device, unknown repeat value. */
    assert(sp_parse_playback("{\"is_playing\":true,\"repeat_state\":\"bogus\","
                             "\"item\":{\"name\":\"X\",\"artists\":[{\"name\":\"A\"}],"
                             "\"album\":{\"name\":\"Alb\"}}}", &s) == SP_OK);
    assert(s.active == 1 && s.is_playing == 1);
    assert(s.repeat == REPEAT_OFF);
    assert(s.volume == -1);
    assert(s.device_name[0] == '\0');
    assert(!strcmp(s.track.name, "X"));
    assert(!strcmp(s.track.artists, "A"));
    assert(s.track.art_url[0] == '\0');

    /* Only too-small images: no art rather than a blurry one. */
    assert(sp_parse_playback("{\"item\":{\"album\":{\"images\":"
                             "[{\"url\":\"u\",\"width\":32},{\"url\":\"v\",\"width\":null}]}}}",
                             &s) == SP_OK);
    assert(s.track.art_url[0] == '\0');

    /* No sharp size offered: the largest usable one still beats no art. */
    assert(sp_parse_playback("{\"item\":{\"album\":{\"images\":[{\"url\":\"s\",\"width\":64},"
                             "{\"url\":\"m\",\"width\":200},{\"url\":\"t\",\"width\":32}]}}}",
                             &s) == SP_OK);
    assert(!strcmp(s.track.art_url, "m"));

    /* item null (ad, private session): a device is active but no track. */
    assert(sp_parse_playback("{\"device\":{\"name\":\"D\",\"volume_percent\":0},"
                             "\"is_playing\":true,\"item\":null}", &s) == SP_OK);
    assert(s.active == 1);
    assert(s.volume == 0);
    assert(s.track.name[0] == '\0' && s.track.artists[0] == '\0');
    assert(s.track.duration_ms == 0);

    /* Wrong types are ignored, not misread. */
    assert(sp_parse_playback("{\"is_playing\":\"yes\",\"progress_ms\":\"5\","
                             "\"shuffle_state\":1,\"item\":{\"name\":7}}", &s) == SP_OK);
    assert(s.is_playing == 0 && s.progress_ms == 0 && s.shuffle == 0);
    assert(s.track.name[0] == '\0');
}

static void test_repeat_shuffle(void)
{
    static const struct { const char *json; int shuffle; RepeatMode repeat; } cases[] = {
        { "{\"shuffle_state\":false,\"repeat_state\":\"off\"}",     0, REPEAT_OFF },
        { "{\"shuffle_state\":true,\"repeat_state\":\"context\"}",  1, REPEAT_CONTEXT },
        { "{\"shuffle_state\":true,\"repeat_state\":\"track\"}",    1, REPEAT_TRACK },
        { "{\"repeat_state\":\"TRACK\"}",                           0, REPEAT_OFF },
        { "{\"shuffle_state\":null,\"repeat_state\":null}",         0, REPEAT_OFF },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        PlaybackState s;
        assert(sp_parse_playback(cases[i].json, &s) == SP_OK);
        assert(s.shuffle == cases[i].shuffle);
        assert(s.repeat == cases[i].repeat);
    }
}

static void test_truncation(void)
{
    static char json[4096];
    char title[1001];
    PlaybackState s;
    int n;

    memset(title, 'a', 1000);
    title[1000] = '\0';
    n = snprintf(json, sizeof json,
                 "{\"item\":{\"name\":\"%s\",\"artists\":[{\"name\":\"%s\"},{\"name\":\"B\"}],"
                 "\"album\":{\"name\":\"Alb\"}}}", title, title);
    assert(n > 0 && (size_t)n < sizeof json);
    memset(&s, 0x7f, sizeof s);
    assert(sp_parse_playback(json, &s) == SP_OK);
    assert(strlen(s.track.name) == SP_NAME_LEN - 1);
    assert(strspn(s.track.name, "a") == SP_NAME_LEN - 1);
    /* Truncated artist list ends there: no ", B" glued onto a cut name. */
    assert(strlen(s.track.artists) == SP_NAME_LEN - 1);
    assert(strchr(s.track.artists, ',') == NULL);
    /* Neighbouring fields are intact. */
    assert(!strcmp(s.track.album, "Alb"));

    /* Never cut inside a UTF-8 sequence: 254 ASCII + "é" (2 bytes) would
     * need 257 bytes, so the whole "é" is dropped. */
    memset(title, 'a', 254);
    memcpy(title + 254, "\xC3\xA9", 3);
    n = snprintf(json, sizeof json, "{\"item\":{\"name\":\"%s\"}}", title);
    assert(n > 0 && (size_t)n < sizeof json);
    assert(sp_parse_playback(json, &s) == SP_OK);
    assert(strlen(s.track.name) == 254);

    /* Exact fit is kept whole. */
    memset(title, 'a', SP_NAME_LEN - 1);
    title[SP_NAME_LEN - 1] = '\0';
    n = snprintf(json, sizeof json, "{\"item\":{\"name\":\"%s\"}}", title);
    assert(n > 0 && (size_t)n < sizeof json);
    assert(sp_parse_playback(json, &s) == SP_OK);
    assert(!strcmp(s.track.name, title));
}

static void test_malformed(void)
{
    PlaybackState s;
    assert(sp_parse_playback("{\"is_playing\":tru", &s) == SP_ERR_PARSE);
    assert(s.active == 0);
    assert(sp_parse_playback("", &s) == SP_ERR_PARSE);
    assert(sp_parse_playback("[1,2]", &s) == SP_ERR_PARSE);
    assert(sp_parse_playback("null", &s) == SP_ERR_PARSE);
    assert(sp_parse_playback(NULL, &s) == SP_ERR_PARSE);
}

static void test_strerror(void)
{
    assert(strstr(sp_strerror(SP_ERR_PREMIUM), "Premium"));
    assert(strstr(sp_strerror(SP_ERR_SCOPE), "sign in again"));
    for (int e = SP_ERR_SCOPE; e <= SP_OK; e++)
        assert(sp_strerror(e) && sp_strerror(e)[0]);
    assert(sp_strerror(12345)[0]);
}

static const char DEVICES_JSON[] =
    "{\"devices\":["
    "{\"id\":\"a1\",\"is_active\":false,\"name\":\"Smalltify Player\",\"type\":\"Computer\",\"volume_percent\":70},"
    "{\"id\":\"b2\",\"is_active\":true,\"name\":\"Kitchen\",\"type\":\"Speaker\",\"volume_percent\":null},"
    "{\"id\":null,\"is_active\":false,\"name\":\"Restricted TV\",\"type\":\"TV\"}]}";

static void test_devices(void)
{
    Device d[4];
    int n = -1;

    assert(sp_parse_devices(DEVICES_JSON, d, 4, &n) == SP_OK);
    assert(n == 3);
    assert(!strcmp(d[0].id, "a1") && !strcmp(d[0].name, "Smalltify Player"));
    assert(!strcmp(d[0].type, "Computer") && d[0].volume == 70 && !d[0].is_active);
    assert(d[1].is_active && d[1].volume == -1); /* null volume_percent */
    assert(d[2].id[0] == '\0' && d[2].volume == -1 && !strcmp(d[2].type, "TV"));
}

static void test_devices_empty(void)
{
    Device d[2];
    int n = -1;

    assert(sp_parse_devices("{\"devices\":[]}", d, 2, &n) == SP_OK);
    assert(n == 0);
    assert(sp_parse_devices(DEVICES_JSON, NULL, 0, &n) == SP_OK);
    assert(n == 0);
}

static void test_devices_cap(void)
{
    Device d[3];
    int n = -1;

    memset(d, 0x55, sizeof d);
    assert(sp_parse_devices(DEVICES_JSON, d, 2, &n) == SP_OK);
    assert(n == 2);
    assert(!strcmp(d[1].name, "Kitchen"));
    assert((unsigned char)d[2].name[0] == 0x55); /* untouched past cap */
}

static void test_devices_malformed(void)
{
    Device d[2];
    int n = -1;

    assert(sp_parse_devices("{\"devices\":[", d, 2, &n) == SP_ERR_PARSE && n == 0);
    assert(sp_parse_devices("{}", d, 2, &n) == SP_ERR_PARSE);
    assert(sp_parse_devices("[]", d, 2, &n) == SP_ERR_PARSE);
    assert(sp_parse_devices(NULL, d, 2, &n) == SP_ERR_PARSE);
    assert(sp_parse_devices("{\"devices\":[]}", d, 2, NULL) == SP_ERR_OTHER);
}

/* --- lists: response bodies as Spotify sends them (Nov 2026, a development
 * mode app: "items" naming), plus the older "tracks" naming ------------- */

static SpPage first_page(void)
{
    SpPage p;
    memset(&p, 0, sizeof p);
    return p;
}

/* /me/playlists: a custom cover (no sizes), a null entry, and a followed
 * playlist without an "items" summary. */
static const char PLAYLISTS_JSON[] =
    "{\"href\":\"https://api.spotify.com/v1/me/playlists?offset=0&limit=3\",\"limit\":3,"
    "\"next\":\"https://api.spotify.com/v1/me/playlists?offset=3&limit=3\",\"offset\":0,"
    "\"previous\":null,\"total\":19,\"items\":["
    "{\"collaborative\":false,\"description\":\"\",\"id\":\"p1\","
    "\"images\":[{\"height\":null,\"url\":\"https://img/custom\",\"width\":null}],"
    "\"name\":\"Wake Up Mix\",\"owner\":{\"display_name\":\"Gary\",\"id\":\"gary\",\"type\":\"user\"},"
    "\"public\":true,\"snapshot_id\":\"x\",\"items\":{\"href\":\"https://api.spotify.com/v1/playlists/p1/items\","
    "\"total\":50},\"type\":\"playlist\",\"uri\":\"spotify:playlist:p1\"},"
    "null,"
    "{\"collaborative\":false,\"id\":\"p2\",\"images\":[{\"height\":640,\"url\":\"https://img/640\",\"width\":640},"
    "{\"height\":300,\"url\":\"https://img/300\",\"width\":300},{\"height\":60,\"url\":\"https://img/60\",\"width\":60}],"
    "\"name\":\"Synthwave\",\"owner\":{\"display_name\":null,\"id\":\"someone\"},\"type\":\"playlist\","
    "\"uri\":\"spotify:playlist:p2\"}]}";

/* The same list before Feb 2026: "tracks" instead of "items". */
static const char PLAYLISTS_OLD_JSON[] =
    "{\"items\":[{\"id\":\"p1\",\"uri\":\"spotify:playlist:p1\",\"name\":\"Chill\","
    "\"tracks\":{\"href\":\"https://api.spotify.com/v1/playlists/p1/tracks\",\"total\":42}}],"
    "\"next\":null,\"total\":1}";

static void test_list_playlists(void)
{
    Item it[4];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_list(SP_LIST_PLAYLISTS, PLAYLISTS_JSON, &pg, it, 4, &n) == SP_OK);
    assert(n == 2); /* null entry skipped */
    assert(it[0].kind == ITEM_PLAYLIST && !strcmp(it[0].id, "p1"));
    assert(!strcmp(it[0].uri, "spotify:playlist:p1") && !strcmp(it[0].name, "Wake Up Mix"));
    assert(!strcmp(it[0].sub, "Gary") && it[0].count == 50 && it[0].playable);
    assert(!strcmp(it[0].art_url, "https://img/custom")); /* unsized cover still used */
    assert(it[0].duration_ms == 0 && it[0].album[0] == '\0');
    /* Not yours: Spotify may leave out the count; the owner has no name. */
    assert(it[1].count == -1 && it[1].sub[0] == '\0');
    assert(!strcmp(it[1].art_url, "https://img/300"));
    /* The null entry counts for paging: the next page starts after it. */
    assert(pg.offset == 3 && pg.total == 19 && pg.more == 1);
    assert(!pg.restricted && !pg.now_playing);

    pg = first_page();
    assert(sp_parse_list(SP_LIST_PLAYLISTS, PLAYLISTS_OLD_JSON, &pg, it, 4, &n) == SP_OK);
    assert(n == 1 && it[0].count == 42 && !strcmp(it[0].name, "Chill"));
    assert(pg.offset == 1 && pg.total == 1 && pg.more == 0);
}

/* /me/tracks: one playable track, one Spotify can't play in your country. */
static const char LIKED_JSON[] =
    "{\"href\":\"https://api.spotify.com/v1/me/tracks?offset=0&limit=2&market=from_token\","
    "\"items\":[{\"added_at\":\"2026-11-01T10:00:00Z\",\"track\":{"
    "\"album\":{\"album_type\":\"album\",\"id\":\"al1\",\"images\":["
    "{\"height\":640,\"url\":\"https://a/640\",\"width\":640},{\"height\":300,\"url\":\"https://a/300\",\"width\":300},"
    "{\"height\":64,\"url\":\"https://a/64\",\"width\":64}],\"name\":\"Discovery\",\"release_date\":\"2001-03-12\","
    "\"total_tracks\":14,\"type\":\"album\",\"uri\":\"spotify:album:al1\"},"
    "\"artists\":[{\"id\":\"ar1\",\"name\":\"Daft Punk\",\"type\":\"artist\",\"uri\":\"spotify:artist:ar1\"},"
    "{\"id\":\"ar2\",\"name\":\"Romanthony\",\"type\":\"artist\",\"uri\":\"spotify:artist:ar2\"}],"
    "\"disc_number\":1,\"duration_ms\":320357,\"explicit\":false,\"id\":\"t1\",\"is_local\":false,"
    "\"is_playable\":true,\"name\":\"One More Time\",\"track_number\":1,\"type\":\"track\","
    "\"uri\":\"spotify:track:t1\"}},"
    "{\"added_at\":\"2026-10-30T10:00:00Z\",\"track\":{\"album\":{\"name\":\"Gone\",\"images\":[],"
    "\"uri\":\"spotify:album:al2\"},\"artists\":[{\"name\":\"Someone\",\"uri\":\"spotify:artist:ar3\"}],"
    "\"duration_ms\":1000,\"id\":\"t2\",\"is_playable\":false,\"name\":\"Unavailable Song\","
    "\"type\":\"track\",\"uri\":\"spotify:track:t2\"}}],"
    "\"limit\":2,\"next\":\"https://api.spotify.com/v1/me/tracks?offset=2&limit=2\",\"offset\":0,"
    "\"previous\":null,\"total\":2434}";

static void test_list_liked(void)
{
    Item it[3];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_list(SP_LIST_LIKED, LIKED_JSON, &pg, it, 3, &n) == SP_OK && n == 2);
    assert(it[0].kind == ITEM_TRACK && !strcmp(it[0].uri, "spotify:track:t1"));
    assert(!strcmp(it[0].name, "One More Time") && !strcmp(it[0].sub, "Daft Punk, Romanthony"));
    assert(!strcmp(it[0].album, "Discovery") && !strcmp(it[0].album_uri, "spotify:album:al1"));
    assert(!strcmp(it[0].artist_uri, "spotify:artist:ar1"));
    assert(!strcmp(it[0].art_url, "https://a/300") && it[0].duration_ms == 320357);
    assert(it[0].count == -1 && it[0].playable == 1 && it[0].date[0] == '\0');
    assert(it[1].playable == 0 && it[1].art_url[0] == '\0');
    assert(pg.offset == 2 && pg.total == 2434 && pg.more == 1);

    /* The page continues from where it stopped: the next one asks offset 2. */
    assert(sp_parse_list(SP_LIST_LIKED, LIKED_JSON, &pg, it, 3, &n) == SP_OK);
    assert(pg.offset == 4);

    /* An entry left over by cap starts the next page. */
    pg = first_page();
    assert(sp_parse_list(SP_LIST_LIKED, LIKED_JSON, &pg, it, 1, &n) == SP_OK && n == 1);
    assert(pg.offset == 1 && pg.more == 1);
}

/* /playlists/{id}/items, Feb 2026 shape: "item" (with the leftover "track"
 * and "episode" booleans inside), an episode, a removed item, a local file. */
static const char PLAYLIST_ITEMS_JSON[] =
    "{\"href\":\"https://api.spotify.com/v1/playlists/p1/items?offset=0&limit=50\",\"items\":["
    "{\"added_at\":\"2026-01-01T00:00:00Z\",\"added_by\":{\"id\":\"gary\"},\"is_local\":false,"
    "\"primary_color\":null,\"item\":{\"album\":{\"name\":\"Discovery\",\"uri\":\"spotify:album:al1\","
    "\"images\":[{\"url\":\"https://a/300\",\"width\":300,\"height\":300}]},"
    "\"artists\":[{\"name\":\"Daft Punk\",\"uri\":\"spotify:artist:ar1\"}],\"duration_ms\":320357,"
    "\"episode\":false,\"track\":true,\"id\":\"t1\",\"is_local\":false,\"is_playable\":true,"
    "\"name\":\"One More Time\",\"type\":\"track\",\"uri\":\"spotify:track:t1\"},"
    "\"video_thumbnail\":{\"url\":null}},"
    "{\"added_at\":\"2026-01-02T00:00:00Z\",\"is_local\":false,\"item\":{\"type\":\"episode\",\"id\":\"e1\","
    "\"uri\":\"spotify:episode:e1\",\"name\":\"Ep 1\",\"duration_ms\":3600000,\"release_date\":\"2026-10-31\","
    "\"images\":[],\"is_playable\":true,\"episode\":true,\"track\":false,\"show\":{\"name\":\"The Show\","
    "\"uri\":\"spotify:show:s1\",\"images\":[{\"url\":\"https://s/640\",\"width\":640},"
    "{\"url\":\"https://s/300\",\"width\":300}]}}},"
    "{\"added_at\":\"2026-01-03T00:00:00Z\",\"is_local\":false,\"item\":null},"
    "{\"added_at\":\"2026-01-04T00:00:00Z\",\"is_local\":true,\"item\":{\"album\":{\"name\":\"\",\"images\":[]},"
    "\"artists\":[{\"name\":\"Me\"}],\"duration_ms\":200000,\"id\":null,\"name\":\"My Demo\","
    "\"type\":\"track\",\"uri\":\"spotify:local:Me::My+Demo:200\"}}"
    "],\"limit\":50,\"next\":null,\"offset\":0,\"previous\":null,\"total\":4}";

/* The same endpoint before Feb 2026: entries named "track". */
static const char PLAYLIST_ITEMS_OLD_JSON[] =
    "{\"items\":[{\"added_at\":\"2020-01-01T00:00:00Z\",\"is_local\":false,\"track\":{"
    "\"album\":{\"name\":\"Alb\",\"images\":[]},\"artists\":[{\"name\":\"A\"}],\"id\":\"t9\","
    "\"name\":\"Old\",\"type\":\"track\",\"uri\":\"spotify:track:t9\"}},"
    "{\"added_at\":null,\"is_local\":false,\"track\":null}],\"next\":null,\"total\":2}";

static void test_list_playlist_items(void)
{
    Item it[5];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_list(SP_LIST_PLAYLIST, PLAYLIST_ITEMS_JSON, &pg, it, 5, &n) == SP_OK);
    assert(n == 3); /* the removed item is skipped */
    assert(it[0].kind == ITEM_TRACK && !strcmp(it[0].name, "One More Time") && it[0].playable);
    assert(it[1].kind == ITEM_EPISODE && !strcmp(it[1].uri, "spotify:episode:e1"));
    assert(!strcmp(it[1].sub, "The Show") && !strcmp(it[1].album, "The Show"));
    assert(!strcmp(it[1].album_uri, "spotify:show:s1") && it[1].artist_uri[0] == '\0');
    assert(!strcmp(it[1].art_url, "https://s/300")); /* no episode images: the show's */
    assert(!strcmp(it[1].date, "2026-10-31") && it[1].duration_ms == 3600000);
    /* Local files are listed but can't be played from here. */
    assert(it[2].kind == ITEM_TRACK && it[2].id[0] == '\0' && !it[2].playable);
    assert(!strcmp(it[2].uri, "spotify:local:Me::My+Demo:200") && !strcmp(it[2].sub, "Me"));
    assert(pg.offset == 50 && pg.total == 4 && pg.more == 0); /* a full limit on */

    pg = first_page();
    assert(sp_parse_list(SP_LIST_PLAYLIST, PLAYLIST_ITEMS_OLD_JSON, &pg, it, 5, &n) == SP_OK);
    assert(n == 1 && !strcmp(it[0].uri, "spotify:track:t9") && !strcmp(it[0].album, "Alb"));
    assert(pg.offset == 2 && pg.more == 0);
}

/* Saved albums (each with its first tracks, which lists ignore), saved shows
 * (Spotify stopped sending "publisher"), saved episodes. */
static const char ALBUMS_JSON[] =
    "{\"items\":[{\"added_at\":\"2026-05-01T00:00:00Z\",\"album\":{\"album_type\":\"album\","
    "\"artists\":[{\"name\":\"Daft Punk\",\"uri\":\"spotify:artist:ar1\"}],\"genres\":[],\"id\":\"al1\","
    "\"images\":[{\"url\":\"https://a/640\",\"width\":640},{\"url\":\"https://a/300\",\"width\":300}],"
    "\"is_playable\":true,\"name\":\"Discovery\",\"release_date\":\"2001-03-12\","
    "\"release_date_precision\":\"day\",\"total_tracks\":14,\"tracks\":{\"items\":[{\"name\":\"x\"}],"
    "\"total\":14},\"type\":\"album\",\"uri\":\"spotify:album:al1\"}}],"
    "\"limit\":20,\"next\":null,\"offset\":0,\"total\":3}";

static const char SHOWS_JSON[] =
    "{\"items\":[{\"added_at\":\"2026-05-01T00:00:00Z\",\"show\":{\"id\":\"s1\",\"name\":\"The Show\","
    "\"uri\":\"spotify:show:s1\",\"total_episodes\":21,\"media_type\":\"audio\",\"type\":\"show\","
    "\"images\":[{\"url\":\"https://s/640\",\"width\":640},{\"url\":\"https://s/300\",\"width\":300}]}},"
    "{\"added_at\":\"2020-05-01T00:00:00Z\",\"show\":{\"id\":\"s2\",\"name\":\"Old Show\","
    "\"publisher\":\"Some Network\",\"uri\":\"spotify:show:s2\",\"total_episodes\":5,\"type\":\"show\"}}],"
    "\"next\":null,\"offset\":0,\"total\":2}";

/* An episode Spotify can't play here, then the all-null placeholder it sends
 * for one that is gone (seen on a real account, Nov 2026). */
static const char EPISODES_JSON[] =
    "{\"items\":[{\"added_at\":\"2026-05-01T00:00:00Z\",\"episode\":{\"id\":\"e1\",\"name\":\"Gone Ep\","
    "\"uri\":\"spotify:episode:e1\",\"duration_ms\":60000,\"release_date\":\"2019\","
    "\"images\":[{\"url\":\"https://e/300\",\"width\":300}],\"is_playable\":false,\"type\":\"episode\","
    "\"show\":{\"name\":\"The Show\",\"uri\":\"spotify:show:s1\"}}},"
    "{\"added_at\":\"2024-01-01T00:00:00Z\",\"episode\":{\"duration_ms\":null,\"id\":null,\"name\":null,"
    "\"release_date\":\"0000\",\"type\":\"episode\",\"uri\":null,\"images\":[],\"show\":null}}],"
    "\"next\":null,\"total\":2}";

static void test_list_saved(void)
{
    Item it[3];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_list(SP_LIST_ALBUMS, ALBUMS_JSON, &pg, it, 3, &n) == SP_OK && n == 1);
    assert(it[0].kind == ITEM_ALBUM && !strcmp(it[0].uri, "spotify:album:al1"));
    assert(!strcmp(it[0].sub, "Daft Punk") && !strcmp(it[0].artist_uri, "spotify:artist:ar1"));
    assert(it[0].count == 14 && !strcmp(it[0].date, "2001-03-12") && it[0].playable);
    assert(!strcmp(it[0].art_url, "https://a/300") && it[0].album[0] == '\0');
    assert(pg.offset == 20 && pg.total == 3 && pg.more == 0);

    pg = first_page();
    assert(sp_parse_list(SP_LIST_SHOWS, SHOWS_JSON, &pg, it, 3, &n) == SP_OK && n == 2);
    assert(it[0].kind == ITEM_SHOW && it[0].count == 21 && it[0].sub[0] == '\0');
    assert(!strcmp(it[0].art_url, "https://s/300"));
    assert(!strcmp(it[1].sub, "Some Network") && it[1].art_url[0] == '\0');

    pg = first_page();
    assert(sp_parse_list(SP_LIST_EPISODES, EPISODES_JSON, &pg, it, 3, &n) == SP_OK && n == 1);
    assert(it[0].kind == ITEM_EPISODE && !strcmp(it[0].sub, "The Show"));
    assert(!it[0].playable && !strcmp(it[0].date, "2019") && it[0].duration_ms == 60000);
    assert(pg.offset == 2 && pg.total == 2 && pg.more == 0); /* the placeholder still counts */
}

/* Cursor paging: followed artists continue after an artist id, recently
 * played before a time. */
static const char ARTISTS_JSON[] =
    "{\"artists\":{\"items\":[{\"id\":\"ar1\",\"name\":\"Daft Punk\",\"type\":\"artist\","
    "\"uri\":\"spotify:artist:ar1\",\"images\":[{\"url\":\"https://p/640\",\"width\":640},"
    "{\"url\":\"https://p/320\",\"width\":320},{\"url\":\"https://p/160\",\"width\":160}]},"
    "{\"id\":\"ar2\",\"name\":\"Justice\",\"type\":\"artist\",\"uri\":\"spotify:artist:ar2\",\"images\":[]}],"
    "\"next\":\"https://api.spotify.com/v1/me/following?type=artist&after=ar2&limit=2\",\"total\":7,"
    "\"cursors\":{\"after\":\"ar2\"},\"limit\":2,"
    "\"href\":\"https://api.spotify.com/v1/me/following?type=artist&limit=2\"}}";

static const char ARTISTS_LAST_JSON[] =
    "{\"artists\":{\"items\":[{\"id\":\"ar7\",\"name\":\"Last\",\"type\":\"artist\","
    "\"uri\":\"spotify:artist:ar7\"}],\"next\":null,\"total\":7,\"cursors\":{\"after\":null},\"limit\":2}}";

static const char RECENT_JSON[] =
    "{\"items\":[{\"track\":{\"album\":{\"name\":\"Discovery\",\"uri\":\"spotify:album:al1\",\"images\":[]},"
    "\"artists\":[{\"name\":\"Daft Punk\",\"uri\":\"spotify:artist:ar1\"}],\"duration_ms\":320357,"
    "\"id\":\"t1\",\"name\":\"One More Time\",\"type\":\"track\",\"uri\":\"spotify:track:t1\"},"
    "\"played_at\":\"2026-11-06T15:04:05.123Z\",\"context\":{\"type\":\"playlist\","
    "\"uri\":\"spotify:playlist:p1\"}},"
    "{\"track\":{\"name\":\"Two\",\"type\":\"track\",\"uri\":\"spotify:track:t2\"},"
    "\"played_at\":\"2026-11-06T14:00:00.000Z\",\"context\":null}],"
    "\"next\":\"https://api.spotify.com/v1/me/player/recently-played?before=1762437600000&limit=2\","
    "\"cursors\":{\"after\":\"1762441445123\",\"before\":\"1762437600000\"},\"limit\":2}";

static void test_list_cursors(void)
{
    Item it[3];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_list(SP_LIST_ARTISTS, ARTISTS_JSON, &pg, it, 3, &n) == SP_OK && n == 2);
    assert(it[0].kind == ITEM_ARTIST && !strcmp(it[0].name, "Daft Punk"));
    assert(!strcmp(it[0].art_url, "https://p/320") && it[0].sub[0] == '\0' && it[0].count == -1);
    assert(!strcmp(pg.cursor, "ar2") && pg.more == 1 && pg.total == 7 && pg.offset == 0);
    assert(sp_parse_list(SP_LIST_ARTISTS, ARTISTS_LAST_JSON, &pg, it, 3, &n) == SP_OK && n == 1);
    assert(pg.more == 0 && pg.cursor[0] == '\0');
    /* Stopped by cap: continue after the last artist actually listed. */
    pg = first_page();
    assert(sp_parse_list(SP_LIST_ARTISTS, ARTISTS_JSON, &pg, it, 1, &n) == SP_OK && n == 1);
    assert(!strcmp(pg.cursor, "ar1") && pg.more == 1);

    pg = first_page();
    assert(sp_parse_list(SP_LIST_RECENT, RECENT_JSON, &pg, it, 3, &n) == SP_OK && n == 2);
    assert(it[0].kind == ITEM_TRACK && !strcmp(it[0].album_uri, "spotify:album:al1"));
    assert(!strcmp(it[1].name, "Two") && it[1].sub[0] == '\0');
    assert(!strcmp(pg.cursor, "1762437600000") && pg.more == 1 && pg.total == -1);
}

/* /me/player/queue: what plays now, then up next (a track and an episode). */
static const char QUEUE_JSON[] =
    "{\"currently_playing\":{\"album\":{\"name\":\"Discovery\",\"images\":[]},\"artists\":[{\"name\":\"Daft Punk\"}],"
    "\"duration_ms\":320357,\"id\":\"t1\",\"name\":\"One More Time\",\"type\":\"track\",\"uri\":\"spotify:track:t1\"},"
    "\"queue\":[{\"album\":{\"name\":\"Discovery\"},\"artists\":[{\"name\":\"Daft Punk\"}],\"id\":\"t3\","
    "\"name\":\"Aerodynamic\",\"type\":\"track\",\"uri\":\"spotify:track:t3\"},"
    "{\"id\":\"e1\",\"name\":\"Ep 1\",\"type\":\"episode\",\"uri\":\"spotify:episode:e1\","
    "\"show\":{\"name\":\"The Show\",\"uri\":\"spotify:show:s1\"}}]}";

static void test_list_queue(void)
{
    Item it[4];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_list(SP_LIST_QUEUE, QUEUE_JSON, &pg, it, 4, &n) == SP_OK && n == 3);
    assert(pg.now_playing == 1 && pg.total == 3 && pg.more == 0);
    assert(!strcmp(it[0].name, "One More Time") && !strcmp(it[1].name, "Aerodynamic"));
    assert(it[2].kind == ITEM_EPISODE && !strcmp(it[2].sub, "The Show"));
    assert(sp_parse_list(SP_LIST_QUEUE, QUEUE_JSON, &pg, it, 2, &n) == SP_OK && n == 2);
    assert(pg.total == 3); /* the whole queue, even past cap */
    /* Nothing playing. */
    assert(sp_parse_list(SP_LIST_QUEUE, "{\"currently_playing\":null,\"queue\":[]}", &pg, it, 4,
                         &n) == SP_OK);
    assert(n == 0 && pg.now_playing == 0 && pg.total == 0);
    assert(sp_parse_list(SP_LIST_QUEUE, "{\"currently_playing\":null}", &pg, it, 4, &n) == SP_ERR_PARSE);
}

/* What an album, artist or show holds: simplified objects without their
 * album or show. */
static const char ALBUM_TRACKS_JSON[] =
    "{\"items\":[{\"artists\":[{\"name\":\"Daft Punk\",\"uri\":\"spotify:artist:ar1\"}],\"disc_number\":1,"
    "\"duration_ms\":320357,\"id\":\"t1\",\"is_local\":false,\"is_playable\":true,\"name\":\"One More Time\","
    "\"track_number\":1,\"uri\":\"spotify:track:t1\"}],\"limit\":50,\"next\":null,\"offset\":0,\"total\":1}";

static const char ARTIST_ALBUMS_JSON[] =
    "{\"items\":[{\"album_type\":\"single\",\"artists\":[{\"name\":\"Daft Punk\",\"uri\":\"spotify:artist:ar1\"}],"
    "\"id\":\"al9\",\"images\":[{\"url\":\"https://a/64\",\"width\":64}],\"name\":\"Single\","
    "\"release_date\":\"2020\",\"total_tracks\":1,\"type\":\"album\",\"uri\":\"spotify:album:al9\"}],"
    "\"limit\":1,\"next\":\"https://api.spotify.com/v1/artists/ar1/albums?offset=1&limit=1\",\"offset\":0,"
    "\"total\":3}";

/* A short page as seen on a real account (Nov 2026): Spotify drops
 * unavailable albums after paging, so offset 10, limit 10 brought 2 albums
 * while "next" points at offset 20. */
static const char ARTIST_ALBUMS_SHORT_JSON[] =
    "{\"items\":[{\"name\":\"A\",\"type\":\"album\",\"uri\":\"spotify:album:a\"},"
    "{\"name\":\"B\",\"type\":\"album\",\"uri\":\"spotify:album:b\"}],\"limit\":10,"
    "\"next\":\"https://api.spotify.com/v1/artists/ar1/albums?offset=20&limit=10\",\"offset\":10,"
    "\"total\":39}";

static const char SHOW_EPISODES_JSON[] =
    "{\"items\":[{\"id\":\"e2\",\"name\":\"Ep 2\",\"uri\":\"spotify:episode:e2\",\"duration_ms\":1800000,"
    "\"release_date\":\"2026-11-01\",\"images\":[{\"url\":\"https://e/640\",\"width\":640},"
    "{\"url\":\"https://e/300\",\"width\":300}],\"is_playable\":true,\"type\":\"episode\"},null],"
    "\"next\":null,\"total\":21}";

static void test_list_contents(void)
{
    Item it[3];
    SpPage pg = first_page();
    int n = -1;

    /* No "type": the list's own kind. */
    assert(sp_parse_list(SP_LIST_ALBUM, ALBUM_TRACKS_JSON, &pg, it, 3, &n) == SP_OK && n == 1);
    assert(it[0].kind == ITEM_TRACK && !strcmp(it[0].sub, "Daft Punk") && it[0].playable);
    assert(it[0].album[0] == '\0' && it[0].album_uri[0] == '\0' && it[0].art_url[0] == '\0');
    assert(!strcmp(it[0].artist_uri, "spotify:artist:ar1"));

    pg = first_page();
    assert(sp_parse_list(SP_LIST_ARTIST, ARTIST_ALBUMS_JSON, &pg, it, 3, &n) == SP_OK && n == 1);
    assert(it[0].kind == ITEM_ALBUM && it[0].count == 1 && !strcmp(it[0].date, "2020"));
    assert(!strcmp(it[0].art_url, "https://a/64")); /* small, but the only one */
    assert(pg.offset == 1 && pg.more == 1 && pg.total == 3);
    pg.offset = 10;
    assert(sp_parse_list(SP_LIST_ARTIST, ARTIST_ALBUMS_SHORT_JSON, &pg, it, 3, &n) == SP_OK);
    assert(n == 2 && pg.offset == 20 && pg.more == 1); /* not 12: no entry seen twice */

    pg = first_page();
    assert(sp_parse_list(SP_LIST_SHOW, SHOW_EPISODES_JSON, &pg, it, 3, &n) == SP_OK && n == 1);
    assert(it[0].kind == ITEM_EPISODE && it[0].sub[0] == '\0' && it[0].album[0] == '\0');
    assert(!strcmp(it[0].art_url, "https://e/300") && !strcmp(it[0].date, "2026-11-01"));
    assert(pg.offset == 2 && pg.more == 0 && pg.total == 21);
}

static void test_list_malformed(void)
{
    Item it[2];
    SpPage pg = first_page();
    int n = -1;

    pg.offset = 5;
    assert(sp_parse_list(SP_LIST_LIKED, "{}", &pg, it, 2, &n) == SP_ERR_PARSE && n == 0);
    assert(pg.offset == 5); /* unchanged: retry the same page */
    assert(sp_parse_list(SP_LIST_LIKED, "{\"items\":[", &pg, it, 2, &n) == SP_ERR_PARSE);
    assert(sp_parse_list(SP_LIST_ARTISTS, LIKED_JSON, &pg, it, 2, &n) == SP_ERR_PARSE);
    assert(sp_parse_list(SP_LIST_PLAYLISTS, NULL, &pg, it, 2, &n) == SP_ERR_PARSE);
    assert(sp_parse_list(SP_LIST_COUNT, LIKED_JSON, &pg, it, 2, &n) == SP_ERR_OTHER);
    assert(sp_parse_list(SP_LIST_LIKED, LIKED_JSON, &pg, it, 0, &n) == SP_ERR_OTHER);
    assert(sp_parse_list(SP_LIST_LIKED, LIKED_JSON, NULL, it, 2, &n) == SP_ERR_OTHER);
    assert(sp_parse_list(SP_LIST_LIKED, "{\"items\":[]}", &pg, it, 2, &n) == SP_OK && n == 0);
    assert(pg.offset == 5 && pg.more == 0);
}

/* /search over every kind: Spotify's key order, null results, and in out
 * the SP_SEARCH_* order. */
static const char SEARCH_JSON[] =
    "{\"tracks\":{\"href\":\"https://api.spotify.com/v1/search?offset=0&limit=2&query=daft&type=track\","
    "\"items\":[{\"album\":{\"name\":\"Discovery\",\"images\":[{\"url\":\"big\",\"width\":640},"
    "{\"url\":\"small\",\"width\":64}]},\"artists\":[{\"name\":\"Daft Punk\"}],\"duration_ms\":1000,"
    "\"id\":\"t1\",\"is_playable\":true,\"name\":\"One More Time\",\"type\":\"track\","
    "\"uri\":\"spotify:track:t1\"}],\"limit\":2,\"next\":\"https://api.spotify.com/v1/search?offset=2\","
    "\"offset\":0,\"previous\":null,\"total\":3},"
    "\"artists\":{\"items\":[{\"id\":\"ar1\",\"name\":\"Daft Punk\",\"type\":\"artist\",\"uri\":\"spotify:artist:ar1\"}],"
    "\"limit\":2,\"next\":null,\"offset\":0,\"total\":1},"
    "\"albums\":{\"items\":[{\"album_type\":\"album\",\"artists\":[{\"name\":\"Daft Punk\"}],\"name\":\"Discovery\","
    "\"total_tracks\":14,\"type\":\"album\",\"uri\":\"spotify:album:al1\"},{\"album_type\":\"album\","
    "\"name\":\"Homework\",\"type\":\"album\",\"uri\":\"spotify:album:al2\"}],\"limit\":2,\"next\":null,"
    "\"offset\":0,\"total\":2},"
    "\"playlists\":{\"items\":[null,{\"name\":\"This Is Daft Punk\",\"owner\":{\"display_name\":\"Spotify\"},"
    "\"items\":{\"total\":30},\"type\":\"playlist\",\"uri\":\"spotify:playlist:p9\"}],\"limit\":2,"
    "\"next\":\"https://api.spotify.com/v1/search?offset=2\",\"offset\":0,\"total\":4},"
    "\"shows\":{\"items\":[{\"name\":\"Daft Talk\",\"total_episodes\":3,\"type\":\"show\",\"uri\":\"spotify:show:s1\"}],"
    "\"limit\":2,\"next\":null,\"offset\":0,\"total\":1},"
    "\"episodes\":{\"items\":[{\"name\":\"On Daft Punk\",\"duration_ms\":60000,\"release_date\":\"2021-02-22\","
    "\"type\":\"episode\",\"uri\":\"spotify:episode:e1\"}],\"limit\":2,\"next\":null,\"offset\":0,\"total\":1}}";

static void test_search(void)
{
    static const ItemKind order[] = { ITEM_TRACK, ITEM_ALBUM, ITEM_ALBUM, ITEM_ARTIST,
                                      ITEM_PLAYLIST, ITEM_SHOW, ITEM_EPISODE };
    Item it[10];
    SpPage pg = first_page();
    int n = -1;

    assert(sp_parse_search(SEARCH_JSON, &pg, it, 10, &n) == SP_OK && n == 7);
    for (int i = 0; i < n; i++)
        assert(it[i].kind == order[i]);
    assert(!strcmp(it[0].sub, "Daft Punk") && !strcmp(it[0].art_url, "big") && it[0].playable);
    assert(!strcmp(it[1].name, "Discovery") && it[1].count == 14 && it[2].count == -1);
    assert(!strcmp(it[4].sub, "Spotify") && it[4].count == 30);
    assert(it[5].count == 3 && !strcmp(it[6].date, "2021-02-22"));
    /* Totals add up (rough since Feb 2026); more if any kind has more; the
     * next page starts after the longest one (2, counting the null). */
    assert(pg.total == 12 && pg.more == 1 && pg.offset == 2);

    pg = first_page();
    assert(sp_parse_search(SEARCH_JSON, &pg, it, 3, &n) == SP_OK && n == 3);
    assert(it[2].kind == ITEM_ALBUM);
    assert(sp_parse_search("{\"tracks\":{\"items\":[]}}", &pg, it, 10, &n) == SP_OK && n == 0);
    assert(pg.more == 0 && pg.total == -1);
    assert(sp_parse_search("{\"tracks\":{}}", &pg, it, 10, &n) == SP_ERR_PARSE && n == 0);
    assert(sp_parse_search("{\"items\":[]}", &pg, it, 10, &n) == SP_ERR_PARSE);
    assert(sp_parse_search(NULL, &pg, it, 10, &n) == SP_ERR_PARSE);
    assert(sp_parse_search(SEARCH_JSON, &pg, it, 0, &n) == SP_ERR_OTHER);
}

static void test_me(void)
{
    User u;
    char json[256], id[SP_ID_LEN];

    assert(sp_parse_me("{\"account_id\":\"x\",\"display_name\":\"Gary\",\"id\":\"gary123\","
                       "\"type\":\"user\",\"uri\":\"spotify:user:gary123\"}", &u) == SP_OK);
    assert(!strcmp(u.id, "gary123") && !strcmp(u.name, "Gary"));
    assert(!strcmp(u.liked_uri, "spotify:user:gary123:collection"));
    assert(sp_parse_me("{\"display_name\":null,\"id\":\"x\"}", &u) == SP_OK && u.name[0] == '\0');
    /* The longest id we keep still names its collection. */
    memset(id, 'a', sizeof id - 1);
    id[sizeof id - 1] = '\0';
    snprintf(json, sizeof json, "{\"id\":\"%s\"}", id);
    assert(sp_parse_me(json, &u) == SP_OK);
    assert(strlen(u.liked_uri) == strlen("spotify:user::collection") + sizeof id - 1);
    assert(sp_parse_me("{}", &u) == SP_ERR_PARSE && u.liked_uri[0] == '\0');
    assert(sp_parse_me("{\"id\":\"\"}", &u) == SP_ERR_PARSE);
    assert(sp_parse_me(NULL, &u) == SP_ERR_PARSE);
}

static void test_contains(void)
{
    int s[4];

    assert(sp_parse_contains("[true,false,true]", s, 3) == SP_OK);
    assert(s[0] == 1 && s[1] == 0 && s[2] == 1);
    assert(sp_parse_contains("[true,null,1]", s, 3) == SP_OK && s[0] == 1 && !s[1] && !s[2]);
    assert(sp_parse_contains("[true,false,true]", s, 4) == SP_ERR_PARSE && s[0] == 0);
    assert(sp_parse_contains("{}", s, 1) == SP_ERR_PARSE);
    assert(sp_parse_contains(NULL, s, 1) == SP_ERR_PARSE);
    assert(sp_parse_contains("[true]", s, 0) == SP_ERR_OTHER);
}

/* What the mutating calls would send (never sent from tests). */
static void test_play_body(void)
{
    char b[256], tiny[24];

    assert(sp_play_body("spotify:playlist:p1", "spotify:track:t1", b, sizeof b) == 0);
    assert(!strcmp(b, "{\"context_uri\":\"spotify:playlist:p1\",\"offset\":{\"uri\":\"spotify:track:t1\"}}"));
    assert(sp_play_body("spotify:user:gary:collection", "spotify:track:t1", b, sizeof b) == 0);
    assert(!strcmp(b, "{\"context_uri\":\"spotify:user:gary:collection\","
                      "\"offset\":{\"uri\":\"spotify:track:t1\"}}"));
    assert(sp_play_body("spotify:album:a", NULL, b, sizeof b) == 0);
    assert(!strcmp(b, "{\"context_uri\":\"spotify:album:a\"}"));
    assert(sp_play_body(NULL, "spotify:track:t", b, sizeof b) == 0);
    assert(!strcmp(b, "{\"uris\":[\"spotify:track:t\"]}"));
    assert(sp_play_body("", "spotify:episode:e", b, sizeof b) == 0);
    assert(!strcmp(b, "{\"uris\":[\"spotify:episode:e\"]}"));
    assert(sp_play_body("a\"b", "", b, sizeof b) == 0 && !strcmp(b, "{\"context_uri\":\"a\\\"b\"}"));
    assert(sp_play_body(NULL, NULL, b, sizeof b) == -1 && b[0] == '\0');
    assert(sp_play_body("", "", b, sizeof b) == -1);
    assert(sp_play_body("spotify:album:abcdefghijkl", NULL, tiny, sizeof tiny) == -1 && tiny[0] == '\0');
}

static void test_uris_path(void)
{
    static const char *const two[] = { "spotify:track:a", "spotify:album:b" };
    static const char *const blank[] = { "spotify:track:a", "" };
    const char *many[SP_LIBRARY_MAX + 1];
    char p[2048], small[32];

    assert(sp_uris_path("/me/library?uris=", two, 2, p, sizeof p) == 0);
    assert(!strcmp(p, "/me/library?uris=spotify%3Atrack%3Aa,spotify%3Aalbum%3Ab"));
    assert(sp_uris_path("/me/library/contains?uris=", two, 1, p, sizeof p) == 0);
    assert(!strcmp(p, "/me/library/contains?uris=spotify%3Atrack%3Aa"));
    assert(sp_uris_path("/me/player/queue?uri=", two + 1, 1, p, sizeof p) == 0);
    assert(!strcmp(p, "/me/player/queue?uri=spotify%3Aalbum%3Ab"));
    /* Spotify's limit: 40 full-length URIs fit the path; 41 are refused. */
    for (int i = 0; i <= SP_LIBRARY_MAX; i++)
        many[i] = "spotify:track:0123456789abcdefghijkl";
    assert(sp_uris_path("/me/library?uris=", many, SP_LIBRARY_MAX, p, sizeof p) == 0);
    /* each "spotify%3Atrack%3A" + 22-char id, then a ',' */
    assert(strlen(p) == strlen("/me/library?uris=") + SP_LIBRARY_MAX * 41 - 1);
    assert(sp_uris_path("/me/library?uris=", many, SP_LIBRARY_MAX + 1, p, sizeof p) == -1);
    assert(sp_uris_path("/me/library?uris=", two, 0, p, sizeof p) == -1 && p[0] == '\0');
    assert(sp_uris_path("/me/library?uris=", blank, 2, p, sizeof p) == -1 && p[0] == '\0');
    assert(sp_uris_path("/me/library?uris=", two, 2, small, sizeof small) == -1 && small[0] == '\0');
}

static void test_fmt_time(void)
{
    char b[16];

    sp_fmt_time(b, sizeof b, 0);
    assert(!strcmp(b, "0:00"));
    sp_fmt_time(b, sizeof b, -5000);
    assert(!strcmp(b, "0:00"));
    sp_fmt_time(b, sizeof b, 59999);
    assert(!strcmp(b, "0:59"));
    sp_fmt_time(b, sizeof b, 60000);
    assert(!strcmp(b, "1:00"));
    sp_fmt_time(b, sizeof b, 3599999);
    assert(!strcmp(b, "59:59"));
    sp_fmt_time(b, sizeof b, 3600000);
    assert(!strcmp(b, "1:00:00"));
    sp_fmt_time(b, sizeof b, 3600000 + 61000);
    assert(!strcmp(b, "1:01:01"));
}

/* Where Play goes: our device, the active one, any other, or nowhere. */
static void test_pick_device(void)
{
    const char *me = "Smalltify Player (mac)";
    Device d[3];
    memset(d, 0, sizeof d);
    snprintf(d[0].id, sizeof d[0].id, "phone");
    snprintf(d[0].name, sizeof d[0].name, "iPhone");
    snprintf(d[1].id, sizeof d[1].id, "mine");
    snprintf(d[1].name, sizeof d[1].name, "%s", me);
    snprintf(d[2].name, sizeof d[2].name, "No id TV"); /* not controllable */
    int where = -1;

    /* (a) paired and ready: a picked uri plays here even if the phone is active. */
    d[0].is_active = 1;
    assert(sp_pick_device(d, 3, me, 1, &where) == 1 && where == SP_ON_LOCAL);
    /* (b) Play keeps an active device. */
    assert(sp_pick_device(d, 3, me, 0, &where) == 0 && where == SP_ON_ACTIVE);
    /* nothing active: ours */
    d[0].is_active = 0;
    assert(sp_pick_device(d, 3, me, 0, &where) == 1 && where == SP_ON_LOCAL);
    /* (c) not paired (no local name): the first available device */
    assert(sp_pick_device(d, 3, NULL, 0, &where) == 0 && where == SP_ON_OTHER);
    assert(sp_pick_device(d + 1, 2, NULL, 1, &where) == 0 && where == SP_ON_OTHER);
    /* (d) nothing that can play */
    assert(sp_pick_device(d + 2, 1, me, 1, &where) == -1);
    assert(sp_pick_device(d, 0, me, 1, &where) == -1);
}

int main(void)
{
    test_pick_device();
    test_track();
    test_paused_episode();
    test_missing_fields();
    test_repeat_shuffle();
    test_truncation();
    test_malformed();
    test_strerror();
    test_devices();
    test_devices_empty();
    test_devices_cap();
    test_devices_malformed();
    test_list_playlists();
    test_list_liked();
    test_list_playlist_items();
    test_list_saved();
    test_list_cursors();
    test_list_queue();
    test_list_contents();
    test_list_malformed();
    test_search();
    test_me();
    test_contains();
    test_play_body();
    test_uris_path();
    test_fmt_time();
    return 0;
}
