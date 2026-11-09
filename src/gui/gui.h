/*
 * gui.h: the GUI kit shared by every screen and panel. Built on Raven
 * (https://github.com/oscar-soirey/Raven, Apache-2.0).
 *
 * Model: immediate mode, as the old UI was. The window has one root
 * custom widget. Its paint callback calls gui_begin_paint() and then the
 * active screen's draw function; its event callback feeds gui_event().
 * Screens lay out and hit-test with the rectangles they draw, as before.
 *
 * Units: all sizes and coordinates are logical units (Raven applies the
 * window scale). Colours are GuiColor, 0..1 per channel.
 *
 * Threading: everything here runs on the UI thread (the thread that called
 * Rinit). Worker threads only call Rtask_post / Rwake_up.
 */
#ifndef SMP_GUI_H
#define SMP_GUI_H

#include <stddef.h>

#include <Raven.h>

/* ---- geometry and colour ------------------------------------------------ */

typedef struct GuiRect {
    float x, y, w, h;
} GuiRect;

typedef struct GuiColor {
    float r, g, b, a;
} GuiColor;

/* Palette: the old C_* constants of ui.c (value / 255, alpha 1). */
#define GUI_BG     ((GuiColor){ 18 / 255.0f, 18 / 255.0f, 18 / 255.0f, 1.0f })
#define GUI_PANEL  ((GuiColor){ 40 / 255.0f, 40 / 255.0f, 40 / 255.0f, 1.0f })
#define GUI_HOVER  ((GuiColor){ 58 / 255.0f, 58 / 255.0f, 58 / 255.0f, 1.0f })
#define GUI_PRESS  ((GuiColor){ 28 / 255.0f, 28 / 255.0f, 28 / 255.0f, 1.0f })
#define GUI_TEXT   ((GuiColor){ 235 / 255.0f, 235 / 255.0f, 235 / 255.0f, 1.0f })
#define GUI_DIM    ((GuiColor){ 150 / 255.0f, 150 / 255.0f, 150 / 255.0f, 1.0f })
#define GUI_ACCENT ((GuiColor){ 30 / 255.0f, 215 / 255.0f, 96 / 255.0f, 1.0f })
#define GUI_ON     ((GuiColor){ 0 / 255.0f, 114 / 255.0f, 48 / 255.0f, 1.0f })
#define GUI_ERR    ((GuiColor){ 240 / 255.0f, 110 / 255.0f, 110 / 255.0f, 1.0f })
#define GUI_WHITE  ((GuiColor){ 1.0f, 1.0f, 1.0f, 1.0f })

/* ---- frame and input ----------------------------------------------------
 * gui_begin_paint() is called by the root widget's paint callback with the
 * painter and the window size. Input is filled by gui_event(); the edges
 * (clicked, released, keys, text, wheel) belong to one frame: gui_end_paint()
 * clears them after the paint. */

typedef struct GuiInput {
    float mx, my;          /* pointer, logical units */
    int   down;            /* left button held */
    int   clicked;         /* left button went down this frame */
    int   released;        /* left button went up this frame */
    float wheel;           /* lines scrolled this frame, + = up */
    int   key_enter;       /* Enter or keypad Enter pressed (press only, not repeat) */
    int   key_escape;      /* Escape pressed (press only, not repeat) */
    int   key_backspace;   /* Backspace pressed, including auto-repeat */
    int   key_paste;       /* Ctrl+V or Cmd+V pressed (press only, not repeat) */
    char  text[256];       /* UTF-8 typed this frame; longer input is cut */
} GuiInput;

extern GuiInput gui_input;

/* The widget holding the pointer: a W_* id from ui_state.h, 0 = none. Set
 * on a press inside a widget, cleared when the left button is released. */
extern int gui_active;

/* Paint bracket, used by the window code only. */
void gui_begin_paint(RPainter* painter, float width, float height);
void gui_end_paint(void);

/* Event handling: called from the root widget's event callback. Returns 1
 * when the event was used. */
int gui_event(const REvent* event);

/* ---- fonts and text ----------------------------------------------------
 * One Inter face (the embedded subset) at every size. Sizes are the old
 * logical font sizes (Metrics.fs / Metrics.fl in ui_state.h). */

int   gui_font_init(const unsigned char* ttf, size_t len); /* 0 = ok */
void  gui_font_shutdown(void);

float gui_text_width(float size, const char* text);
float gui_text_height(float size); /* height of one line at size */

