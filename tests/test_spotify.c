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
    assert(!strcmp(s.track.art_url, "https://i/64"));
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
    for (int e = SP_ERR_OTHER; e <= SP_OK; e++)
        assert(sp_strerror(e) && sp_strerror(e)[0]);
    assert(sp_strerror(12345)[0]);
}

static const char DEVICES_JSON[] =
    "{\"devices\":["
    "{\"id\":\"a1\",\"is_active\":false,\"name\":\"Spotify C Player\",\"type\":\"Computer\",\"volume_percent\":70},"
    "{\"id\":\"b2\",\"is_active\":true,\"name\":\"Kitchen\",\"type\":\"Speaker\",\"volume_percent\":null},"
    "{\"id\":null,\"is_active\":false,\"name\":\"Restricted TV\",\"type\":\"TV\"}]}";

static void test_devices(void)
{
    Device d[4];
    int n = -1;

    assert(sp_parse_devices(DEVICES_JSON, d, 4, &n) == SP_OK);
    assert(n == 3);
    assert(!strcmp(d[0].id, "a1") && !strcmp(d[0].name, "Spotify C Player"));
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

static const char PLAYLISTS_JSON[] =
    "{\"items\":["
    "{\"id\":\"p1\",\"uri\":\"spotify:playlist:p1\",\"name\":\"Chill\",\"tracks\":{\"total\":42}},"
    "null,"
    "{\"id\":\"p2\",\"uri\":\"spotify:playlist:p2\",\"name\":\"Run\",\"items\":{\"total\":7}},"
    "{\"id\":\"p3\",\"uri\":\"spotify:playlist:p3\",\"name\":\"Bare\"}"
    "],\"next\":\"https://api.spotify.com/v1/me/playlists?offset=50\"}";

static void test_playlists(void)
{
    Playlist p[4];
    int n = -1;

    assert(sp_parse_playlists(PLAYLISTS_JSON, p, 4, &n) == SP_OK);
    assert(n == 3); /* null entry skipped */
    assert(!strcmp(p[0].uri, "spotify:playlist:p1") && !strcmp(p[0].name, "Chill"));
    assert(p[0].track_count == 42);
    assert(!strcmp(p[1].id, "p2") && p[1].track_count == 7);
    assert(p[2].track_count == 0);
    assert(sp_parse_playlists(PLAYLISTS_JSON, p, 1, &n) == SP_OK && n == 1);
    assert(sp_parse_playlists("{\"items\":[]}", p, 4, &n) == SP_OK && n == 0);
    assert(sp_parse_playlists("{}", p, 4, &n) == SP_ERR_PARSE && n == 0);
    assert(sp_parse_playlists("nope", p, 4, &n) == SP_ERR_PARSE);
    assert(sp_parse_playlists(PLAYLISTS_JSON, p, 4, NULL) == SP_ERR_OTHER);
}

static const char SEARCH_JSON[] =
    "{\"tracks\":{\"items\":["
    "{\"id\":\"t1\",\"uri\":\"spotify:track:t1\",\"name\":\"Song\",\"duration_ms\":1000,"
    "\"artists\":[{\"name\":\"A\"},{\"name\":\"B\"}],"
    "\"album\":{\"name\":\"Alb\",\"images\":[{\"url\":\"big\",\"width\":640},{\"url\":\"small\",\"width\":64}]}},"
    "{\"id\":\"t2\",\"uri\":\"spotify:track:t2\",\"name\":\"Other\"}"
    "]}}";

static void test_search(void)
{
    Track t[4];
    int n = -1;

    assert(sp_parse_search(SEARCH_JSON, t, 4, &n) == SP_OK && n == 2);
    assert(!strcmp(t[0].uri, "spotify:track:t1") && !strcmp(t[0].artists, "A, B"));
    assert(!strcmp(t[0].album, "Alb") && !strcmp(t[0].art_url, "small"));
    assert(t[0].duration_ms == 1000);
    assert(!strcmp(t[1].name, "Other") && t[1].artists[0] == '\0' && t[1].art_url[0] == '\0');
    assert(sp_parse_search(SEARCH_JSON, t, 1, &n) == SP_OK && n == 1);
    assert(sp_parse_search("{\"tracks\":{}}", t, 4, &n) == SP_ERR_PARSE && n == 0);
    assert(sp_parse_search("{\"items\":[]}", t, 4, &n) == SP_ERR_PARSE);
    assert(sp_parse_search(NULL, t, 4, &n) == SP_ERR_PARSE);
}

int main(void)
{
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
    test_playlists();
    test_search();
    return 0;
}
