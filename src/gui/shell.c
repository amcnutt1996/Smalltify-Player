/*
 * shell.c: the frame of every screen (shell.h): pages, sidebar and its
 * slide, title bands, card, toast and menus.
 *
 *   root VBox
 *     MenuBar                    (the system's on macOS: takes no room)
 *     Overlay
 *       Switcher                 pages
 *         Overlay                SHELL_PLAYER
 *           VBox                 main area, left padding = room the sidebar takes
 *             HBox               title band: traffic lights, show button, title slot
 *             VBox               main slot
 *           Border (glass)       sidebar, floating; slides by its left padding
 *             VBox: HBox (top row: hide button), VBox (sidebar slot)
 *         Overlay                SHELL_CARD
 *           HBox                 title band
 *           Border (glass)       card: ScrollBox > VBox (card slot)
 *       Toast
 */
#include "shell.h"

#include <math.h>
#include <string.h>

#include "components.h"
#include "icons.h"
#include "macos.h"
#include "theme.h"
#include "window.h"

#ifdef __APPLE__
#define TITLE_H      ((float)MAC_TITLE_H)
#define LIGHTS_W     ((float)MAC_LIGHTS_W)
#define MOD          R_MOD_SUPER
#define SIDEBAR_MODS (R_MOD_SUPER | R_MOD_CTRL)
#else
#define TITLE_H      44.0f
#define LIGHTS_W     0.0f
#define MOD          R_MOD_CTRL
#define SIDEBAR_MODS (R_MOD_CTRL | R_MOD_SHIFT)
#endif

#define SIDEBAR_W 280.0f /* at text scale 1; it grows with the square root of the scale */
#define CARD_W    460.0f

static struct {
    Rid pages, main, lights, show, title_slot, main_slot;
    Rid sidebar, sidebar_slot, card, card_slot, toast;
    Rid menu[SHELL_MENU_COUNT];
    ShellPage page;
    /* the sidebar: t is 1 when it is in, 0 when it is away; a spring
     * pulls t to `shown` while it slides */
    int shown, sliding;
    float t, v, width, scale;
    float placed_t, placed_w; /* what place_sidebar set last */
} g;

/* ---- the sidebar's slide ---------------------------------------------------- */

/* A damped spring (Raven's motion, exact solution: steady at any frame
 * rate): a little past its target before it settles. */
static void spring_step(float *x, float *v, float target, float dt)
{
    const float response = 0.42f, zeta = 0.82f;
    float omega = 6.2831853f / response, d = *x - target, decay = expf(-zeta * omega * dt);
    float wd = omega * sqrtf(1.0f - zeta * zeta), c = cosf(wd * dt), s = sinf(wd * dt);
    float b = (*v + zeta * omega * d) / wd;
    *x = target + decay * (d * c + b * s);
    *v = decay * ((b * wd - zeta * omega * d) * c - (d * wd + zeta * omega * b) * s);
}

/* The sidebar at t: its left edge goes from the margin to past the window's
 * left edge (its shadow too), and the main area's left edge follows. */
static void place_sidebar(void)
{
    float t = g.t, w = g.width;
    if (t == g.placed_t && w == g.placed_w)
        return;
    g.placed_t = t;
    g.placed_w = w;
    float away = w + COMP_MARGIN + 64.0f, in = fminf(fmaxf(t, 0.0f), 1.0f);
    Rwidget_set_size(g.sidebar, w, -1.0f);
    Rslot_set_padding(g.sidebar, COMP_MARGIN - away * (1.0f - t), COMP_MARGIN, 0.0f, COMP_MARGIN);
    Rslot_set_padding(g.main, fmaxf((COMP_MARGIN + w) * t, 0.0f), 0.0f, 0.0f, 0.0f);
    /* the traffic lights are over the sidebar, or over the title band (whose
     * margin and gap come before the show button) */
    Rwidget_set_size(g.lights, fmaxf(LIGHTS_W - COMP_MARGIN - COMP_GAP, 0.0f) * (1.0f - in), -1.0f);
}

void shell_set_sidebar(int shown)
{
    shown = shown != 0;
    if (shown == g.shown)
        return;
    g.shown = shown;
    shell_menu_set_text(SHELL_MENU_SIDEBAR, shown ? "Hide Sidebar" : "Show Sidebar");
    comp_set_visible(g.show, !shown);
    Rwidget_set_visibility(g.sidebar, R_VISIBLE);
    if (!Rget_animations_enabled()) {
        g.t = (float)shown;
        g.v = 0.0f;
        place_sidebar();
        if (!shown)
            Rwidget_set_visibility(g.sidebar, R_HIDDEN);
        return;
    }
    if (!g.sliding) {
        g.sliding = 1;
        window_animating(+1);
    }
}

int shell_sidebar_shown(void)
{
    return g.shown;
}

