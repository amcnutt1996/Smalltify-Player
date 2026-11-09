/* ui_internal.h: what the screens (screens/) call back into ui.c for: the
 * workers' requests, the Library's navigation, the screen transitions, the
 * sign-in and key steps, and the player's state. Defined in ui.c. */
#ifndef SMP_UI_INTERNAL_H
#define SMP_UI_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "ui_state.h"

/* --- command worker ------------------------------------------------------ */
void cmd_post_uri(CmdWorker *w, CmdKind kind, int arg, const char *uri, const char *ctx);
void cmd_post(CmdWorker *w, CmdKind kind, int arg);
/* Pull a new device list from the command worker; copies only on change. */
void sync_devices(Ui *u);

/* --- library worker and the Library's stack of screens ------------------- */
void lib_post_save(LibWorker *w, const char *uri, int on, uint64_t seq);
/* The list a screen shows, as browse_play_target takes it: -1 for search. */
int  lib_list_of(const Level *lv);
void lib_request(Ui *u, Level *lv);
void lib_pop(Ui *u);
void lib_open_list(Ui *u, SpList list, const char *title, const char *uri);
void lib_open_search(Ui *u, const char *query, unsigned types, const char *title);
void lib_open_item(Ui *u, const Item *it);
void lib_opened(Ui *u);
void lib_sync(Ui *u);

/* --- screen transitions and Soloist -------------------------------------- */
void set_screen(Ui *u, Screen s);
void stop_session(Ui *u);
void start_engine(Ui *u, int force);
void advance_signin(Ui *u);

/* --- setup, sign-in and key screens -------------------------------------- */
void setup_save(Ui *u);
void open_again(Ui *u);
void login_join(Ui *u);
void start_login(Ui *u);
void start_reauth(Ui *u, int now);
void end_reauth(Ui *u, int signed_in);
void leave_key(Ui *u);
void mask_secret(const char *s, char *out, size_t cap);
void reload_key(Ui *u);
void save_key(Ui *u);

/* --- player -------------------------------------------------------------- */
void note_other(Ui *u, const char *name);
void poll_engine(Ui *u);
int  local_ready(Ui *u);
int  progress_at(const PlaybackState *s, uint64_t now);
void pend_reapply(Ui *u);
void apply_snapshot(Ui *u, const PlaybackState *s, uint64_t now);
void pend_begin(Ui *u, int bit, uint64_t now);
void resync(Ui *u);
int  likeable(const PlaybackState *s);
int  heart_sync(Ui *u);
void refresh_player_text(Ui *u, float maxw, float status_w, float hint_w);
int  status_message(Ui *u, char *out, size_t cap);

#endif /* SMP_UI_INTERNAL_H */
