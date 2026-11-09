/* signin.h: the screens before the player: Setup (no client ID), Move (a
 * read-only app folder), Login and the Soloist API key. */
#ifndef SMP_SCREENS_SIGNIN_H
#define SMP_SCREENS_SIGNIN_H

#include "ui_state.h"

void draw_setup(Ui *u);
void draw_move(Ui *u);
void draw_login(Ui *u);
void draw_key(Ui *u);

#endif /* SMP_SCREENS_SIGNIN_H */
