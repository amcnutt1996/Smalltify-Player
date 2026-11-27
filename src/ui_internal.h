/* ui_internal.h: what the screen modules (screens/) and library.c call back
 * into ui.c for: the workers' requests, the screen transitions, the
 * sign-in and key steps, and the player's state. Defined in ui.c.
 *
 * All of it runs on the UI thread. The update pass of ui.c pulls the
 * workers' news into Ui before the modules' updates run (see screens/). */
#ifndef SMP_UI_INTERNAL_H
#define SMP_UI_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "ui_state.h"

/* --- command worker ------------------------------------------------------ */
void cmd_post_uri(CmdWorker *w, CmdKind kind, int arg, const char *uri, const char *ctx);
void cmd_post(CmdWorker *w, CmdKind kind, int arg);

/* --- library worker ------------------------------------------------------ */
/* A list or search page (library.c asks through lib_request). */
void lib_post(LibWorker *w, const LibReq *r);
void lib_post_save(LibWorker *w, const char *uri, int on, uint64_t seq);

/* --- screen transitions and Soloist -------------------------------------- */
void set_screen(Ui *u, Screen s);
void stop_session(Ui *u);
void start_engine(Ui *u, int force);
void advance_signin(Ui *u);
/* Log out: stops everything that talks to Spotify (Soloist first: logging
 * out deletes its pairing), forgets the sign-in, back to the sign-in screen. */
void log_out(Ui *u);

/* --- setup, sign-in and key screens -------------------------------------- */
void setup_save(Ui *u);
void open_again(Ui *u);
void start_login(Ui *u);
void start_reauth(Ui *u, int now);
void end_reauth(Ui *u, int signed_in);
/* The sign-in screen's "Reload .env": a client ID fixed in .env applies
 * without a restart (the setup screen when there is none). */
void reload_env(Ui *u);
void leave_key(Ui *u);
void mask_secret(const char *s, char *out, size_t cap);
void reload_key(Ui *u);
void save_key(Ui *u);

/* --- settings ------------------------------------------------------------ */
/* A CFG_FONT_* text size: applies at once (theme_set_text_size) and is
 * saved to .env as UI_FONT_SIZE; size_unsaved says when that failed. */
void set_text_size(Ui *u, int size);

/* --- player -------------------------------------------------------------- */
int  local_ready(Ui *u);
int  progress_at(const PlaybackState *s, uint64_t now);
void pend_reapply(Ui *u);
void pend_begin(Ui *u, int bit, uint64_t now);
void resync(Ui *u);
int  likeable(const PlaybackState *s);

#endif /* SMP_UI_INTERNAL_H */