/* Draw text with its top-left corner at (x, y). */
void gui_text(float size, float x, float y, const char* text, GuiColor c);
/* Draw text centred horizontally on cx, top at y. */
void gui_text_center(float size, float cx, float y, const char* text, GuiColor c);

/* Cut text to fit max_w, appending an ellipsis (U+2026) when cut. Writes
 * at most cap bytes including the NUL. Returns out. */
char* gui_fit_text(float size, const char* text, float max_w, char* out, size_t cap);

/* Wrap text at spaces into lines of max_w; newlines start a new line. With
 * max_lines > 0 at most that many lines: a word that cannot break becomes
 * the last line and is ellipsised, and so is the last line when text
 * remains (the old fit_two_lines). With max_lines <= 0 there is no limit and
 * an overlong word overflows (the old wrap_words). Lines are joined by '\n'.
 * Returns the number of lines written. */
int gui_wrap_text(float size, const char* text, float max_w, int max_lines,
    char* out, size_t cap);
/* Hard wrap at any character, spaces included: the old wrap_text, used for
 * the env path where spaces are part of the path. */
int gui_wrap_path(float size, const char* text, float max_w, char* out,
    size_t cap);

/* ---- shapes -------------------------------------------------------------
 * `roundness` is the old fraction (0 = square, 1 = fully round, the
 * radius being roundness * min(w, h) / 2). The kit converts it to pixels. */

void gui_fill(GuiRect r, float roundness, GuiColor c);
void gui_outline(GuiRect r, float roundness, float thickness, GuiColor c);
void gui_circle(float cx, float cy, float radius, GuiColor c);
void gui_line(float x0, float y0, float x1, float y1, float thickness, GuiColor c);
void gui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, GuiColor c);
void gui_image(Rid texture, GuiRect r, GuiColor tint);
/* Clip everything drawn until gui_clip_end() to r. Clips nest. */
void gui_clip_begin(GuiRect r);
void gui_clip_end(void);

/* Pointer (gui_input.mx, my) inside r: left and top edges inclusive, right
 * and bottom edges exclusive. */
int gui_hit(GuiRect r);

/* ---- widgets ------------------------------------------------------------
 * `id` is the W_* widget id (ui_state.h). Buttons return 1 when activated
 * this frame. The press/release semantics are the old clicked(): a press
 * inside makes the widget active; release inside while active activates
 * it. Buttons: on = 1 draws the GUI_ON fill and skips the hover tint (the
 * old button()'s `on` argument; no old button had a disabled state).
 * Sliders: enabled = 0 draws dim and ignores input. */

int gui_button(int id, GuiRect r, const char* label, float size, int on);

typedef enum GuiIcon {
    GUI_ICON_PREV,
    GUI_ICON_PLAY,
    GUI_ICON_PAUSE,
    GUI_ICON_NEXT
} GuiIcon;

/* Button with a vector transport icon drawn in its rectangle. */
int gui_icon_button(int id, GuiRect r, GuiIcon icon);
/* The icon button's face alone, for a caller that checks the click first
 * (gui_clicked) and draws the icon in the state that click leaves. */
void gui_icon_face(int id, GuiRect r, GuiIcon icon, int on);
/* The icon alone, drawn in colour c inside r. */
void gui_icon(GuiRect r, GuiIcon icon, GuiColor c);

/* Horizontal slider, value in [0, 1]. While dragging it writes the new
 * value to *value. *released is set to 1 on the frame the drag ends, so the
 * caller can commit the value (the old commit-on-release). */
void gui_slider(int id, GuiRect r, float* value, int enabled, int* released);

/* A press and release inside r for widget id, as the widgets take them:
 * returns 1 on the release. A caller that draws a widget itself checks this
 * first, changes its state on a click, then draws. */
int gui_clicked(int id, GuiRect r);
/* Heart drawn with two arcs and a triangle: solid when on, else its outline.
 * thickness is the outline width in pixels (the caller's max(1.5, fs / 8)),
 * unused when on = 1. Draws only; clicks come from gui_clicked(). */
void gui_heart(GuiRect r, int on, float thickness);

/* Editable single-line field: edits buf (cap bytes, NUL-terminated) from
 * this frame's typed text, Backspace and paste. key_chars = 1 drops spaces
 * and non-ASCII (API keys), as the old edit_line did. The caller holds
 * focus; the kit only edits. */
void gui_edit_line(char* buf, size_t cap, int key_chars);

#endif /* SMP_GUI_H */
