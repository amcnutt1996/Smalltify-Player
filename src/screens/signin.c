/* signin.c: the card screens (signin.h). Every screen is a page of the one
 * card, under the same header: the app's mark, its name and a line about
 * the screen. The steps themselves (saving, signing in, reloading .env)
 * stay in ui.c. */
#include "screens/signin.h"

#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "gui/components.h"
#include "gui/shell.h"
#include "platform.h"
#include "screens/signin_badge.h"
#include "ui_internal.h"

#define SPOTIFY_DASHBOARD_URL "https://developer.spotify.com/dashboard"
#define SOLOIST_KEY_URL       "https://developer.spotify.com/dashboard/soloist"
#define ELLIPSIS              "\xE2\x80\xA6" /* U+2026 */
#define OPEN_FAILED           "Could not open the browser."

enum { PAGES = SCR_PLAYER }; /* one page per screen before the player */

static struct {
    Ui    *u;
    Screen shown; /* the page shown; SCR_PLAYER before the first pass */
    Rid    about; /* the header's line about the screen */
    Rid    page[PAGES];
    /* setup */
    Rid id_field, id_save, uri, setup_err, setup_env;
    int id_typed; /* id_save says Save (else Reload) */
    /* login */
    Rid idle, later, reload, waiting, wait_text, again_row, login_err;
    /* key */
    Rid key_field, key_save, key_err, key_env;
    int key_typed;
} g;

/* ---- pieces --------------------------------------------------------------------- */

static Rid centered(Rid label)
{
    Rlabel_set_text_align(label, R_TEXT_ALIGN_CENTER);
    return label;
}

static Rid with_class(Rid widget, const char *cls)
{
    Rwidget_add_class(widget, cls);
    return widget;
}

/* A numbered step: the number in a ring, then the text; returns the column
 * under the text, where the step's content goes. */
static Rid step(Rid parent, const char *number, const char *text)
{
    Rid row = comp_hbox(parent, 12.0f);
    Rid ring = Rwidget_create(R_WIDGET_BORDER);
    Rwidget_add_child(row, ring);
    Rborder_set_corner_radius(ring, 11.0f, 11.0f, 11.0f, 11.0f);
    Rborder_set_outline(ring, 1.5f, 1.0f, 1.0f, 1.0f, 0.3f);
    Rwidget_set_size(ring, 22.0f, 22.0f);
    Rslot_set_valign(ring, R_VALIGN_TOP);
    Rwidget_set_accessible_hidden(ring, R_TRUE);
    Rid n = with_class(centered(comp_label(ring, number, TEXT_CAPTION)), "on");
    Rslot_set_valign(n, R_VALIGN_CENTER);
    Rid col = comp_vbox(row, COMP_GAP);
    Rslot_set_fill(col, 1.0f);
    comp_label(col, text, TEXT_BODY);
    return col;
}

/* A web address the step is about, and the button that opens it. */
static void web_page(Rid col, const char *shown, RButtonClickedCallback open)
{
    with_class(comp_text(col, shown, TEXT_BODY), "muted");
    Rid b = comp_button(col, "Open the page", BUTTON_SECONDARY, open, g.u);
    Rslot_set_halign(b, R_HALIGN_LEFT);
}

/* A field with its Save / Reload button at its end. */
static Rid field_row(Rid col, const char *placeholder, RTextChangedCallback on_change,
                     RTextChangedCallback on_commit, RButtonClickedCallback on_save, Rid *save)
{
    Rid row = comp_hbox(col, COMP_GAP);
    Rid field = comp_field(row, placeholder, 0, on_change, on_commit, g.u);
    Rslot_set_fill(field, 1.0f);
    Rslot_set_valign(field, R_VALIGN_CENTER);
    *save = comp_button(row, "Reload", BUTTON_PRIMARY, on_save, g.u);
    Rwidget_set_min_size(*save, 84.0f, -1.0f);
    Rslot_set_valign(*save, R_VALIGN_CENTER);
    return field;
}

static Rid error_line(Rid parent)
{
    Rid l = with_class(comp_label(parent, "", TEXT_BODY), "error");
    comp_set_visible(l, 0);
    return l;
}

