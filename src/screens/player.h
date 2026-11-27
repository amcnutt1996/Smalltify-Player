/* player.h: the main area of the player screen: the album art, the track
 * (title, artists, album) with the heart, the seek bar and its times, the
 * transport, shuffle and repeat, the volume, the status line with the
 * pairing tip, and toasts for transient notes ("Added to queue").
 *
 * Owner: the Player module (screens/player*.c). Its widgets are
 * file-static there; what it does goes through ui.c (ui_internal.h). */
#ifndef SMP_SCREENS_PLAYER_H
#define SMP_SCREENS_PLAYER_H

#include "ui_state.h"

/* Builds the player into parent (shell_main_slot()), once at startup, and
 * binds ACT_PLAY_PAUSE, ACT_PREVIOUS, ACT_NEXT and ACT_LIKE (gui/shell.h). */
void player_build(Ui *u, Rid parent);

/* Every update pass while the player page shows: pushes what changed in Ui
 * (st, art_gen, heart, status, hint...) into the widgets. */
void player_update(Ui *u);

#endif /* SMP_SCREENS_PLAYER_H */
