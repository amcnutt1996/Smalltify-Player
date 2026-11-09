/* signin.c: the screens before the player: Setup, Move, Login and the
 * Soloist API key. Drawing and layout only; the sign-in logic stays in ui.c. */

#include "signin.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../ui_internal.h"
#include "common.h"

#define SPOTIFY_DASHBOARD_URL "https://developer.spotify.com/dashboard"
#define SOLOIST_KEY_URL "https://developer.spotify.com/dashboard/soloist"

/* <app>/.env hard-wrapped to the window (a path has no spaces). */
static void wrap_env_path(Ui *u, float maxw)
{
    char env_path[CFG_PATH_LEN];
    if (plat_path_join(env_path, sizeof env_path, u->cfg.app_dir, ".env") != 0)
        snprintf(env_path, sizeof env_path, ".env");
    gui_wrap_path(px_of(u, FS_S), env_path, maxw, u->env_wrapped, sizeof u->env_wrapped);
}

/* --- setup screen (no client id) ----------------------------------------- */
void draw_setup(Ui *u)
{
    const Metrics *m = &u->m;
    float maxw = (float)(u->w - 2 * PAD);
    if (u->text_dirty) {
        wrap_env_path(u, maxw);
        u->text_dirty = 0;
    }
    /* The one field on this screen always has the keyboard. The client ID
     * is public (PKCE), so it is shown as typed. */
    gui_edit_line(u->id_input, sizeof u->id_input, 0);
    int enter = gui_input.key_enter;

    float y = 48;
    text_center(u, "Smalltify Player", y, FS_L, GUI_TEXT);
    y += (float)m->fl + 28;
    y += para(u, "A Spotify client ID is needed.", PAD, y, maxw, GUI_TEXT) + 14;
    text(u, "1. Create an app at", PAD, y, FS_S, GUI_DIM);
    y += m->line;
    text(u, "developer.spotify.com/dashboard", PAD, y, FS_S, GUI_TEXT);
    y += (float)m->fs + 8;
    GuiRect ro = { PAD, y, btn_w(u, "Open the page", 120), (float)m->fs + 14 };
    if (gui_button(W_OPEN_DASH, ro, "Open the page", px_of(u, FS_S), 0) &&
        plat_open_url(SPOTIFY_DASHBOARD_URL) != 0)
        u->setup_msg = "Could not open the browser.";
    y += ro.h + 12;
    y += para(u, "Set this redirect URI in the app:", PAD, y, maxw, GUI_DIM) + 8;
    text(u, u->cfg.redirect_uri, PAD, y, FS_S, GUI_TEXT);
    y += (float)m->fs + 18;
    text(u, "2. Paste its Client ID here and press Save:", PAD, y, FS_S, GUI_DIM);
    y += (float)m->fs + 8;
    float bw = fmaxf(btn_w(u, "Save", 72), btn_w(u, "Reload", 72)), gap = 6;
    GuiRect box = { PAD, y, maxw - bw - gap, m->row };
    GuiRect rb = { PAD + box.w + gap, y, bw, m->row };
    draw_field(u, box, u->id_input, "Paste the Client ID");
    /* Empty box: re-read a client ID added to .env by hand. */
    if (gui_button(W_RELOAD, rb, u->id_input[0] ? "Save" : "Reload", px_of(u, FS_S), 1) || enter) {
        setup_save(u);
        if (u->screen != SCR_SETUP)
            return;
    }
    y += m->row + 8;
    if (u->setup_msg)
        y += para_center(u, u->setup_msg, y, GUI_ERR) + 8;
    y += para(u, "Or put SPOTIFY_CLIENT_ID=... in this file\nand press Reload:", PAD, y, maxw,
              GUI_DIM) + 6;
    text(u, u->env_wrapped, PAD, y, FS_S, GUI_TEXT);
    y += text_h(u, u->env_wrapped);
    u->need_h = y + (float)m->fs + PAD;
}