/* "Or put NAME=... in this file and press Reload:" and the file. */
static Rid env_footer(Rid parent, const char *text)
{
    Rid box = comp_vbox(parent, 4.0f);
    with_class(comp_label(box, text, TEXT_CAPTION), "muted");
    Rid path = comp_label(box, "", TEXT_CAPTION);
    Rlabel_set_selectable(path, R_TRUE);
    return path;
}

static void show_error(Rid label, const char *msg)
{
    comp_text_set(label, msg);
    comp_set_visible(label, msg && msg[0]);
}

/* <app>/.env, where the settings are kept. */
static void show_env_path(Rid label, const Ui *u)
{
    char path[CFG_PATH_LEN];
    if (plat_path_join(path, sizeof path, u->cfg.app_dir, ".env") != 0)
        snprintf(path, sizeof path, ".env");
    comp_text_set(label, path);
}

/* An empty field re-reads .env (a value added there by hand). */
static void show_save(Rid button, int *shown, int typed)
{
    if (*shown == typed)
        return;
    *shown = typed;
    comp_button_set_text(button, typed ? "Save" : "Reload");
}

/* ---- setup --------------------------------------------------------------------- */

static void on_open_dashboard(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    if (plat_open_url(SPOTIFY_DASHBOARD_URL) != 0)
        u->setup_msg = OPEN_FAILED;
}

static void on_copy_uri(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    Rclipboard_set_text(u->cfg.redirect_uri);
    shell_toast("Redirect URI copied", 0);
}

/* The client ID is public (PKCE): shown as typed, kept in id_input. */
static void on_id_changed(Rid field, const char *text, void *ud)
{
    (void)field;
    Ui *u = ud;
    snprintf(u->id_input, sizeof u->id_input, "%s", text);
}

static void on_id_commit(Rid field, const char *text, void *ud)
{
    on_id_changed(field, text, ud);
    setup_save(ud);
}

static void on_id_save(Rid button, void *ud)
{
    (void)button;
    setup_save(ud);
}

static void build_setup(Rid p)
{
    Rid s1 = step(p, "1", "Create an app at");
    web_page(s1, "developer.spotify.com/dashboard", on_open_dashboard);
    with_class(comp_label(s1, "Set this redirect URI in the app:", TEXT_BODY), "muted");
    Rid row = comp_hbox(s1, COMP_GAP);
    g.uri = comp_text(row, "", TEXT_BODY);
    Rslot_set_fill(g.uri, 1.0f);
    Rslot_set_valign(g.uri, R_VALIGN_CENTER);
    Rid copy = comp_button(row, "Copy", BUTTON_LINK, on_copy_uri, g.u);
    Rslot_set_valign(copy, R_VALIGN_CENTER);

    Rid s2 = step(p, "2", "Paste its Client ID here and press Save:");
    g.id_field = field_row(s2, "Paste the Client ID", on_id_changed, on_id_commit, on_id_save, &g.id_save);
    Rtext_input_set_max_length(g.id_field, (int)sizeof g.u->id_input - 1);
    g.setup_err = error_line(s2);

    g.setup_env = env_footer(p, "Or put SPOTIFY_CLIENT_ID=... in this file and press Reload:");
}

static void update_setup(Ui *u)
{
    comp_field_set(g.id_field, u->id_input); /* setup_save trims or empties it */
    comp_text_set(g.uri, u->cfg.redirect_uri);
    show_save(g.id_save, &g.id_typed, u->id_input[0] != 0);
    show_error(g.setup_err, u->setup_msg);
    show_env_path(g.setup_env, u);
}

/* ---- login ---------------------------------------------------------------------- */

static void on_sign_in(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    if (u->auth && !u->login_th)
        start_login(u);
}

static void on_reload_env(Rid button, void *ud)
{
    (void)button;
    reload_env(ud);
}

/* Over the player: go on without the new permissions. */
static void on_later(Rid button, void *ud)
{
    (void)button;
    end_reauth(ud, 0);
}

static void on_open_again(Rid button, void *ud)
{
    (void)button;
    open_again(ud);
}

