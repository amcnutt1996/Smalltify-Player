/*
 * common.c: text and layout helpers shared by the screens, ported from
 * ui.c onto the GUI kit.
 */
#include <stdio.h>
#include <string.h>

#include "common.h"

float px_of(const Ui *u, int fs)
{
    return (float)(fs == FS_L ? u->m.fl : u->m.fs);
}

/* No extra letter spacing: the glyph advances alone make a text's width,
 * so the width of a string is the sum of its characters' widths. */
float text_w(const Ui *u, const char *s, int fs)
{
    return gui_text_width(px_of(u, fs), s);
}

/* Height of s, body text that may hold line breaks: one line per '\n' plus
 * the first, lines spaced as para_center steps them. */
float text_h(const Ui *u, const char *s)
{
    float h = gui_text_height((float)u->m.fs);
    for (const char *p = s; (p = strchr(p, '\n')) != NULL; p++)
        h += (float)u->m.fs + LINE_GAP;
    return h;
}

/* Top of one line of body text centered vertically in r. */
float mid_y(const Ui *u, GuiRect r)
{
    return r.y + (r.h - px_of(u, FS_S)) / 2.0f;
}

void text(const Ui *u, const char *s, float x, float y, int fs, GuiColor c)
{
    gui_text(px_of(u, fs), x, y, s, c);
}

void text_center(const Ui *u, const char *s, float y, int fs, GuiColor c)
{
    gui_text_center(px_of(u, fs), (float)u->w / 2.0f, y, s, c);
}

/* src in body text, word-wrapped to maxw at (x, y); returns its height. */
float para(const Ui *u, const char *src, float x, float y, float maxw, GuiColor c)
{
    char buf[512];
    gui_wrap_text(px_of(u, FS_S), src, maxw, 0, buf, sizeof buf);
    text(u, buf, x, y, FS_S, c);
    return text_h(u, buf);
}

/* A message in body text, wrapped to the window and centered line by line;
 * returns its height. */
float para_center(const Ui *u, const char *src, float y, GuiColor c)
{
    char buf[512], line[512];
    gui_wrap_text(px_of(u, FS_S), src, (float)(u->w - 2 * PAD), 0, buf, sizeof buf);
    for (const char *s = buf;; y += (float)u->m.fs + LINE_GAP) {
        size_t n = strcspn(s, "\n");
        snprintf(line, sizeof line, "%.*s", (int)n, s);
        text_center(u, line, y, FS_S, c);
        if (!s[n])
            return text_h(u, buf);
        s += n + 1;
    }
}

/* A button width that fits label: minw, or more for the label and a margin
 * of one text height on either side. */
float btn_w(const Ui *u, const char *label, float minw)
{
    float w = text_w(u, label, FS_S) + 2.0f * (float)u->m.fs;
    return w > minw ? w : minw;
}

/* One-line text box: s with a static caret (a blinking one would need
 * timed redraws), scrolled to its end; the placeholder, cut to fit, when s
 * is empty. */
void draw_field(const Ui *u, GuiRect box, const char *s, const char *placeholder)
{
    char shown[SP_NAME_LEN + 2];
    float room = box.w - 16, ty = mid_y(u, box);
    gui_fill(box, 0.3f, GUI_PANEL);
    if (s[0]) {
        snprintf(shown, sizeof shown, "%s_", s);
        float tw = text_w(u, shown, FS_S);
        gui_clip_begin((GuiRect){ (float)((int)box.x + 8), (float)(int)box.y,
                                  (float)(int)room, (float)(int)box.h });
        text(u, shown, box.x + 8 + (tw > room ? room - tw : 0), ty, FS_S, GUI_TEXT);
        gui_clip_end();
    } else {
        gui_fit_text(px_of(u, FS_S), placeholder, room, shown, sizeof shown);
        text(u, shown, box.x + 8, ty, FS_S, GUI_DIM);
    }
}
