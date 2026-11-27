/* sidebar.h: what the glass sidebar holds: the Library and Devices tabs;
 * the Library (its title, the search field, the sections that open as
 * accordions, a list opened within it with its way back, the search
 * results); the Devices list; and at its bottom "Playing on", Log out /
 * Sign in and Settings.
 *
 * Owner: the Sidebar module (screens/sidebar*.c and library.c). Its
 * widgets are file-static there; what it does goes through ui.c
 * (ui_internal.h) and library.c (library.h). */
#ifndef SMP_SCREENS_SIDEBAR_H
#define SMP_SCREENS_SIDEBAR_H

#include "ui_state.h"

/* Builds into parent (shell_sidebar_slot()), once at startup. */
void sidebar_build(Ui *u, Rid parent);

/* Every update pass while the player page shows, sidebar away or not:
 * takes the library worker's pages (lib_sync) and pushes what changed
 * (lib_gen, devs_seq, st.device_name...) into the widgets. */
void sidebar_update(Ui *u);

/* ACT_FIND (Cmd+F): the Library tab, with the keyboard in the search field.
 * The shell has brought the sidebar back already. */
void sidebar_find(Ui *u);

#endif /* SMP_SCREENS_SIDEBAR_H */