static void on_copy_link(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    char url[SIGNIN_URL_LEN];
    plat_mutex_lock(u->mu);
    memcpy(url, u->login_url, sizeof url);
    plat_mutex_unlock(u->mu);
    if (!url[0])
        return;
    Rclipboard_set_text(url);
    shell_toast("Sign-in link copied", 0);
}

static void on_cancel(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    u->login_cancel = 1;
}

static void build_login(Rid p)
{
    g.idle = comp_vbox(p, 10.0f);
    comp_button(g.idle, "Sign in with Spotify", BUTTON_PRIMARY, on_sign_in, g.u);
    g.reload = comp_button(g.idle, "Reload .env", BUTTON_SECONDARY, on_reload_env, g.u);
    g.later = comp_button(g.idle, "Later", BUTTON_SECONDARY, on_later, g.u);

    g.waiting = comp_vbox(p, 10.0f);
    g.wait_text = with_class(centered(comp_label(g.waiting, "", TEXT_BODY)), "muted");
    g.again_row = comp_hbox(g.waiting, COMP_GAP);
    Rslot_set_halign(g.again_row, R_HALIGN_CENTER);
    comp_button(g.again_row, "Open sign-in page again", BUTTON_SECONDARY, on_open_again, g.u);
    Rid copy = comp_button(g.again_row, "Copy link", BUTTON_LINK, on_copy_link, g.u);
    Rslot_set_valign(copy, R_VALIGN_CENTER);
    comp_button(g.waiting, "Cancel", BUTTON_SECONDARY, on_cancel, g.u);

    g.login_err = centered(error_line(p));
}

static void update_login(Ui *u)
{
    int waiting = u->login_th != NULL;
    comp_set_visible(g.idle, !waiting);
    comp_set_visible(g.waiting, waiting);
    if (waiting) {
        plat_mutex_lock(u->mu);
        int has_url = u->login_url[0] != 0;
        plat_mutex_unlock(u->mu);
        comp_text_set(g.wait_text, u->login_cancel ? "Cancelling" ELLIPSIS : "Waiting for browser" ELLIPSIS);
        comp_set_visible(g.again_row, has_url && !u->login_cancel);
    } else {
        /* Over the player: Later. Else a client ID fixed in .env applies
         * without a restart. */
        comp_set_visible(g.later, u->reauth);
        comp_set_visible(g.reload, !u->reauth);
    }
    show_error(g.login_err, u->login_msg);
}

/* ---- key ------------------------------------------------------------------------- */

static void on_open_key_page(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    if (plat_open_url(SOLOIST_KEY_URL) != 0)
        u->setup_msg = OPEN_FAILED;
}

static void on_key_save(Rid button, void *ud)
{
    (void)button;
    save_key(ud);
}

static void on_back(Rid button, void *ud)
{
    (void)button;
    leave_key(ud);
}

/* What an API key holds: printable ASCII, no blanks (a pasted line break or
 * space is dropped). */
static void key_append(Ui *u, const char *s)
{
    size_t n = strlen(u->key_input);
    for (; *s && n + 1 < sizeof u->key_input; s++)
        if (*s > ' ' && *s <= '~')
            u->key_input[n++] = *s;
    u->key_input[n] = 0;
}

/* The key never enters the field: it is kept in key_input, and the field
 * shows it masked (mask_secret: only its last 4 characters). So the field
 * takes typing, Backspace and paste here, at the end of the key, as
 * before; Enter saves, Escape goes back. */
static int on_key_event(Rid field, const REvent *e, void *ud)
{
    (void)field;
    Ui *u = ud;
    if (e->type == R_EVENT_TEXT) {
        char c[2] = { (char)e->codepoint, 0 };
        if (e->codepoint < 128)
            key_append(u, c);
        return R_TRUE;
    }
    if (e->type != R_EVENT_KEY_DOWN)
        return R_FALSE;
    int command = (e->modifiers & (R_MOD_SUPER | R_MOD_CTRL)) != 0;
    switch (e->key) {
    case R_KEY_TAB:
        return R_FALSE;
    case R_KEY_ESCAPE:
        if (!e->repeat)
            leave_key(u);
        return R_TRUE;
    case R_KEY_ENTER:
    case R_KEY_KP_ENTER:
        if (!e->repeat)
            save_key(u);
        return R_TRUE;
    case R_KEY_BACKSPACE: {
        size_t n = strlen(u->key_input);
        if (command)
            n = 0;
        else if (n > 0)
            n--;
        memset(u->key_input + n, 0, sizeof u->key_input - n);
        return R_TRUE;
    }
    case R_KEY_V:
        if (command) {
            const char *clip = Rclipboard_get_text();
            if (clip && !e->repeat)
                key_append(u, clip);
        }
        return R_TRUE;
    default:
        /* other shortcuts (Quit, Settings...) go on; the field's own
         * editing (caret, selection, cut) does not apply to the mask */
        return !command || e->key == R_KEY_A || e->key == R_KEY_C || e->key == R_KEY_X;
    }
}

