/*
 * window.h: the window shell. Owns the Raven lifecycle (init, one window,
 * one root custom widget that paints through gui.h), the event loop and the
 * redraw/wake plumbing. Knows nothing about screens: ui.c passes a draw
 * function.
 *
 * Threading: window_open, window_run, window_set_size, window_set_min_size,
 * window_is_minimized and window_redraw run on the UI
 * thread. window_wake
 * and window_quit are thread-safe (worker threads and signal-driven quits
 * call them).
 */
#ifndef SMP_GUI_WINDOW_H
#define SMP_GUI_WINDOW_H

#include "gui.h"

/* Paints one frame. Called on the UI thread inside the paint bracket
 * (gui_begin_paint / gui_end_paint, done by the window). w, h are the
 * window size in logical units. */
typedef void (*WindowDrawFn)(void* user, float w, float h);

/* The user closed the window, or window_quit was called. Runs once, on the
 * UI thread, before the loop returns. */
typedef void (*WindowCloseFn)(void* user);

/* Creates the Raven runtime and one window, loads the GUI font from the
 * compiled-in ui_font_ttf (ui_font.c) and sets the root widget to draw.
 * w, h, min_w, min_h are logical units. Returns 0 on success. */
int window_open(const char* title, int w, int h, int min_w, int min_h,
    WindowDrawFn draw, void* user);

/* Runs the event loop until the window closes or window_quit is called,
 * then calls on_close, releases the font and shuts Raven down. Returns the
 * exit code (0 for a normal close). */
int window_run(WindowCloseFn on_close, void* close_user);

/* Resizes the window to w x h logical units (UI thread). */
void window_set_size(int w, int h);

/* Sets the smallest size the user can resize the window to, in logical
 * units (UI thread). Raven: Rwindow_set_min_size. */
void window_set_min_size(int w, int h);

/* 1 while the window is minimized (UI thread). */
int window_is_minimized(void);

/* Marks the window for a repaint on the next frame (UI thread). */
void window_redraw(void);

/* Wakes the loop so it repaints or handles its queue (thread-safe). */
void window_wake(void);

/* Asks the loop to return (thread-safe). The close callback still runs. */
void window_quit(void);

#endif /* SMP_GUI_WINDOW_H */
