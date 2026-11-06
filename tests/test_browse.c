#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "browse.h"
#include "spotify.h"

static Item item(ItemKind kind, const char *uri, const char *name)
{
    Item it;
    memset(&it, 0, sizeof it);
    it.kind = kind;
    snprintf(it.uri, sizeof it.uri, "%s", uri);
    snprintf(it.name, sizeof it.name, "%s", name);
    it.count = -1;
    it.playable = 1;
    return it;
}

static void test_play_rows(void)
{
    PlayTarget t;
    Item track = item(ITEM_TRACK, "spotify:track:t1", "Song");
    Item episode = item(ITEM_EPISODE, "spotify:episode:e1", "Episode");

    /* Liked Songs: from that song on, in Liked Songs. */
    assert(browse_play_target(SP_LIST_LIKED, NULL, &track, &t) == 0);
    assert(t.liked && !t.ctx[0] && !strcmp(t.item, "spotify:track:t1"));
    /* A playlist, an album and a show are the context of their rows. */
    assert(browse_play_target(SP_LIST_PLAYLIST, "spotify:playlist:p1", &track, &t) == 0);
    assert(!t.liked && !strcmp(t.ctx, "spotify:playlist:p1") && !strcmp(t.item, "spotify:track:t1"));
    assert(browse_play_target(SP_LIST_ALBUM, "spotify:album:a1", &track, &t) == 0);
    assert(!strcmp(t.ctx, "spotify:album:a1") && !strcmp(t.item, "spotify:track:t1"));
    assert(browse_play_target(SP_LIST_SHOW, "spotify:show:s1", &episode, &t) == 0);
    assert(!strcmp(t.ctx, "spotify:show:s1") && !strcmp(t.item, "spotify:episode:e1"));
    /* Everywhere else a row plays on its own. */
    static const int alone[] = { -1, SP_LIST_RECENT, SP_LIST_QUEUE, SP_LIST_EPISODES };
    for (size_t i = 0; i < sizeof alone / sizeof alone[0]; i++) {
        assert(browse_play_target(alone[i], NULL, &track, &t) == 0);
        assert(!t.liked && !t.ctx[0] && !strcmp(t.item, "spotify:track:t1"));
    }
    /* A container without its URI: the row still plays, on its own. */
    assert(browse_play_target(SP_LIST_PLAYLIST, "", &track, &t) == 0);
    assert(!t.ctx[0] && !strcmp(t.item, "spotify:track:t1"));
}

static void test_play_nothing(void)
{
    PlayTarget t;
    Item track = item(ITEM_TRACK, "spotify:track:t1", "Song");

    /* Unplayable rows (local files, unavailable) play nothing. */
    track.playable = 0;
    assert(browse_play_target(SP_LIST_PLAYLIST, "spotify:playlist:p1", &track, &t) == -1);
    assert(!t.ctx[0] && !t.item[0] && !t.liked);
    /* Rows that open play nothing either. */
    static const ItemKind opens[] = { ITEM_ALBUM, ITEM_ARTIST, ITEM_PLAYLIST, ITEM_SHOW };
    for (size_t i = 0; i < sizeof opens / sizeof opens[0]; i++) {
        Item c = item(opens[i], "spotify:album:a1", "Container");
        assert(browse_play_target(-1, NULL, &c, &t) == -1);
        assert(browse_play_target(SP_LIST_ARTIST, "spotify:artist:r1", &c, &t) == -1);
    }
}

static void test_play_button(void)
{
    PlayTarget t;
    assert(browse_play_target(SP_LIST_LIKED, NULL, NULL, &t) == 0);
    assert(t.liked && !t.ctx[0] && !t.item[0]);
    static const struct { SpList list; const char *uri; } containers[] = {
        { SP_LIST_PLAYLIST, "spotify:playlist:p1" }, { SP_LIST_ALBUM, "spotify:album:a1" },
        { SP_LIST_ARTIST, "spotify:artist:r1" },     { SP_LIST_SHOW, "spotify:show:s1" },
    };
    for (size_t i = 0; i < sizeof containers / sizeof containers[0]; i++) {
        assert(browse_play_target(containers[i].list, containers[i].uri, NULL, &t) == 0);
        assert(!t.liked && !strcmp(t.ctx, containers[i].uri) && !t.item[0]);
        assert(browse_play_target(containers[i].list, "", NULL, &t) == -1);
    }
    /* Lists that are no context have no Play. */
    static const int plain[] = { -1, SP_LIST_PLAYLISTS, SP_LIST_ALBUMS, SP_LIST_ARTISTS,
                                 SP_LIST_SHOWS, SP_LIST_EPISODES, SP_LIST_RECENT, SP_LIST_QUEUE };
    for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++)
        assert(browse_play_target(plain[i], "spotify:album:a1", NULL, &t) == -1);
}

