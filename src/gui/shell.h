/*
 * shell.h: the frame of the application, the same on every screen. Over
 * the wallpaper (theme.c), two pages, one shown at a time:
 *
 *   SHELL_PLAYER  a floating glass sidebar on the left, which slides away
 *                 and back with a spring (View > Hide Sidebar), and the main
 *                 area, which grows into the room it leaves;
 *   SHELL_CARD    one glass card in the middle (setup, sign-in, key, move).
 *
 * Plus the title band at the top of each page (it moves the window; on
 * macOS the traffic lights sit in it), the menu bar (the system's on
 * macOS) and the toast over everything. Screens build into the slots.
 *
 * Threading: UI thread only.
 */
#ifndef SMP_GUI_SHELL_H
#define SMP_GUI_SHELL_H

#include <Raven.h>

/* Actions of the menus and their shortcuts (Raction_bind). The shell binds
 * ACT_SIDEBAR itself; the others are bound by the modules that do them. */
#define ACT_SETTINGS   "app.settings"    /* Cmd+, (in the application menu on macOS) */
#define ACT_PLAY_PAUSE "player.toggle"   /* Space */
#define ACT_PREVIOUS   "player.previous" /* Cmd+Left */
#define ACT_NEXT       "player.next"     /* Cmd+Right */
#define ACT_LIKE       "player.like"
#define ACT_SIDEBAR    "view.sidebar"    /* Ctrl+Cmd+S: hide / show the sidebar */
#define ACT_FIND       "edit.find"       /* Cmd+F */

typedef enum { SHELL_PLAYER, SHELL_CARD } ShellPage;

/* The menu items whose state a module may change. */
typedef enum {
    SHELL_MENU_PLAY_PAUSE,
    SHELL_MENU_PREVIOUS,
    SHELL_MENU_NEXT,
    SHELL_MENU_LIKE, /* checkable: checked while the track playing is liked */
    SHELL_MENU_SIDEBAR,
    SHELL_MENU_FIND,
    SHELL_MENU_COUNT
} ShellMenu;

/* Builds the whole frame and makes it the window's content. Once, after
 * window_open. The player page shows first. */
void shell_build(void);

/* Shows a page. The items of the Playback and View menus, and Find, are
 * enabled on the player page only. */
void shell_show(ShellPage page);
ShellPage shell_page(void);

/* Where screens build (VBoxes, empty until then):
 *   sidebar  inside the glass sidebar, under its top row (the hide button);
 *   main     the main area, under the title band, the rest of the window;
 *   title    the end of the main area's title band, for a few small buttons;
 *   card     inside the card, which scrolls when its content is too tall. */
Rid shell_sidebar_slot(void);
Rid shell_main_slot(void);
Rid shell_title_slot(void);
Rid shell_card_slot(void);

/* Slides the sidebar away or back (a spring; at once with reduced motion).
 * While it is away, a button in the title band brings it back. */
void shell_set_sidebar(int shown);
int shell_sidebar_shown(void);

/* The smallest room the player's main area works in, below the title band.
 * The window's minimum is that, plus the sidebar while it is in; showing
 * or hiding the sidebar grows or shrinks the window by its width. */
void shell_set_main_min(float w, float h);

/* A short message rising at the bottom of the window for a few seconds. */
void shell_toast(const char *text, int error);

/* Menu items: their text, check mark and whether they can be chosen. Each
 * compares first. */
void shell_menu_set_text(ShellMenu item, const char *text);
void shell_menu_set_checked(ShellMenu item, int checked);
void shell_menu_set_enabled(ShellMenu item, int enabled);

/* Once per frame, before the screens' updates: moves the sidebar while it
 * slides, follows the text size, keeps the macOS application menu whole. */
void shell_frame(double dt);

#endif /* SMP_GUI_SHELL_H */
