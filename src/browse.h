/* The Library panel's rules, apart from the GUI so the tests can run them:
 * what a click plays, what a row says, how search results group, and how a
 * list grows. */
#ifndef BROWSE_H
#define BROWSE_H

#include <stddef.h>

#include "model.h"

/* Items one list keeps in memory; past it the panel says "Showing first N". */
#define BROWSE_KEEP 500

/* What a click plays: sp_play_smart's context_uri and item_uri ("" = none). */
typedef struct {
    char ctx[SP_URI_LEN];
    char item[SP_URI_LEN];
    int  liked; /* the context is Liked Songs (User.liked_uri; ctx stays "") */
} PlayTarget;

/* What a click plays on a screen of list `list` (an SpList, or -1 for
 * search results) showing container, the URI of the playlist, album,
 * artist or show listed (NULL or "" for the other lists). it is the row
 * clicked, NULL for the screen's Play button. A track or episode plays in
 * its album, playlist, show or Liked Songs, and on its own everywhere else
 * (search, recently played, the queue, your episodes); Play plays the
 * container or Liked Songs. -1 when the click plays nothing: an unplayable
 * row, a row that opens instead (an album, artist, playlist or show), or
 * Play on a list that is no context. */
int browse_play_target(int list, const char *container, const Item *it, PlayTarget *out);

/* A row's second line (sub) and the figure at its right (info): a track's
 * or episode's length, or how many songs or episodes a container holds; ""
 * when there is none. An episode without its show's name shows its date. */
void browse_row_text(const Item *it, char *sub, size_t sub_cap, char *info, size_t info_cap);

/* Search results group by kind, listed in this order: Songs, Artists,
 * Albums, Playlists, Podcasts, Episodes. */
#define BROWSE_GROUPS 6
int         browse_group(ItemKind kind);  /* 0 .. BROWSE_GROUPS-1 */
const char *browse_group_name(int group); /* "Songs", ...; "" if out of range */
unsigned    browse_group_type(int group); /* its SP_SEARCH_* bit; 0 if out of range */
/* Sort items into group order; within a group they keep their order. */
void browse_sort_groups(Item *items, int n);

/* A list on the heap that grows as pages come. */
typedef struct {
    Item *v;
    int   n, cap;
} ItemList;

/* Append add[0..count) to l, but never past keep items. Returns how many it
 * appended, or -1 when out of memory (l is then unchanged). */
int  browse_append(ItemList *l, const Item *add, int count, int keep);
void browse_free(ItemList *l); /* empties l; safe on an empty one */

#endif