static void build_key(Rid p)
{
    Rid s1 = step(p, "1", "Generate a key at");
    web_page(s1, "developer.spotify.com/dashboard/soloist", on_open_key_page);

    Rid s2 = step(p, "2", "Paste it here and press Save:");
    g.key_field = field_row(s2, "Paste the key (it stays hidden)", NULL, NULL, on_key_save, &g.key_save);
    Rwidget_set_event_callback(g.key_field, on_key_event, g.u);
    g.key_err = error_line(s2);

    g.key_env = env_footer(p, "Or put SOLOIST_API_KEY=... in this file and press Reload:");
    comp_button(p, "Back", BUTTON_SECONDARY, on_back, g.u);
}

static void update_key(Ui *u)
{
    char masked[sizeof u->key_input];
    mask_secret(u->key_input, masked, sizeof masked);
    comp_field_set(g.key_field, masked);
    show_save(g.key_save, &g.key_typed, u->key_input[0] != 0);
    show_error(g.key_err, u->setup_msg);
    show_env_path(g.key_env, u);
}

/* ---- the card ---------------------------------------------------------------------- */

void signin_build(Ui *u, Rid parent)
{
    memset(&g, 0, sizeof g);
    g.u = u;
    g.shown = SCR_PLAYER;
    g.id_typed = g.key_typed = -1;

    Rid root = comp_vbox(parent, 22.0f);
    Rid head = comp_vbox(root, 10.0f);
    Rslot_set_halign(signin_badge(head, 60.0f), R_HALIGN_CENTER);
    centered(comp_label(head, "Smalltify Player", TEXT_LARGE));
    g.about = centered(comp_label(head, "", TEXT_BODY));

    for (int i = 0; i < PAGES; i++) {
        g.page[i] = comp_vbox(root, 18.0f);
        comp_set_visible(g.page[i], 0);
    }
    centered(comp_label(g.page[SCR_MOVE],
                        "Move Smalltify Player to Applications\n(or another writable folder)\nand open it again.",
                        TEXT_BODY));
    build_setup(g.page[SCR_SETUP]);
    build_login(g.page[SCR_LOGIN]);
    build_key(g.page[SCR_KEY]);
}

void signin_update(Ui *u)
{
    Screen s = u->screen;
    if (s >= PAGES)
        return;
    if (s != g.shown) {
        for (int i = 0; i < PAGES; i++)
            comp_set_visible(g.page[i], i == (int)s);
        g.shown = s;
        /* the one field of the screen has the keyboard, as before */
        if (s == SCR_SETUP)
            Rwidget_focus(g.id_field);
        else if (s == SCR_KEY)
            Rwidget_focus(g.key_field);
    }

    static const char *const about[PAGES] = {
        [SCR_MOVE] = "This folder is read-only.",
        [SCR_SETUP] = "A Spotify client ID is needed.",
        [SCR_LOGIN] = "Sign in to control your music.",
        [SCR_KEY] = "To play music on this computer, it needs a Spotify Soloist API key (optional).",
    };
    comp_text_set(g.about, s == SCR_LOGIN && u->reauth ? AUTH_RECONSENT_REASON : about[s]);
    comp_set_class(g.about, "error", s == SCR_MOVE);

    switch (s) {
    case SCR_SETUP: update_setup(u); break;
    case SCR_LOGIN: update_login(u); break;
    case SCR_KEY:   update_key(u); break;
    default:        break;
    }
}
