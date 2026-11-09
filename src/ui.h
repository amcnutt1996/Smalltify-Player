/* GUI. Event-waiting mode (Raven): redraws only on input or poller/art
 * wakeups, so an idle window costs ~0% CPU. */
#ifndef UI_H
#define UI_H

#include "config.h"

/* Runs the window until closed. Shows a sign-in screen first when not
 * logged in (or a "set SPOTIFY_CLIENT_ID in .env" notice). Returns exit code. */
int ui_run(const Config *cfg);

#endif
