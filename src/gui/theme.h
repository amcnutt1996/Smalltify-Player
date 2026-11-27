/*
 * theme.h: the look of the application. Raven Glass Dark, San Francisco on
 * macOS (Inter elsewhere), the text size of the settings, and the accent
 * color and wallpaper taken from the album art playing.
 *
 * Text: every text is drawn at a size of the design (theme_text_size) that
 * Raven multiplies by the text scale of the setting
 * (Raccessibility_set_text_scale): labels, inputs, rows and the heights of
 * the controls all follow, live. Never multiply by the scale yourself.
 *
 * Classes the theme styles (Rwidget_set_class / Rwidget_add_class):
 *   "caption" "headline" "title" "large-title"   text sizes (TextRole)
 *   "muted"     secondary text (dimmer)
 *   "error"     text of an error (red)
 *   "on"        a toggle that is on: text and icon in the accent
 *   "current"   the item playing now: text in the accent
 *   "round"     a button with round ends / a circle
 *   "row"       a list row: flat, hover and press tints, rounded
 *   "bare"      a text input with no frame (inside a field component)
 */
#ifndef SMP_GUI_THEME_H
#define SMP_GUI_THEME_H

#include <stddef.h>

#include <Raven.h>

typedef enum {
    TEXT_BODY,     /* the theme's own font size, no class */
    TEXT_CAPTION,  /* secondary lines, times, counts */
    TEXT_HEADLINE, /* section and sheet titles; bold */
    TEXT_TITLE,    /* the track title; bold */
    TEXT_LARGE,    /* "Library" and card titles; bold */
    TEXT_ROLES
} TextRole;

/* Look, font, styles and the default accent. Called by window_open, after
 * Rinit and before any widget exists. */
void theme_init(void);

/* The text size setting (CFG_FONT_*), applied at once to every window.
 * No-op when it is already the one in use. */
void theme_set_text_size(int cfg_font_size);

/* Size of a text role in logical units at scale 1, its class ("" for
 * TEXT_BODY), and whether it is bold. */
float theme_text_size(TextRole role);
const char *theme_text_class(TextRole role);
int theme_text_bold(TextRole role);

/* The font of every text, regular or bold (Raven derives the bold one). */
Rid theme_font(int bold);

/* Album art bytes (JPEG or PNG) for the wallpaper and the accent; NULL or
 * an image that cannot be read brings back the defaults. Decodes on the UI
 * thread, once per image. */
void theme_set_art(const unsigned char *data, size_t len);

/* The accent in use (taken from the art, or the default). */
void theme_accent(float rgb[3]);

/* Colors of the design that are not roles of the theme: for custom
 * painting. */
#define THEME_TEXT_DIM_ALPHA 0.62f /* "muted" text: white at this alpha */

#endif /* SMP_GUI_THEME_H */
