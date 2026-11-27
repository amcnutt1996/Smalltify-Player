/* signin.h: the screens before the player, each on the glass card (the
 * shell's card page): Move (a read-only app folder), Setup (no client ID:
 * paste it or reload .env), Login (sign in with Spotify; over the running
 * player, ask for new permissions, with Later) and the Soloist key.
 *
 * Owner: the Sheets module (screens/signin*.c), with settings.c. */
#ifndef SMP_SCREENS_SIGNIN_H
#define SMP_SCREENS_SIGNIN_H

#include "ui_state.h"

/* Builds into parent (shell_card_slot()), once at startup. */
void signin_build(Ui *u, Rid parent);

/* Every update pass while one of these screens shows (u->screen): shows
 * that screen's content and pushes what changed into it. */
void signin_update(Ui *u);

#endif /* SMP_SCREENS_SIGNIN_H */
