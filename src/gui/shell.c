/*
 * shell.c: the frame of every screen (shell.h): pages, sidebar and its
 * slide, title bands, card, toast and menus.
 *
 *   root VBox
 *     MenuBar                    (the system's on macOS: takes no room)
 *     Overlay
 *       Switcher                 pages
 *         Overlay                SHELL_PLAYER
 *           Border (tint)        title bar, across the window: traffic lights, title slot
 *           VBox                 main area under it, left padding = room the sidebar takes
 *             VBox               main slot
 *           Button               show sidebar: floats under the title bar while the sidebar is away
 *           Border (glass)       sidebar, floating under the title bar; slides by its left padding
 *             VBox: HBox (tabs slot, hide button), VBox (sidebar slot)
 *         Overlay                SHELL_CARD
 *           Border (tint)        title bar
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
#define MOD          R_MOD_SUPER
#define SIDEBAR_MODS (R_MOD_SUPER | R_MOD_CTRL)
#else
#define TITLE_H      28.0f
#define MOD          R_MOD_CTRL
#define SIDEBAR_MODS (R_MOD_CTRL | R_MOD_SHIFT)
#endif

/* at text scale 1, it grows with the square root of the scale: the
 * Library / Devices tabs and the hide button beside them */
#define SIDEBAR_W    322.0f
#define CARD_W       460.0f
/* the title bar: a little darker than the wallpaper, like Notes' toolbar */
#define TITLE_TINT 0.0f, 0.0f, 0.0f, 0.16f

static struct {
    Rid pages, main, show, title_slot, main_slot;
    Rid sidebar, tabs_slot, sidebar_slot, card, card_slot, toast;
    Rid menu[SHELL_MENU_COUNT];
    ShellPage page;
    /* the sidebar: t is 1 when it is in, 0 when it is away; a spring
     * pulls t to `shown` while it slides, and the window's width with it:
     * base + grow * t (grow 0: the window keeps its size, maximized) */
    int shown, sliding;
    float t, v, width, scale;
    float base, grow;
    int sized_w;              /* what size_window set last */
    float placed_t, placed_w; /* what place_sidebar set last */
    float main_min_w, main_min_h; /* the player's smallest room (shell_set_main_min) */
    int min_w, min_h;             /* what update_min set last */
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

/* The sidebar at t: under the title bar, its left edge goes from the
 * margin to past the window's left edge (its shadow too), and the main
 * area's left edge follows. */
static void place_sidebar(void)
{
    float t = g.t, w = g.width;
    if (t == g.placed_t && w == g.placed_w)
        return;
    g.placed_t = t;
    g.placed_w = w;
    float away = w + COMP_MARGIN + 64.0f;
    Rwidget_set_size(g.sidebar, w, -1.0f);
    Rslot_set_padding(g.sidebar, COMP_MARGIN - away * (1.0f - t), TITLE_H + COMP_MARGIN, 0.0f, COMP_MARGIN);
    Rslot_set_padding(g.main, fmaxf((COMP_MARGIN + w) * t, 0.0f), TITLE_H, 0.0f, 0.0f);
}

/* Room the sidebar takes beside the main area. */
static float sidebar_room(void)
{
    return g.width + COMP_MARGIN;
}

/* The window's minimum: the player's room, plus the sidebar once it is in
 * (not while it slides: the window is still growing to it); on the card
 * page, the card. */
static void update_min(void)
{
    int w, h;
    if (g.page == SHELL_PLAYER) {
        w = (int)ceilf(g.main_min_w + (g.shown && !g.sliding ? sidebar_room() : 0.0f));
        h = (int)ceilf(g.main_min_h + TITLE_H);
    } else {
        w = (int)ceilf(roundf(CARD_W * sqrtf(g.scale)) + 2.0f * COMP_MARGIN);
        h = (int)ceilf(2.0f * TITLE_H + 240.0f);
    }
    if (w == g.min_w && h == g.min_h)
        return;
    g.min_w = w;
    g.min_h = h;
    window_set_min_size(w, h);
}

void shell_set_main_min(float w, float h)
{
    if (w == g.main_min_w && h == g.main_min_h)
        return;
    g.main_min_w = w;
    g.main_min_h = h;
    update_min();
}

/* Where the window's width goes as the sidebar slides, from the width it
 * has now: it grows by the sidebar's room, or shrinks by it down to the
 * player's minimum, so the player keeps its room and its width (not when
 * the OS sized the window: maximized). */
static void plan_window(int shown)
{
    Rid win = window_id();
    int w = 0, h = 0;
    Rwindow_get_size(win, &w, &h);
    g.sized_w = w;
    if (Rwindow_is_maximized(win)) {
        g.base = (float)w;
        g.grow = 0.0f;
    } else if (shown) {
        g.base = (float)w;
        g.grow = ceilf(sidebar_room());
    } else {
        g.base = fmaxf((float)w - ceilf(sidebar_room()), ceilf(g.main_min_w));
        g.grow = fmaxf((float)w - g.base, 0.0f);
    }
}

/* The window's width at t, set once per change. */
static void size_window(void)
{
    if (g.grow <= 0.0f)
        return;
    int to = (int)lroundf(g.base + g.grow * g.t);
    if (to == g.sized_w)
        return;
    g.sized_w = to;
    Rid win = window_id();
    int w = 0, h = 0;
    Rwindow_get_size(win, &w, &h);
    Rwindow_set_size(win, to, h);
}

void shell_set_sidebar(int shown)
{
    shown = shown != 0;
    if (shown == g.shown)
        return;
    g.shown = shown;
    shell_menu_set_text(SHELL_MENU_SIDEBAR, shown ? "Hide Sidebar" : "Show Sidebar");
    /* turned back while it slides: the window goes back the way it came */
    if (!g.sliding)
        plan_window(shown);
    comp_set_visible(g.show, !shown);
    Rwidget_set_visibility(g.sidebar, R_VISIBLE);
    if (!Rget_animations_enabled()) {
        g.t = (float)shown;
        g.v = 0.0f;
        size_window(); /* growing: the new size before the larger minimum */
        update_min();
        place_sidebar();
        if (!shown)
            Rwidget_set_visibility(g.sidebar, R_HIDDEN);
        return;
    }
    if (!g.sliding) {
        g.sliding = 1;
        window_animating(+1);
    }
    update_min(); /* the smaller one while it slides */
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
    size_window();
    if (!g.sliding)
        update_min(); /* in: the minimum takes the sidebar's room again */
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

/* The show / hide button: a medium icon button with a larger icon. */
static Rid sidebar_button(Rid parent, const char *tip, RButtonClickedCallback cb)
{
    Rid button = comp_icon_button(parent, ICON_SIDEBAR, ICON_BUTTON_MEDIUM, tip, cb, NULL);
    Rwidget_set_size(Rwidget_get_child(button, 0), 25.0f, 25.0f);
    return button;
}

static int on_title_event(Rid widget, const REvent *event, void *ud)
{
    (void)widget;
    (void)ud;
    return window_title_event(event);
}

/* A muted bar across the top of a page that moves the window. Returns its
 * HBox; the bar is its parent. */
static Rid title_band(Rid parent)
{
    Rid bar = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_BORDER));
    /* flat: the look would make a Border a rounded pane of glass */
    Rborder_set_material(bar, R_MATERIAL_SOLID);
    Rborder_set_corner_radius(bar, 0.0f, 0.0f, 0.0f, 0.0f);
    Rborder_set_background(bar, TITLE_TINT);
    Rwidget_set_size(bar, -1.0f, TITLE_H);
    Rid band = comp_hbox(bar, COMP_GAP);
    Rwidget_set_event_callback(band, on_title_event, NULL);
    return band;
}