/* --- move screen (app folder not writable) -------------------------------- */
void draw_move(Ui *u)
{
    const Metrics *m = &u->m;
    float y = 120;
    text_center(u, "Smalltify Player", y, FS_L, GUI_TEXT);
    y += (float)m->fl + 36;
    text_center(u, "This folder is read-only.", y, FS_S, GUI_ERR);
    y += (float)m->fs + 18;
    text_center(u, "Move Smalltify Player to Applications", y, FS_S, GUI_TEXT);
    y += m->line;
    text_center(u, "(or another writable folder)", y, FS_S, GUI_TEXT);
    y += m->line;
    text_center(u, "and open it again.", y, FS_S, GUI_TEXT);
    u->need_h = y + (float)m->fs + PAD;
}

/* --- login screen -------------------------------------------------------- */
/* Our PKCE login for the Web API. With reauth set it is over the running
 * player, asking an older sign-in for new permissions: Later goes back. */
void draw_login(Ui *u)
{
    const Metrics *m = &u->m;
    text_center(u, "Smalltify Player", 120, FS_L, GUI_TEXT);
    if (u->login_th) {
        plat_mutex_lock(u->mu);
        int done = u->login_done, rc = u->login_rc;
        plat_mutex_unlock(u->mu);
        if (done) {
            int cancelled = u->login_cancel;
            login_join(u);
            if (rc == 0) {
                u->login_msg = NULL;
                u->consent_asked = 1; /* it got what Spotify grants */
                if (u->reauth)
                    end_reauth(u, 1);
                else
                    advance_signin(u);
                return;
            }
            u->login_msg = cancelled ? "Sign-in cancelled." : auth_login_strerror(rc);
        }
    }

    float y = 120 + (float)m->fl + 48;
    if (u->reauth) {
        y += para_center(u, AUTH_RECONSENT_REASON, y, GUI_TEXT) + 26;
    } else {
        text_center(u, "Sign in to control your music.", y, FS_S, GUI_TEXT);
        y += (float)m->fs + 26;
    }
    float bw = btn_w(u, "Sign in with Spotify", 200), bh = (float)m->fs + 30;
    GuiRect rb = { ((float)u->w - bw) / 2.0f, y, bw, bh };
    if (!u->login_th) {
        if (gui_button(W_SIGNIN, rb, "Sign in with Spotify", px_of(u, FS_S), 1))
            start_login(u);
        /* Over the player: go on without the new permissions. Else a client
         * ID fixed in .env applies without a restart. */
        const char *second = u->reauth ? "Later" : "Reload .env";
        float rw = btn_w(u, second, 120), rh = (float)m->fs + 14;
        GuiRect rr = { ((float)u->w - rw) / 2.0f, y + bh + 8, rw, rh };
        u->need_h = rr.y + rh + 12 + (float)m->fs + PAD;
        if (u->reauth && gui_button(W_LATER, rr, second, px_of(u, FS_S), 0)) {
            end_reauth(u, 0);
            return;
        }
        if (!u->reauth && gui_button(W_RELOAD, rr, second, px_of(u, FS_S), 0)) {
            Config c;
            AuthStore *a = NULL;
            if (config_load(&c) == 0 && (a = auth_store_new(&c)) != NULL) {
                auth_store_free(u->auth); /* nothing else uses it on this screen */
                u->auth = a;
                u->cfg = c;
                u->login_msg = NULL;
                if (!c.client_id[0])
                    set_screen(u, SCR_SETUP);
            }
            memset(&c, 0, sizeof c);
            return;
        }
        if (u->login_msg)
            para_center(u, u->login_msg, rr.y + rh + 12, GUI_ERR);
        return;
    }
    text_center(u, u->login_cancel ? "Cancelling..." : "Waiting for browser...", mid_y(u, rb),
                FS_S, GUI_TEXT);
    float by = y + bh + 8;
    plat_mutex_lock(u->mu);
    int has_url = u->login_url[0] != 0;
    plat_mutex_unlock(u->mu);
    if (has_url && !u->login_cancel) {
        float ow = btn_w(u, "Open sign-in page again", 200);
        GuiRect ro = { ((float)u->w - ow) / 2.0f, by, ow, m->row };
        if (gui_button(W_OPEN_URL, ro, "Open sign-in page again", px_of(u, FS_S), 0))
            open_again(u);
        by += m->row + 8;
    }
    float xw = btn_w(u, "Cancel", 100);
    GuiRect rc = { ((float)u->w - xw) / 2.0f, by, xw, m->row };
    u->need_h = by + m->row + 12 + (float)m->fs + PAD;
    if (gui_button(W_CANCEL, rc, "Cancel", px_of(u, FS_S), 0))
        u->login_cancel = 1;
    if (u->login_msg)
        para_center(u, u->login_msg, by + m->row + 12, GUI_ERR);
}

