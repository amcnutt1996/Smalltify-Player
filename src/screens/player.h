/* The player screen: the art, the track, the transport and the bottom bar,
 * with the Library, Devices and Settings panels over its area. Drawing
 * only; the logic it calls stays in ui.c. */
#ifndef SMP_SCREENS_PLAYER_H
#define SMP_SCREENS_PLAYER_H

#include "ui_state.h"

/* The heart's square at the end of the title's line. */
float heart_side(const Metrics *m);

/* The player's height without the art: padding, the rows below the art,
 * the status lines and the bottom bar. */
float player_reserve(const Metrics *m);

/* The player screen: one frame. */
void draw_player(Ui *u);

#endif /* SMP_SCREENS_PLAYER_H */