static void slide(double dt)
{
    /* the first frame after an idle spell reports the whole spell; the
     * speed of every animation (Rset_animation_speed) applies here too */
    float step = (float)(dt > 1.0 / 30.0 ? 1.0 / 30.0 : dt) * Rget_animation_speed();
    float target = (float)g.shown;
    spring_step(&g.t, &g.v, target, step);
    if (fabsf(g.t - target) < 0.001f && fabsf(g.v) < 0.01f) {
        g.t = target;
        g.v = 0.0f;
        g.sliding = 0;
        window_animating(-1);
        /* away, it would only be drawn outside the window */
        if (!g.shown)
            Rwidget_set_visibility(g.sidebar, R_HIDDEN);
    }
    place_sidebar();
}

static void on_toggle_sidebar(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    (void)ud;
    shell_set_sidebar(!g.shown);
}

static void on_show_clicked(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    shell_set_sidebar(1);
}

static void on_hide_clicked(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    shell_set_sidebar(0);
}

/* ---- building ------------------------------------------------------------------ */

static int on_title_event(Rid widget, const REvent *event, void *ud)
{
    (void)widget;
    (void)ud;
    return window_title_event(event);
}

/* A band across the top of a page that moves the window. */
static Rid title_band(Rid parent)
{
    Rid band = comp_hbox(parent, COMP_GAP);
    Rwidget_set_size(band, -1.0f, TITLE_H);
    Rwidget_set_event_callback(band, on_title_event, NULL);
    return band;
}

static void build_player_page(Rid pages)
{
    Rid page = Rwidget_add_child(pages, Rwidget_create(R_WIDGET_OVERLAY));

    g.main = comp_vbox(page, 0.0f);
    Rid band = title_band(g.main);
    Rslot_set_padding(band, COMP_MARGIN, 0.0f, COMP_MARGIN, 0.0f);
    g.lights = Rwidget_add_child(band, Rwidget_create(R_WIDGET_SPACER));
    g.show = comp_icon_button(band, ICON_SIDEBAR, ICON_BUTTON_MEDIUM, "Show Sidebar", on_show_clicked, NULL);
    Rslot_set_valign(g.show, R_VALIGN_CENTER);
    Rwidget_set_transition(g.show, R_TRANSITION_FADE, 0.0f);
    comp_set_visible(g.show, 0);
    comp_fill(band);
    g.title_slot = comp_hbox(band, COMP_GAP);
    Rslot_set_valign(g.title_slot, R_VALIGN_CENTER);
    g.main_slot = comp_vbox(g.main, COMP_GAP);
    Rslot_set_fill(g.main_slot, 1.0f);

    Rid panel = comp_panel(page, PANEL_FROSTED, COMP_RADIUS, 0.0f);
    g.sidebar = Rwidget_get_parent(panel);
    Rslot_set_halign(g.sidebar, R_HALIGN_LEFT);
    Rborder_set_padding(g.sidebar, 12.0f, 0.0f, 12.0f, 12.0f);
    /* the top row lines up with the title band: the traffic lights sit on
     * its left, the hide button on its right */
    Rid top = comp_hbox(panel, COMP_GAP);
    Rwidget_set_size(top, -1.0f, TITLE_H - 2.0f * COMP_MARGIN);
    Rwidget_set_event_callback(top, on_title_event, NULL);
    comp_fill(top);
    Rid hide = comp_icon_button(top, ICON_SIDEBAR, ICON_BUTTON_MEDIUM, "Hide Sidebar", on_hide_clicked, NULL);
    Rslot_set_valign(hide, R_VALIGN_CENTER);
    g.sidebar_slot = comp_vbox(panel, COMP_GAP);
    Rslot_set_fill(g.sidebar_slot, 1.0f);
}

static void build_card_page(Rid pages)
{
    Rid page = Rwidget_add_child(pages, Rwidget_create(R_WIDGET_OVERLAY));
    Rid band = title_band(page);
    Rslot_set_valign(band, R_VALIGN_TOP);
    Rid panel = comp_panel(page, PANEL_FROSTED, 26.0f, 28.0f);
    g.card = Rwidget_get_parent(panel);
    Rslot_set_halign(g.card, R_HALIGN_CENTER);
    Rslot_set_valign(g.card, R_VALIGN_CENTER);
    Rslot_set_padding(g.card, COMP_MARGIN, TITLE_H, COMP_MARGIN, TITLE_H);
    g.card_slot = comp_scroll(panel, 12.0f);
}

static Rid menu_item(Rid menu, const char *text, RKey key, Ruint mods, const char *action)
{
    Rid item = Rmenu_add_item(menu, text);
    if (key != R_KEY_UNKNOWN)
        Rmenu_item_set_shortcut(item, key, mods);
    Rwidget_set_action(item, action);
    return item;
}