/* --- Soloist key screen -------------------------------------------------- */
void draw_key(Ui *u)
{
    const Metrics *m = &u->m;
    float maxw = (float)(u->w - 2 * PAD);
    if (u->text_dirty) {
        wrap_env_path(u, maxw);
        u->text_dirty = 0;
    }
    /* The one field on this screen always has the keyboard. */
    gui_edit_line(u->key_input, sizeof u->key_input, 1);
    int enter = gui_input.key_enter;

    float y = 48;
    text_center(u, "Smalltify Player", y, FS_L, GUI_TEXT);
    y += (float)m->fl + 28;
    y += para(u, "To play music on this computer, it needs\na Spotify Soloist API key (optional).",
              PAD, y, maxw, GUI_TEXT) + 16;
    text(u, "1. Generate a key at", PAD, y, FS_S, GUI_DIM);
    y += m->line;
    text(u, "developer.spotify.com/dashboard/soloist", PAD, y, FS_S, GUI_TEXT);
    y += (float)m->fs + 8;
    GuiRect ro = { PAD, y, btn_w(u, "Open the page", 120), (float)m->fs + 14 };
    if (gui_button(W_OPEN_KEY, ro, "Open the page", px_of(u, FS_S), 0) &&
        plat_open_url(SOLOIST_KEY_URL) != 0)
        u->setup_msg = "Could not open the browser.";
    y += ro.h + 12;
    text(u, "2. Paste it here and press Save:", PAD, y, FS_S, GUI_DIM);
    y += (float)m->fs + 8;
    float bw = fmaxf(btn_w(u, "Save", 72), btn_w(u, "Reload", 72)), gap = 6;
    GuiRect box = { PAD, y, maxw - bw - gap, m->row };
    GuiRect rb = { PAD + box.w + gap, y, bw, m->row };
    char masked[SP_NAME_LEN];
    mask_secret(u->key_input, masked, sizeof masked);
    draw_field(u, box, masked, "Paste the key (it stays hidden)");
    /* Empty box: re-read a key added to .env by hand. */
    if (gui_button(W_RELOAD, rb, u->key_input[0] ? "Save" : "Reload", px_of(u, FS_S), 1) || enter) {
        save_key(u);
        if (u->screen != SCR_KEY)
            return;
    }
    y += m->row + 12;
    y += para(u, "Or put SOLOIST_API_KEY=... in this file\nand press Reload:", PAD, y, maxw,
              GUI_DIM) + 6;
    text(u, u->env_wrapped, PAD, y, FS_S, GUI_TEXT);
    y += text_h(u, u->env_wrapped) + 16;

    float sw = btn_w(u, "Back", 100);
    GuiRect rs = { ((float)u->w - sw) / 2.0f, y, sw, m->row };
    u->need_h = y + m->row + 12 + (float)m->fs + PAD;
    if (gui_button(W_BACK, rs, "Back", px_of(u, FS_S), 0)) {
        leave_key(u);
        return;
    }
    if (u->setup_msg)
        para_center(u, u->setup_msg, y + m->row + 12, GUI_ERR);
}