static void build_player_page(Rid pages)
{
    Rid page = Rwidget_add_child(pages, Rwidget_create(R_WIDGET_OVERLAY));

    Rid band = title_band(page);
    Rslot_set_valign(Rwidget_get_parent(band), R_VALIGN_TOP);
    Rslot_set_padding(band, COMP_MARGIN, 0.0f, COMP_MARGIN, 0.0f);
    comp_fill(band);
    g.title_slot = comp_hbox(band, COMP_GAP);
    Rslot_set_valign(g.title_slot, R_VALIGN_CENTER);
    g.main = comp_vbox(page, 0.0f);
    g.main_slot = comp_vbox(g.main, COMP_GAP);
    Rslot_set_fill(g.main_slot, 1.0f);

    /* under the title bar at the window's left, while the sidebar is away;
     * the sidebar slides in over it */
    g.show = sidebar_button(page, "Show Sidebar", on_show_clicked);
    Rslot_set_halign(g.show, R_HALIGN_LEFT);
    Rslot_set_valign(g.show, R_VALIGN_TOP);
    Rslot_set_padding(g.show, 12.0f, TITLE_H + COMP_GAP, 0.0f, 0.0f);
    Rwidget_set_transition(g.show, R_TRANSITION_FADE, 0.0f);
    comp_set_visible(g.show, 0);

    Rid panel = comp_panel(page, PANEL_FROSTED, COMP_RADIUS, 12.0f);
    g.sidebar = Rwidget_get_parent(panel);
    Rslot_set_halign(g.sidebar, R_HALIGN_LEFT);
    /* the top row: the tabs (tabs slot), then the hide button */
    Rid top = comp_hbox(panel, COMP_GAP);
    g.tabs_slot = comp_vbox(top, 0.0f);
    Rslot_set_fill(g.tabs_slot, 1.0f);
    Rslot_set_valign(g.tabs_slot, R_VALIGN_CENTER);
    Rid hide = sidebar_button(top, "Hide Sidebar", on_hide_clicked);
    Rslot_set_valign(hide, R_VALIGN_CENTER);
    g.sidebar_slot = comp_vbox(panel, COMP_GAP);
    Rslot_set_fill(g.sidebar_slot, 1.0f);
}

static void build_card_page(Rid pages)
{
    Rid page = Rwidget_add_child(pages, Rwidget_create(R_WIDGET_OVERLAY));
    Rid band = title_band(page);
    Rslot_set_valign(Rwidget_get_parent(band), R_VALIGN_TOP);
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
    update_min();
    for (int i = 0; i < SHELL_MENU_COUNT; i++)
        shell_menu_set_enabled((ShellMenu)i, page == SHELL_PLAYER);
}

ShellPage shell_page(void)
{
    return g.page;
}

Rid shell_sidebar_tabs_slot(void)
{
    return g.tabs_slot;
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
        update_min();
    }
    if (g.sliding)
        slide(dt);
#ifdef __APPLE__
    mac_menu_sync(ACT_SETTINGS);
#endif
}
