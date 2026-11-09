/* The Library panel's rules (browse.h): no GUI, no network. */
#include "browse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spotify.h"

int browse_play_target(int list, const char *container, const Item *it, PlayTarget *out)
{
    /* Lists whose rows play in the list itself, and the containers among them. */
    int liked = list == SP_LIST_LIKED;
    int in_container = list == SP_LIST_PLAYLIST || list == SP_LIST_ALBUM || list == SP_LIST_SHOW;

    memset(out, 0, sizeof *out);
    if (!container)
        container = "";
    if (!it) { /* the Play button */
        if (liked) {
            out->liked = 1;
            return 0;
        }
        if ((in_container || list == SP_LIST_ARTIST) && container[0]) {
            snprintf(out->ctx, sizeof out->ctx, "%s", container);
            return 0;
        }
        return -1;
    }
    if (!it->playable || !it->uri[0] || (it->kind != ITEM_TRACK && it->kind != ITEM_EPISODE))
        return -1;
    snprintf(out->item, sizeof out->item, "%s", it->uri);
    if (liked)
        out->liked = 1;
    else if (in_container && container[0])
        snprintf(out->ctx, sizeof out->ctx, "%s", container);
    return 0;
}

void browse_row_text(const Item *it, char *sub, size_t sub_cap, char *info, size_t info_cap)
{
    snprintf(sub, sub_cap, "%s", it->sub);
    snprintf(info, info_cap, "%s", "");
    switch (it->kind) {
    case ITEM_TRACK:
    case ITEM_EPISODE:
        if (it->kind == ITEM_EPISODE && !it->sub[0])
            snprintf(sub, sub_cap, "%s", it->date);
        if (it->duration_ms > 0)
            sp_fmt_time(info, info_cap, it->duration_ms);
        break;
    case ITEM_ALBUM:
    case ITEM_PLAYLIST:
        if (it->count >= 0)
            snprintf(info, info_cap, "%d song%s", it->count, it->count == 1 ? "" : "s");
        break;
    case ITEM_SHOW:
        if (it->count >= 0)
            snprintf(info, info_cap, "%d episode%s", it->count, it->count == 1 ? "" : "s");
        break;
    case ITEM_ARTIST:
        break;
    }
}

static const struct {
    ItemKind    kind;
    const char *name;
    unsigned    type;
} GROUPS[BROWSE_GROUPS] = {
    { ITEM_TRACK, "Songs", SP_SEARCH_TRACK },          { ITEM_ARTIST, "Artists", SP_SEARCH_ARTIST },
    { ITEM_ALBUM, "Albums", SP_SEARCH_ALBUM },         { ITEM_PLAYLIST, "Playlists", SP_SEARCH_PLAYLIST },
    { ITEM_SHOW, "Podcasts", SP_SEARCH_SHOW },         { ITEM_EPISODE, "Episodes", SP_SEARCH_EPISODE },
};

int browse_group(ItemKind kind)
{
    for (int g = 0; g < BROWSE_GROUPS; g++)
        if (GROUPS[g].kind == kind)
            return g;
    return 0;
}

const char *browse_group_name(int group)
{
    return group >= 0 && group < BROWSE_GROUPS ? GROUPS[group].name : "";
}

unsigned browse_group_type(int group)
{
    return group >= 0 && group < BROWSE_GROUPS ? GROUPS[group].type : 0;
}

/* Insertion sort: stable, and a search page holds at most a few dozen. */
void browse_sort_groups(Item *items, int n)
{
    Item tmp;
    for (int i = 1; i < n; i++) {
        int g = browse_group(items[i].kind), j = i;
        while (j > 0 && browse_group(items[j - 1].kind) > g)
            j--;
        if (j == i)
            continue;
        tmp = items[i];
        memmove(&items[j + 1], &items[j], (size_t)(i - j) * sizeof *items);
        items[j] = tmp;
    }
}

int browse_append(ItemList *l, const Item *add, int count, int keep)
{
    if (count > keep - l->n)
        count = keep - l->n;
    if (count <= 0)
        return 0;
    if (l->n + count > l->cap) {
        /* Doubling keeps the copies few; keep bounds it. */
        int cap = l->cap ? 2 * l->cap : count;
        if (cap < l->n + count)
            cap = l->n + count;
        if (cap > keep)
            cap = keep;
        Item *v = realloc(l->v, (size_t)cap * sizeof *v);
        if (!v)
            return -1;
        l->v = v;
        l->cap = cap;
    }
    memcpy(l->v + l->n, add, (size_t)count * sizeof *add);
    l->n += count;
    return count;
}

void browse_free(ItemList *l)
{
    free(l->v);
    memset(l, 0, sizeof *l);
}