static void build_menus(Rid root)
{
    Rid bar = Rwidget_add_child(root, Rwidget_create(R_WIDGET_MENU_BAR));
#ifndef __APPLE__
    /* macOS has these in its application menu (mac_menu_sync) */
    Rid app = Rmenu_add_menu(bar, "Smalltify Player");
    menu_item(app, "Settings\xE2\x80\xA6", R_KEY_COMMA, MOD, ACT_SETTINGS);
    Rmenu_add_separator(app);
    menu_item(app, "Quit", R_KEY_Q, MOD, "app.quit");
#endif
    Rid edit = Rmenu_add_menu(bar, "Edit");
    g.menu[SHELL_MENU_FIND] = menu_item(edit, "Find", R_KEY_F, MOD, ACT_FIND);
    Rid view = Rmenu_add_menu(bar, "View");
    g.menu[SHELL_MENU_SIDEBAR] = menu_item(view, "Hide Sidebar", R_KEY_S, SIDEBAR_MODS, ACT_SIDEBAR);
    Rid playback = Rmenu_add_menu(bar, "Playback");
    /* a plain key: Raven fires it unless a text field has the keyboard */
    g.menu[SHELL_MENU_PLAY_PAUSE] = menu_item(playback, "Play", R_KEY_SPACE, 0, ACT_PLAY_PAUSE);
    g.menu[SHELL_MENU_PREVIOUS] = menu_item(playback, "Previous", R_KEY_LEFT, MOD, ACT_PREVIOUS);
    g.menu[SHELL_MENU_NEXT] = menu_item(playback, "Next", R_KEY_RIGHT, MOD, ACT_NEXT);
    Rmenu_add_separator(playback);
    g.menu[SHELL_MENU_LIKE] = menu_item(playback, "Like", R_KEY_UNKNOWN, 0, ACT_LIKE);
    Rmenu_item_set_checkable(g.menu[SHELL_MENU_LIKE], R_TRUE);
}

void shell_build(void)
{
    memset(&g, 0, sizeof g);
    Rid root = comp_vbox(R_INVALID_ID, 0.0f);
    build_menus(root);
    Rid overlay = Rwidget_add_child(root, Rwidget_create(R_WIDGET_OVERLAY));
    Rslot_set_fill(overlay, 1.0f);
    g.pages = Rwidget_add_child(overlay, Rwidget_create(R_WIDGET_SWITCHER));
    build_player_page(g.pages);
    build_card_page(g.pages);
    g.toast = Rwidget_add_child(overlay, Rwidget_create(R_WIDGET_TOAST));
    Rslot_set_halign(g.toast, R_HALIGN_CENTER);
    Rslot_set_valign(g.toast, R_VALIGN_BOTTOM);
    Rslot_set_padding(g.toast, 0.0f, 0.0f, 0.0f, 28.0f);
    Rwindow_set_content(window_id(), root);
    Raction_bind(ACT_SIDEBAR, on_toggle_sidebar, NULL);

    g.page = SHELL_PLAYER;
    Rswitcher_set_index(g.pages, SHELL_PLAYER);
    g.shown = 1;
    g.t = 1.0f;
    g.placed_t = -1.0f;
    shell_frame(0.0); /* sizes for the text scale, and the sidebar's place */
}

/* ---- pages, toast, menus ------------------------------------------------------- */

void shell_show(ShellPage page)
{
    if (page == g.page)
        return;
    g.page = page;
    Rswitcher_set_index(g.pages, (int)page);
    for (int i = 0; i < SHELL_MENU_COUNT; i++)
        shell_menu_set_enabled((ShellMenu)i, page == SHELL_PLAYER);
}

ShellPage shell_page(void)
{
    return g.page;
}

Rid shell_sidebar_slot(void)
{
    return g.sidebar_slot;
}

Rid shell_main_slot(void)
{
    return g.main_slot;
}

Rid shell_title_slot(void)
{
    return g.title_slot;
}

Rid shell_card_slot(void)
{
    return g.card_slot;
}

void shell_toast(const char *text, int error)
{
    if (text && text[0])
        Rtoast_show_kind(g.toast, text, error ? R_TOAST_ERROR : R_TOAST_INFO, 2.5);
}

/* A change to the menus makes Raven build the system's menu bar again in
 * this frame; the next frame completes the application menu (macOS), so
 * one must come. */
static void menu_changed(void)
{
    Rwake_up();
}

void shell_menu_set_text(ShellMenu item, const char *text)
{
    if (strcmp(Rmenu_item_get_text(g.menu[item]), text) == 0)
        return;
    Rmenu_item_set_text(g.menu[item], text);
    menu_changed();
}

void shell_menu_set_checked(ShellMenu item, int checked)
{
    if (!Rmenu_item_is_checked(g.menu[item]) == !checked)
        return;
    Rmenu_item_set_checked(g.menu[item], checked);
    menu_changed();
}

void shell_menu_set_enabled(ShellMenu item, int enabled)
{
    if (!Rwidget_is_enabled(g.menu[item]) == !enabled)
        return;
    Rwidget_set_enabled(g.menu[item], enabled);
    menu_changed();
}

/* ---- frame ------------------------------------------------------------------------ */

void shell_frame(double dt)
{
    float scale = Raccessibility_get_text_scale();
    if (scale != g.scale) {
        /* larger text: a wider sidebar and card */
        g.scale = scale;
        g.width = roundf(SIDEBAR_W * sqrtf(scale));
        Rwidget_set_size(g.card, roundf(CARD_W * sqrtf(scale)), -1.0f);
        place_sidebar();
    }
    if (g.sliding)
        slide(dt);
#ifdef __APPLE__
    mac_menu_sync(ACT_SETTINGS);
#endif
}
