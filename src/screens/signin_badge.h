/* signin_badge.h: the application's mark, the logo tile (src/gui/logo.png,
 * compiled in). It heads every card (signin.c) and the Settings
 * sheet's About line (settings.c), so they read as one family.
 *
 * Owner: the Sheets module. */
#ifndef SMP_SCREENS_SIGNIN_BADGE_H
#define SMP_SCREENS_SIGNIN_BADGE_H

#include <Raven.h>

/* Adds the mark, side x side units, to parent; returns it. */
Rid signin_badge(Rid parent, float side);

#endif /* SMP_SCREENS_SIGNIN_BADGE_H */
