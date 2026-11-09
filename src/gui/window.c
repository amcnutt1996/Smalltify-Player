/*
 * window.c: the window shell on Raven, in place of the old ui_run window
 * setup and frame loop: one resizable window, one root custom
 * widget that paints through gui.h, and an event-waiting loop that
 * background threads wake.
 */
#include "window.h"

#include <stdio.h>
#include <string.h>

/* The GUI font, third_party/inter/Inter-Regular.subset.ttf, compiled in by
 * CMakeLists.txt (ui_font.c in the build folder). */
extern const unsigned char ui_font_ttf[];
extern const unsigned int ui_font_ttf_len;

static Rid g_window = R_INVALID_ID;
static Rid g_root = R_INVALID_ID; /* the custom widget the screens draw in */
static WindowDrawFn g_draw;
static void *g_draw_user;

/* Raven only records warnings and errors unless it has a callback, so this
 * one reports them; the old build's log level was WARNING. Runs on any
 * thread, the render thread included. */
static void log_error(RError code, RSeverity severity, const char *detail)
{
    if (severity < R_SEVERITY_WARNING)
        return;
    fprintf(stderr, "raven: %s (%s): %s\n", Rerror_to_string(code),
            Rseverity_to_string(severity), detail ? detail : "");
}

/* One frame of the old loop: the kit's paint bracket around the screen.
 * The root widget fills the window, so its size is the window's. */
static void paint(Rid widget, RPainter *painter, float width, float height, void *user)
{
    (void)widget;
    (void)user;
    gui_begin_paint(painter, width, height);
    g_draw(g_draw_user, width, height);
    gui_end_paint();
}

/* The kit reads input while it paints, so an event it used needs a frame;
 * one it did not use changes nothing on screen. */
static int feed(const REvent *event)
{
    if (!gui_event(event))
        return R_FALSE;
    Rwidget_invalidate(g_root);
    return R_TRUE;
}

/* Input comes through the window hook, ahead of any widget. The root
 * widget's own callback would get keys only while it held the keyboard
 * focus, and the pointer only over the window: Raven gives custom widgets
 * no mouse capture, so a release outside the window would be lost. The old
 * build's window saw both. */
static int on_input(Rid window, const REvent *event, void *user)
{
    (void)window;
    (void)user;
    return feed(event);
}

static int on_root_event(Rid widget, const REvent *event, void *user)
{
    (void)widget;
    (void)event;
    (void)user;
    return R_FALSE;
}

/* Closing ends the loop like window_quit. The window stays until
 * Rshutdown, as the old build's window stayed until after the cleanup,
 * so on_close can still use it. */
static int on_close_request(Rid window, void *user)
{
    (void)window;
    (void)user;
    Rquit(0);
    return R_FALSE;
}

int window_open(const char *title, int w, int h, int min_w, int min_h,
                WindowDrawFn draw, void *user)
{
    RInitDesc desc;
    GuiColor bg = GUI_BG;

    g_draw = draw;
    g_draw_user = user;
    Rerror_set_callback(log_error); /* before Rinit, so a failed start is reported */

    /* The defaults keep vsync on (FLAG_VSYNC_HINT) and always apply the
     * monitor scale to logical sizes (FLAG_WINDOW_HIGHDPI). */
    memset(&desc, 0, sizeof desc);
    desc.backend = R_BACKEND_VULKAN;
    if (!Rinit_ex(&desc))
        return -1;

    /* R_WINDOW_DEFAULT is resizable (FLAG_WINDOW_RESIZABLE). No key closes
     * a Raven window, so there is no exit key to turn off (SetExitKey). */
    g_window = Rwindow_create(title, w, h, R_WINDOW_DEFAULT);
    if (g_window == R_INVALID_ID)
        goto fail;
    Rwindow_set_min_size(g_window, min_w, min_h);
    /* ClearBackground(C_BG) of the old frame; it also fills a resized
     * window until its new frame. */
    Rwindow_set_clear_color(g_window, bg.r, bg.g, bg.b, bg.a);
    Rwindow_set_close_callback(g_window, on_close_request, NULL);
    Rwindow_set_event_hook(g_window, on_input, NULL);

    if (gui_font_init(ui_font_ttf, ui_font_ttf_len) != 0)
        goto fail;
    g_root = Rwidget_create(R_WIDGET_CUSTOM);
    if (g_root == R_INVALID_ID) {
        gui_font_shutdown();
        goto fail;
    }
    Rcustom_set_paint_callback(g_root, paint, NULL);
    Rwidget_set_event_callback(g_root, on_root_event, NULL);
    Rwindow_set_content(g_window, g_root);
    return 0;

fail:
    Rshutdown();
    g_window = g_root = R_INVALID_ID;
    return -1;
}

int window_run(WindowCloseFn on_close, void *close_user)
{
    /* Event waiting, as EnableEventWaiting was: Rwait_frame sleeps in the
     * OS event wait until input or a wake, and draws only a
     * window that asked for a frame. */
    while (Rwait_frame(-1.0)) {}
    if (on_close)
        on_close(close_user);
    gui_font_shutdown();
    int rc = Rget_exit_code(); /* Rshutdown clears it */
    Rshutdown();
    g_window = g_root = R_INVALID_ID;
    return rc;
}

void window_set_size(int w, int h)
{
    Rwindow_set_size(g_window, w, h);
}

void window_set_min_size(int w, int h)
{
    Rwindow_set_min_size(g_window, w, h);
}

int window_is_minimized(void)
{
    return Rwindow_is_minimized(g_window);
}

void window_redraw(void)
{
    Rwidget_invalidate(g_root);
}

/* Every turn of the old loop drew a frame, and the screens pick up the
 * workers' results while they draw, so a wake must bring a frame; a bare
 * Rwake_up would only turn the loop. Off the UI thread this sets an atomic
 * flag and makes the same wake as Rwake_up. */
void window_wake(void)
{
    Rwindow_request_redraw(g_window);
}

void window_quit(void)
{
    Rquit(0);
}
