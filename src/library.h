/* library.h: the Library's navigation, fed by the library worker (ui.c).
 *
 * The root holds the sections (Liked Songs, Playlists... Queue), which open
 * one at a time, like an accordion: each is a list of its own that loads its
 * first page when it opens and pages from there. Over the root, a stack of
 * screens: search results (every kind, grouped), a kind's "See all", and
 * what a playlist, album, artist or show holds. Its state is this module's
 * own (library.c); the sidebar shows it.
 *
 * Requests: the library worker keeps one list request and answers it with
 * that request's seq, so this module sends one at a time. A list that wants
 * a page waits its turn (Level.need); the screen on top goes first, then
 * the open section, then the Liked Songs total of the root. A list left
 * (popped, closed, reset) drops its request: its page is not taken when it
 * comes, and the next request goes out at once. */
#ifndef SMP_LIBRARY_H
#define SMP_LIBRARY_H

#include <stdint.h>

#include "ui_state.h"

/* The root (the Liked Songs total), a list, or search results. */
typedef enum { LV_ROOT, LV_LIST, LV_SEARCH } LevelKind;

typedef struct {
    LevelKind kind;
    SpList    list;               /* LV_LIST */
    unsigned  types;              /* LV_SEARCH: SP_SEARCH_* bits */
    char      uri[SP_URI_LEN];    /* the playlist, album, artist or show listed */
    char      title[SP_NAME_LEN + 16];
    char      detail[SP_NAME_LEN + 32]; /* what it is: "Album · artists", "Search results"... */
    char      query[SP_NAME_LEN]; /* LV_SEARCH */
    ItemList  items;              /* the root: none, it only shows page.total */
    SpPage    page;               /* the next page: more, total, restricted */
    int       cap;                /* rows a page asks for */
    int       loaded;             /* a page came */
    int       err;                /* SP_ERR_* of the last request */
    int       need;               /* a request waits its turn */
    int       refresh;            /* that request starts over and its page
                                     replaces the items (they show meanwhile) */
    uint64_t  want;               /* the request in flight (LibReq.seq), 0 = none */
    uint64_t  id;                 /* changes whenever it holds another list */
    uint64_t  epoch;              /* changes whenever its items are emptied or replaced */
    uint64_t  gen;                /* changes with anything it holds */
} Level;

/* Screens deep: the root, search results, "See all", an artist, an album. */
#define LIB_DEPTH 5

/* Rows a section's page brings (Liked Songs: the 20 added last, then 20
 * more each time); a screen of the stack pages by SP_PAGE_MAX. Spotify
 * caps an artist's albums at SP_ARTIST_PAGE_MAX and a kind of search
 * results at SP_SEARCH_LIMIT. */
#define LIB_SECTION_PAGE 20

/* The root's sections, in the order they show. */
enum {
    LIB_SEC_LIKED,
    LIB_SEC_PLAYLISTS,
    LIB_SEC_ALBUMS,
    LIB_SEC_ARTISTS,
    LIB_SEC_SHOWS,
    LIB_SEC_EPISODES,
    LIB_SEC_RECENT,
    LIB_SEC_QUEUE,
    LIB_SECTIONS
};

/* The screen shown (the top of the stack), its depth (0 = the root), and
 * any screen of the stack. */
Level *lib_top(void);
int lib_depth(void);
Level *lib_level(int depth);
/* Bumped whenever the stack, a section or what a list holds changes: a
 * module looks at its lists (Level.gen) only when this differs from the
 * one it showed. */
uint64_t lib_gen(void);

/* A section's name, its list (empty until it opens), the one open (-1 =
 * none), and opening and closing them: opening one closes the one open
 * and loads its first page; a section that closes forgets its pages. */
const char *lib_section_name(int section);
Level *lib_section(int section);
int lib_section_open(void);
void lib_open_section(Ui *u, int section);
void lib_close_section(Ui *u, int section);

/* The list a screen shows, as browse_play_target takes it: -1 for search. */
int lib_list_of(const Level *lv);
/* Search results of every kind, one page grouped by kind ("See all" has
 * the rest of a kind). */
int lib_grouped(const Level *lv);
/* A page of lv is on its way or waits its turn (not counting a refresh,
 * whose list keeps showing). */
int lib_loading(const Level *lv);
/* Ask the library worker for lv's next page (the root's: the Liked Songs
 * total). It waits its turn; one per list at a time. */
void lib_request(Ui *u, Level *lv);
/* The next page, if there is one and none is on its way. */
void lib_more(Ui *u, Level *lv);
/* After an error: the page that failed, again (from the start if none
 * came). */
void lib_again(Ui *u, Level *lv);
/* The first page again, quietly: the items show until it replaces them
 * (the queue and what played last change behind the Library's back). */
void lib_refresh(Ui *u, Level *lv);

/* Back one screen: the root fetches its Liked Songs total again, a list
 * that never loaded tries again. */
void lib_pop(Ui *u);
/* Back to the root (the sections). */
void lib_home(Ui *u);
/* The search field: results of every kind for query, instead of whatever
 * the stack held. */
void lib_search(Ui *u, const char *query);
/* A search is on the stack (its field holds the query). */
int lib_searching(void);
void lib_open_list(Ui *u, SpList list, const char *title, const char *uri);
/* Search results of every kind (SP_SEARCH_ALL, grouped), or of one. */
void lib_open_search(Ui *u, const char *query, unsigned types, const char *title);
/* What an album, artist, playlist or show row holds, on a screen of its own. */
void lib_open_item(Ui *u, const Item *it);
/* The Library came into view: the Liked Songs total, the queue and what
 * played last may have changed since. */
void lib_opened(Ui *u);
/* Take the library worker's page if a list still wants it, then send the
 * next request waiting. Called by every update pass of the module showing
 * the Library; the first one after lib_reset asks for the root's total. */
void lib_sync(Ui *u);
/* After a new sign-in: the lists shown try again if their page failed. */
void lib_retry(Ui *u);
/* Empty the stack and close the sections: back to the root (a new
 * session, the window closing). */
void lib_reset(void);

#endif /* SMP_LIBRARY_H */
