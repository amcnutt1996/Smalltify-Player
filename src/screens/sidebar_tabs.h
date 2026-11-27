/* sidebar_tabs.h: the two tabs of the sidebar, for sidebar.c, which holds
 * them (with the tab bar and the footer) and calls them from its own build
 * and update. Widgets stay file-static in each tab's file. */
#ifndef SMP_SCREENS_SIDEBAR_TABS_H
#define SMP_SCREENS_SIDEBAR_TABS_H

#include "gui/icons.h"
#include "ui_state.h"

/* sidebar_library.c: the title, the search field, the sections, the lists
 * opened within the tab and the search results. */
void library_tab_build(Ui *u, Rid parent);
/* Every update pass, shown or not: pushes what library.c changed. */
void library_tab_update(Ui *u);
/* The keyboard in the search field, its text selected (Cmd+F). */
void library_tab_find(void);

/* sidebar_devices.c: the Spotify Connect devices, and the way to play on
 * this computer. */
void devices_tab_build(Ui *u, Rid parent);
/* Every update pass, shown or not: pushes what changed in u->devs. */
void devices_tab_update(Ui *u);

/* The icon of a Spotify Connect device type ("Computer", "Smartphone"...). */
Icon device_icon(const char *type);

#endif /* SMP_SCREENS_SIDEBAR_TABS_H */
