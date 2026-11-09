/*
 * common.h: text and layout helpers shared by the screens, ported from
 * ui.c onto the GUI kit. fs is FS_S (body) or FS_L (title).
 */
#ifndef SMP_SCREENS_COMMON_H
#define SMP_SCREENS_COMMON_H

#include "../ui_state.h"
#include "../gui/gui.h"

/* Text size in logical pixels for fs. */
float px_of(const Ui *u, int fs);
/* Width of s at fs. */
float text_w(const Ui *u, const char *s, int fs);
/* Height of s in body text, which may hold line breaks. */
float text_h(const Ui *u, const char *s);
/* Top of one line of body text centred vertically in r. */
float mid_y(const Ui *u, GuiRect r);
/* s at fs with its top-left corner at (x, y). */
void text(const Ui *u, const char *s, float x, float y, int fs, GuiColor c);
/* s at fs centred on the window, top at y. */
void text_center(const Ui *u, const char *s, float y, int fs, GuiColor c);
/* src in body text, word-wrapped to maxw at (x, y); returns its height. */
float para(const Ui *u, const char *src, float x, float y, float maxw, GuiColor c);
/* A message in body text, wrapped to the window and centred line by line;
 * returns its height. */
float para_center(const Ui *u, const char *src, float y, GuiColor c);
/* A button width that fits label: minw, or more for the label and a margin
 * of one text height on either side. */
float btn_w(const Ui *u, const char *label, float minw);
/* One-line text box: s with a static caret, scrolled to its end; the
 * placeholder, cut to fit, when s is empty. */
void draw_field(const Ui *u, GuiRect box, const char *s, const char *placeholder);

#endif /* SMP_SCREENS_COMMON_H */
