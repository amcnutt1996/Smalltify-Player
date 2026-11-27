/*
 * macos.h: the few things only AppKit can do (macos.m). Declared for macOS
 * builds only; callers guard their calls with #ifdef __APPLE__.
 */
#ifndef SMP_GUI_MACOS_H
#define SMP_GUI_MACOS_H

#ifdef __APPLE__
#include <Raven.h>

/* The title bar of the system, transparent over the content (which covers
 * the whole window): the real traffic lights sit at the top left, centred
 * in the top MAC_TITLE_H of the window. Clicks in that band reach Raven:
 * the content moves the window itself (window_title_event). Call before
 * the window shows. */
#define MAC_TITLE_H  32
void mac_window_setup(Rid window);

/* 1 while the application is the one in front. Raven's window counts as
 * active from its creation until AppKit says otherwise, which it never does
 * for an app launched in the background (open -g). mac_window_setup also
 * wakes the loop whenever the application comes to the front or leaves it. */
int mac_app_active(void);

/* The application menu, which Raven builds (About, Hide, Quit): adds
 * "Settings..." (Cmd+,), which fires `settings_action` on the UI thread, and
 * names the application by its bundle name rather than the executable's.
 * Raven builds a new menu whenever the MenuBar changes, so this runs on
 * every frame; it only works when the menu is new. */
void mac_menu_sync(const char *settings_action);
#endif

#endif /* SMP_GUI_MACOS_H */
