/* settings.h: the Settings sheet (Cmd+, and the sidebar's gear): the text
 * size (Small, Default, Large, Extra large), applied at once and saved to
 * .env (set_text_size), with a warning while it could not be saved
 * (Ui.size_unsaved); the account (signed in, Log out); the app's name and
 * version.
 *
 * Owner: the Sheets module (screens/settings*.c), with signin.c. */
#ifndef SMP_SCREENS_SETTINGS_H
#define SMP_SCREENS_SETTINGS_H

#include "ui_state.h"

/* Opens the sheet (once: no-op while it is open). */
void settings_open(Ui *u);

/* Every update pass, on every screen: pushes what changed into the sheet
 * while it is open; nothing otherwise. */
void settings_update(Ui *u);

#endif /* SMP_SCREENS_SETTINGS_H */
