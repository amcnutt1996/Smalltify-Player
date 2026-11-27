/*
 * window.h: the window shell. Owns the Raven lifecycle (init, one window,
 * shutdown), the event loop and the wake plumbing. Knows nothing about
 * screens: ui.c passes a frame function that runs once per loop iteration.
 *
 * Threading: everything here runs on the UI thread (the one that called
 * window_open), except window_wake and window_quit, which any thread may
 * call (workers, the signal-driven quit).
 */
#ifndef SMP_GUI_WINDOW_H
#define SMP_GUI_WINDOW_H

#include <Raven.h>

/* Runs once per loop iteration, before layout and paint: after every wake
 * (window_wake), input event, timer and animation frame. dt is the time
 * since the previous call, in seconds. */
typedef void (*WindowFrameFn)(void *user, double dt);

/* The user closed the window, or window_quit was called. Runs once, on the
 * UI thread, before the loop returns. */
typedef void (*WindowCloseFn)(void *user);

/* Starts Raven (Vulkan), applies the theme (theme_init) and creates the
 * window, hidden until window_run: w x h and the minimum size in logical
 * units. On macOS the title bar is transparent over the content, with the
 * system's traffic lights inline (mac_window_setup). Returns 0 on success. */
int window_open(const char *title, int w, int h, int min_w, int min_h);

/* The window, for Raven calls that take one (Rdialog_open...). */
Rid window_id(void);

/* Shows the window and runs the loop until it closes or window_quit is
 * called, then calls on_close and shuts Raven down. The loop sleeps in the
 * OS until something happens: an idle window costs no CPU. Returns the exit
 * code (0 for a normal close). */
int window_run(WindowFrameFn frame, WindowCloseFn on_close, void *user);

/* Smallest size the user can resize the window to, in logical units. */
void window_set_min_size(int w, int h);

/* 1 while the window is minimized. */
int window_is_minimized(void);

/* Wakes the loop so the frame function runs soon (thread-safe; Rwake_up). */
void window_wake(void);

/* Asks the loop to return (thread-safe). The close function still runs. */
void window_quit(void);

/* For the event callback (Rwidget_set_event_callback) of a widget that
 * stands for the title bar: a left press moves the window, a double click
 * zooms it. Returns R_TRUE when it used the event. Events the widget's
 * children handle (buttons) never get here. */
int window_title_event(const REvent *event);

/* Continuous frames while an animation runs: +1 when one starts, -1 when it
 * settles. Frames stop again when the count is back to 0. */
void window_animating(int delta);

#endif /* SMP_GUI_WINDOW_H */
