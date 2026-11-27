/*
 * window.c: the window shell on Raven: one window and an event-waiting loop
 * that background threads wake. The widgets themselves are retained: the
 * frame function pushes state changes into them, and Raven lays out and
 * paints only what changed.
 */
#include "window.h"

#include <stdio.h>
#include <string.h>

#include "macos.h"
#include "theme.h"

static Rid g_window = R_INVALID_ID;
static WindowFrameFn g_frame;
static void *g_frame_user;
static int g_animating; /* animations running: frames keep coming */
static Rid g_parked = R_INVALID_ID; /* the field that had the keyboard when the window went to the background */

/* Raven only records warnings and errors unless it has a callback, so this
 * one reports them, from WARNING up. Runs on any thread, the render
 * thread included. */
static void log_error(RError code, RSeverity severity, const char *detail)
{
    if (severity < R_SEVERITY_WARNING)
        return;
    fprintf(stderr, "raven: %s (%s): %s\n", Rerror_to_string(code), Rseverity_to_string(severity),
            detail ? detail : "");
}

/* Closing ends the loop like window_quit. The window stays until
 * Rshutdown, after the cleanup, so the close function can still use it. */
static int on_close_request(Rid window, void *user)
{
    (void)window;
    (void)user;
    Rquit(0);
    return R_FALSE;
}

/* The window is the one the user works in: Raven's flag, and on macOS the
 * application's too (see mac_app_active). */
static int window_in_front(void)
{
#ifdef __APPLE__
    if (!mac_app_active())
        return 0;
#endif
    return Rwindow_is_active(g_window);
}

/* A focused field's caret blinks: a repaint twice a second. While the
 * window is in the background no field keeps the keyboard (disabling the
 * field is Raven's only way to drop focus; it is enabled again at once),
 * and the field gets it back when the window does, unless something else
 * took it meanwhile. Screens that focus a field while in the background
 * (sign-in at launch) have it parked here the same pass. */
static void park_text_focus(void)
{
    if (!window_in_front()) {
        if (Rwindow_wants_text_input(g_window)) {
            Rid field = Rwindow_get_focused_widget(g_window);
            Rwidget_set_enabled(field, R_FALSE);
            Rwidget_set_enabled(field, R_TRUE);
            g_parked = field;
        }
    } else if (g_parked != R_INVALID_ID) {
        Rid field = g_parked;
        g_parked = R_INVALID_ID;
        if (Rwindow_get_focused_widget(g_window) == R_INVALID_ID && Rwidget_is_valid(field))
            Rwidget_focus(field);
    }
}

/* Raven calls this once per loop iteration, before its layout and paint. */
static void on_frame(double dt, void *user)
{
    (void)user;
    if (g_frame)
        g_frame(g_frame_user, dt);
    park_text_focus();
}

int window_open(const char *title, int w, int h, int min_w, int min_h)
{
    RInitDesc desc;

    Rerror_set_callback(log_error); /* before Rinit, so a failed start is reported */
    /* The defaults keep vsync on and always apply the monitor scale to
     * logical sizes. */
    memset(&desc, 0, sizeof desc);
    desc.backend = R_BACKEND_VULKAN;
    desc.app_name = title;
    if (!Rinit_ex(&desc))
        return -1;
    /* Every widget is built from C: no interface files to look for or
     * watch. */
    Rxml_set_auto_load(R_FALSE);
    Rxml_set_live_reload(R_FALSE);
    theme_init();

    /* Hidden until window_run, so the content and (on macOS) the title bar
     * are in place when it first shows. R_WINDOW_DEFAULT is resizable. */
    g_window = Rwindow_create(title, w, h, R_WINDOW_HIDDEN);
    if (g_window == R_INVALID_ID) {
        Rshutdown();
        return -1;
    }
    Rwindow_set_min_size(g_window, min_w, min_h);
    Rwindow_set_close_callback(g_window, on_close_request, NULL);
#ifdef __APPLE__
    mac_window_setup(g_window);
#endif
    theme_set_art(NULL, 0); /* the wallpaper before any art */
    return 0;
}

Rid window_id(void)
{
    return g_window;
}

int window_run(WindowFrameFn frame, WindowCloseFn on_close, void *user)
{
    g_frame = frame;
    g_frame_user = user;
    Rset_frame_callback(on_frame, NULL);
    Rwindow_center(g_window);
    Rwindow_show(g_window);
    /* Event waiting: Rwait_frame sleeps in the OS event wait until input,
     * a wake, a timer or an animation, and draws only a window that
     * changed. */
    while (Rwait_frame(-1.0)) {
    }
    Rset_frame_callback(NULL, NULL);
    if (on_close)
        on_close(user);
    int rc = Rget_exit_code(); /* Rshutdown clears it */
    Rshutdown();
    g_window = R_INVALID_ID;
    g_animating = 0;
    g_parked = R_INVALID_ID;
    return rc;
}

void window_set_min_size(int w, int h)
{
    Rwindow_set_min_size(g_window, w, h);
}

int window_is_minimized(void)
{
    return Rwindow_is_minimized(g_window);
}

/* The frame function runs on every loop iteration, so a bare wake is
 * enough: it pulls the workers' news and pushes what changed. */
void window_wake(void)
{
    Rwake_up();
}

void window_quit(void)
{
    Rquit(0);
}

int window_title_event(const REvent *event)
{
    if (event->type != R_EVENT_MOUSE_DOWN || event->button != R_MOUSE_LEFT)
        return R_FALSE;
    if (event->clicks == 2) {
        if (Rwindow_is_maximized(g_window))
            Rwindow_restore(g_window);
        else
            Rwindow_maximize(g_window);
    } else {
        Rwindow_begin_move(g_window);
    }
    return R_TRUE;
}

void window_animating(int delta)
{
    int was = g_animating > 0;
    g_animating += delta;
    if (g_animating < 0)
        g_animating = 0;
    if ((g_animating > 0) != was)
        Rwindow_set_continuous_redraw(g_window, g_animating > 0);
}
