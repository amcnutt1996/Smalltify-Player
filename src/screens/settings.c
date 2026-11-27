/* settings.c: the Settings sheet (settings.h): the text size, the account
 * and what the app is. */
#include "screens/settings.h"

#include <math.h>

#include "auth.h"
#include "gui/components.h"
#include "screens/signin_badge.h"
#include "ui_internal.h"

#define SHEET_W 440.0f /* at text scale 1; it grows with the square root of the scale, as the card does */

static const char *const g_sizes[CFG_FONT_SIZES] = { "Small", "Default", "Large", "Extra large" };

/* The sheet's widgets while it is open (sheet != R_INVALID_ID); the window
 * deletes them when it closes. */
static struct {
    Rid   sheet, size, unsaved, account, log_out;
    int   signed_in; /* what account and log_out show; -1 = not yet */
    float width;     /* the sheet's, set last */
} g = { R_INVALID_ID, R_INVALID_ID, R_INVALID_ID, R_INVALID_ID, R_INVALID_ID, -1, 0.0f };

static float sheet_width(void)
{
    return roundf(SHEET_W * sqrtf(Raccessibility_get_text_scale()));
}

static void on_size(int index, void *ud)
{
    set_text_size(ud, index);
}

/* Logging out only from the player: the sign-in screens own the AuthStore
 * while they show (a sign-in may be running). */
static int signed_in(Ui *u)
{
    return u->screen == SCR_PLAYER && u->auth && auth_store_logged_in(u->auth);
}

static void on_log_out(Rid button, void *ud)
{
    Ui *u = ud;
    comp_sheet_close(button, 0);
    if (signed_in(u))
        log_out(u);
}

static void on_done(Rid button, void *ud)
{
    (void)ud;
    comp_sheet_close(button, 1);
}

static void on_closed(Rid content, int result, void *ud)
{
    (void)content;
    (void)result;
    (void)ud;
    g.sheet = g.size = g.unsaved = g.account = g.log_out = R_INVALID_ID;
    g.signed_in = -1;
}

/* A group of the sheet: its name, small and dimmed, over its content. */
static Rid group(Rid parent, const char *name)
{
    Rid box = comp_vbox(parent, COMP_GAP);
    Rwidget_add_class(comp_label(box, name, TEXT_CAPTION), "muted");
    return box;
}

void settings_open(Ui *u)
{
    if (g.sheet != R_INVALID_ID)
        return;
    g.width = sheet_width();
    CompSheet s = comp_sheet("Settings", g.width);
    Rpanel_set_spacing(s.body, 20.0f);

    Rid text = group(s.body, "Text size");
    g.size = comp_segmented(text, g_sizes, CFG_FONT_SIZES, u->cfg.font_size, on_size, u);
    g.unsaved = comp_label(text, "Could not save this in .env: it applies until the app quits.", TEXT_CAPTION);
    Rwidget_add_class(g.unsaved, "error");
    comp_set_visible(g.unsaved, 0);

    Rid row = comp_hbox(group(s.body, "Account"), COMP_GAP);
    g.account = comp_text(row, "", TEXT_BODY);
    Rslot_set_fill(g.account, 1.0f);
    Rslot_set_valign(g.account, R_VALIGN_CENTER);
    g.log_out = comp_button(row, "Log out", BUTTON_SECONDARY, on_log_out, u);
    Rslot_set_valign(g.log_out, R_VALIGN_CENTER);

    Rid about = comp_hbox(group(s.body, "About"), 12.0f);
    Rslot_set_valign(signin_badge(about, 40.0f), R_VALIGN_CENTER);
    Rid name = comp_vbox(about, 2.0f);
    Rslot_set_fill(name, 1.0f);
    Rslot_set_valign(name, R_VALIGN_CENTER);
    comp_text(name, "Smalltify Player", TEXT_HEADLINE);
#ifdef SMP_VERSION
    Rwidget_add_class(comp_text(name, "Version " SMP_VERSION, TEXT_CAPTION), "muted");
#endif

    comp_button(s.buttons, "Done", BUTTON_PRIMARY, on_done, NULL);
    g.sheet = s.root;
    comp_sheet_open(&s, on_closed, NULL);
    settings_update(u);
}

void settings_update(Ui *u)
{
    if (g.sheet == R_INVALID_ID)
        return;
    float w = sheet_width();
    if (w != g.width) { /* the text size changed: room for the larger choices */
        g.width = w;
        Rwidget_set_size(g.sheet, w, -1.0f);
    }
    comp_segmented_set(g.size, u->cfg.font_size);
    comp_set_visible(g.unsaved, u->size_unsaved);
    int in = signed_in(u);
    if (in != g.signed_in) {
        g.signed_in = in;
        comp_text_set(g.account, in ? "Signed in to Spotify" : "Not signed in");
        comp_set_visible(g.log_out, in);
    }
}
