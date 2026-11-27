/* sidebar.c: the sidebar's content (sidebar.h): the Library and Devices
 * tabs over their pages (sidebar_library.c, sidebar_devices.c), and at the
 * bottom the device playing, Log out (Sign in when signed out, and Sign in
 * again while Spotify waits for new permissions) and Settings. */
#include "screens/sidebar.h"

#include <string.h>

#include "gui/components.h"
#include "gui/shell.h"
#include "gui/window.h"
#include "library.h"
#include "screens/sidebar_tabs.h"
#include "ui_internal.h"

/* Escape while nothing has the keyboard: back one page of the Library. */
#define ACT_LIBRARY_BACK "library.back"

enum { TAB_LIBRARY, TAB_DEVICES, TABS };

static struct {
    Ui *u;
    Rid tabs, pages;
    int tab;
    int shown;       /* the sidebar was in view at the last pass */
    Poller *poller;  /* the session seen at the last pass */
    Rid device, device_name, reauth, account;
    int device_icon; /* Icon + 1 shown, 0 = none yet */
    int signed_in;   /* what the account button offers: 1 Log out, 0 Sign in, -1 not yet */
} g;

/* A tab came into view: what it shows may have changed meanwhile. */
static void tab_opened(void)
{
    if (g.tab == TAB_LIBRARY)
        lib_opened(g.u);
    else
        cmd_post(&g.u->cmd, CMD_DEVICES, 0); /* devices come and go: always ask */
}

static void show_tab(int tab)
{
    if (tab == g.tab)
        return;
    g.tab = tab;
    comp_segmented_set(g.tabs, tab);
    Rswitcher_set_index(g.pages, tab);
    Rwidget_play_transition(Rwidget_get_child(g.pages, tab), R_TRANSITION_FADE, 0.0f);
    tab_opened();
}

static void on_tab(int index, void *ud)
{
    (void)ud;
    show_tab(index);
}

static void on_escape(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    (void)ud;
    if (g.u->screen == SCR_PLAYER && g.tab == TAB_LIBRARY && shell_sidebar_shown())
        lib_pop(g.u);
}

static void on_account(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    Ui *u = g.u;
    if (u->screen != SCR_PLAYER || !u->auth)
        return;
    if (auth_store_logged_in(u->auth))
        log_out(u);
    else
        advance_signin(u);
}

static void on_reauth(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    if (g.u->screen == SCR_PLAYER)
        start_reauth(g.u, 1);
}

static void on_settings(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    Raction_fire(ACT_SETTINGS, R_INVALID_ID);
}

static void footer_build(Rid parent)
{
    Rid line = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_SEPARATOR));
    Rseparator_set_orientation(line, R_HORIZONTAL);
    Rid foot = comp_hbox(parent, COMP_GAP);
    g.device = icon_image(foot, ICON_DEVICES, 20.0f);
    Rwidget_add_class(g.device, "muted");
    Rslot_set_valign(g.device, R_VALIGN_CENTER);
    Rslot_set_padding(g.device, 4.0f, 0.0f, 2.0f, 0.0f);
    Rid texts = comp_vbox(foot, 0.0f);
    Rslot_set_fill(texts, 1.0f);
    Rslot_set_valign(texts, R_VALIGN_CENTER);
    Rid caption = comp_text(texts, "Playing on", TEXT_CAPTION);
    Rwidget_add_class(caption, "muted");
    g.device_name = comp_text(texts, "", TEXT_BODY);
    g.reauth = comp_icon_button(foot, ICON_LOG_IN, ICON_BUTTON_MEDIUM, "Sign in again to allow new features",
                                on_reauth, NULL);
    Rslot_set_valign(g.reauth, R_VALIGN_CENTER);
    comp_icon_button_set_on(g.reauth, 1);
    comp_set_visible(g.reauth, 0);
    g.account = comp_icon_button(foot, ICON_LOG_OUT, ICON_BUTTON_MEDIUM, "Log out", on_account, NULL);
    Rslot_set_valign(g.account, R_VALIGN_CENTER);
    Rid gear = comp_icon_button(foot, ICON_GEAR, ICON_BUTTON_MEDIUM, "Settings", on_settings, NULL);
    Rslot_set_valign(gear, R_VALIGN_CENTER);
    g.signed_in = -1;
}

void sidebar_build(Ui *u, Rid parent)
{
    static const char *const tabs[TABS] = { "Library", "Devices" };
    g.u = u;
    g.tab = TAB_LIBRARY;
    g.tabs = comp_segmented(parent, tabs, TABS, TAB_LIBRARY, on_tab, NULL);
    g.pages = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_SWITCHER));
    Rslot_set_fill(g.pages, 1.0f);
    Rslot_set_padding(g.pages, 0.0f, 6.0f, 0.0f, 0.0f);
    library_tab_build(u, comp_vbox(g.pages, COMP_GAP));
    devices_tab_build(u, comp_vbox(g.pages, COMP_GAP));
    Rswitcher_set_index(g.pages, TAB_LIBRARY);
    footer_build(parent);
    Raction_bind(ACT_LIBRARY_BACK, on_escape, NULL);
    Rwindow_register_shortcut(window_id(), R_KEY_ESCAPE, 0, ACT_LIBRARY_BACK);
}

/* The device playing (or the one a click played on), and its kind. */
static void footer_update(Ui *u)
{
    const PlaybackState *s = &u->st;
    const char *name = "No active device";
    Icon icon = ICON_DEVICES;
    if (s->active && s->device_name[0]) {
        int ours = strcmp(s->device_name, u->cfg.device_name) == 0;
        name = ours ? "This computer" : s->device_name;
        icon = ours ? ICON_COMPUTER : ICON_SPEAKER;
        for (int i = 0; !ours && i < u->devs.ndev; i++)
            if (strcmp(u->devs.devs[i].id, s->device_id) == 0)
                icon = device_icon(u->devs.devs[i].type);
    } else if (u->played_on[0]) {
        name = u->played_on;
    }
    comp_text_set(g.device_name, name);
    if (g.device_icon != (int)icon + 1) {
        g.device_icon = (int)icon + 1;
        icon_set(g.device, icon);
    }

    int in = u->auth && auth_store_logged_in(u->auth);
    comp_set_visible(g.reauth, in && auth_store_needs_reconsent(u->auth));
    comp_set_enabled(g.account, u->auth != NULL); /* the preview has no session to sign in to */
    if (in != g.signed_in) {
        g.signed_in = in;
        comp_icon_button_set(g.account, in ? ICON_LOG_OUT : ICON_LOG_IN, in ? "Log out" : "Sign in");
    }
}

void sidebar_update(Ui *u)
{
    lib_sync(u);
    int shown = shell_sidebar_shown();
    if ((shown && !g.shown) || u->poller != g.poller) /* back in sight, or a new session */
        tab_opened();
    g.shown = shown;
    g.poller = u->poller;
    library_tab_update(u);
    devices_tab_update(u);
    footer_update(u);
}

void sidebar_find(Ui *u)
{
    (void)u;
    show_tab(TAB_LIBRARY);
    library_tab_find();
}