static void test_row_text(void)
{
    char sub[SP_NAME_LEN], info[32];
    Item it = item(ITEM_TRACK, "spotify:track:t1", "Song");
    snprintf(it.sub, sizeof it.sub, "Daft Punk, Romanthony");
    it.duration_ms = 225000;
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!strcmp(sub, "Daft Punk, Romanthony") && !strcmp(info, "3:45"));

    /* An episode without its show (a show's own list) shows its date. */
    it = item(ITEM_EPISODE, "spotify:episode:e1", "Episode");
    snprintf(it.date, sizeof it.date, "2026-02-01");
    it.duration_ms = 3725000;
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!strcmp(sub, "2026-02-01") && !strcmp(info, "1:02:05"));
    snprintf(it.sub, sizeof it.sub, "The Show");
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!strcmp(sub, "The Show"));

    it = item(ITEM_ALBUM, "spotify:album:a1", "Album");
    it.count = 1;
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!strcmp(info, "1 song"));
    it.count = 12;
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!strcmp(info, "12 songs"));

    /* A playlist Spotify shows no contents of has no count. */
    it = item(ITEM_PLAYLIST, "spotify:playlist:p1", "Mix");
    snprintf(it.sub, sizeof it.sub, "Spotify");
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!strcmp(sub, "Spotify") && !info[0]);

    it = item(ITEM_SHOW, "spotify:show:s1", "Show");
    it.count = 5;
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!sub[0] && !strcmp(info, "5 episodes"));

    it = item(ITEM_ARTIST, "spotify:artist:r1", "Artist");
    browse_row_text(&it, sub, sizeof sub, info, sizeof info);
    assert(!sub[0] && !info[0]);
}

static void test_groups(void)
{
    /* sp_search's order (tracks, albums, artists, playlists, shows,
     * episodes) becomes the panel's: artists before albums. */
    Item v[] = {
        item(ITEM_TRACK, "t1", "t1"),       item(ITEM_TRACK, "t2", "t2"),
        item(ITEM_ALBUM, "al1", "al1"),     item(ITEM_ALBUM, "al2", "al2"),
        item(ITEM_ARTIST, "ar1", "ar1"),    item(ITEM_PLAYLIST, "p1", "p1"),
        item(ITEM_SHOW, "s1", "s1"),        item(ITEM_EPISODE, "e1", "e1"),
        item(ITEM_ARTIST, "ar2", "ar2"),
    };
    static const char *const want[] = { "t1", "t2", "ar1", "ar2", "al1", "al2", "p1", "s1", "e1" };
    int n = (int)(sizeof v / sizeof v[0]);
    browse_sort_groups(v, n);
    for (int i = 0; i < n; i++)
        assert(!strcmp(v[i].uri, want[i]));
    for (int i = 1; i < n; i++)
        assert(browse_group(v[i - 1].kind) <= browse_group(v[i].kind));

    assert(!strcmp(browse_group_name(browse_group(ITEM_TRACK)), "Songs"));
    assert(!strcmp(browse_group_name(browse_group(ITEM_SHOW)), "Podcasts"));
    assert(browse_group_type(browse_group(ITEM_ARTIST)) == SP_SEARCH_ARTIST);
    assert(browse_group_type(browse_group(ITEM_EPISODE)) == SP_SEARCH_EPISODE);
    assert(!strcmp(browse_group_name(BROWSE_GROUPS), "") && browse_group_type(-1) == 0);
    unsigned all = 0;
    for (int g = 0; g < BROWSE_GROUPS; g++)
        all |= browse_group_type(g);
    assert(all == SP_SEARCH_ALL);
    browse_sort_groups(v, 0); /* nothing to do */
}

static void test_append(void)
{
    static Item page[SP_PAGE_MAX];
    ItemList l = { 0 };
    for (int i = 0; i < SP_PAGE_MAX; i++)
        page[i] = item(ITEM_TRACK, "spotify:track:x", "x");
    /* Pages pile up to the cap, and not one item past it. */
    int pages = 0, got;
    while ((got = browse_append(&l, page, SP_PAGE_MAX, 120)) > 0)
        pages++;
    assert(got == 0 && pages == 3 && l.n == 120 && l.cap == 120);
    snprintf(page[0].name, sizeof page[0].name, "new");
    assert(browse_append(&l, page, 1, 120) == 0 && l.n == 120);
    browse_free(&l);
    assert(!l.v && l.n == 0 && l.cap == 0);
    /* Short pages keep their order. */
    page[0] = item(ITEM_TRACK, "a", "a");
    page[1] = item(ITEM_TRACK, "b", "b");
    assert(browse_append(&l, page, 1, BROWSE_KEEP) == 1);
    assert(browse_append(&l, page + 1, 1, BROWSE_KEEP) == 1);
    assert(browse_append(&l, page, 0, BROWSE_KEEP) == 0);
    assert(l.n == 2 && !strcmp(l.v[0].uri, "a") && !strcmp(l.v[1].uri, "b"));
    browse_free(&l);
    browse_free(&l); /* twice is fine */
}

int main(void)
{
    test_play_rows();
    test_play_nothing();
    test_play_button();
    test_row_text();
    test_groups();
    test_append();
    return 0;
}
