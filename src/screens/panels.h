/* The Devices and Settings panels of the player. Drawing only; the logic
 * they call stays in ui.c. */
#ifndef SMP_SCREENS_PANELS_H
#define SMP_SCREENS_PANELS_H

#include "ui_state.h"

/* Spotify Connect devices; a click moves playback there via the worker. */
void draw_devices(Ui *u, GuiRect area);

/* The text size: a pick applies at once and is saved to .env as
 * UI_FONT_SIZE. */
void draw_settings(Ui *u, GuiRect area);

#endif /* SMP_SCREENS_PANELS_H */
