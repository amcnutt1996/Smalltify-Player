/*
 * gui.c: the GUI kit on Raven, except the widgets (widgets.c). Input state,
 * the paint bracket, text, shapes, clipping, hit tests and the one-line
 * text field. Ported from the helpers of the old ui.c.
 */
#include "gui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELLIPSIS "\xE2\x80\xA6" /* U+2026 */

/* Between the lines of one text, as the old build spaced them: a line break
 * moves down size + LINE_GAP. */
#define LINE_GAP 4.0f

GuiInput gui_input;
int      gui_active;

static RPainter *painter;
static Rid       font = R_INVALID_ID;
/* Raven's font size is an em; the old build's was the pixel height of ascent
 * to descent. This ratio converts the old size to an em, so glyphs and line
 * heights keep their old size. */
static float     em_per_px = 1.0f;

/* --- frame and input ----------------------------------------------------- */

void gui_begin_paint(RPainter *p, float width, float height)
{
    painter = p;
    (void)width;
    (void)height;
}

void gui_end_paint(void)
{
    painter = NULL;
    gui_input.clicked = 0;
    gui_input.released = 0;
    gui_input.wheel = 0.0f;
    gui_input.key_enter = 0;
    gui_input.key_escape = 0;
    gui_input.key_backspace = 0;
    gui_input.key_paste = 0;
    gui_input.text[0] = 0;
    if (!gui_input.down)
        gui_active = 0;
}

/* UTF-8 bytes of cp: 0 bytes past U+10FFFF. */
static int utf8_encode(uint32_t cp, char out[4])
{
    if (cp <= 0x7f) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7ff) {
        out[0] = (char)(((cp >> 6) & 0x1f) | 0xc0);
        out[1] = (char)((cp & 0x3f) | 0x80);
        return 2;
    }
    if (cp <= 0xffff) {
        out[0] = (char)(((cp >> 12) & 0x0f) | 0xe0);
        out[1] = (char)(((cp >> 6) & 0x3f) | 0x80);
        out[2] = (char)((cp & 0x3f) | 0x80);
        return 3;
    }
    if (cp <= 0x10ffff) {
        out[0] = (char)(((cp >> 18) & 0x07) | 0xf0);
        out[1] = (char)(((cp >> 12) & 0x3f) | 0x80);
        out[2] = (char)(((cp >> 6) & 0x3f) | 0x80);
        out[3] = (char)((cp & 0x3f) | 0x80);
        return 4;
    }
    return 0;
}

/* Next codepoint of s and its byte length in *step: a malformed sequence
 * reads as '?' of one byte. */
static int utf8_next(const char *s, int *step)
{
    const unsigned char *p = (const unsigned char *)s;
    *step = 1;
    if ((p[0] & 0xf8) == 0xf0) {
        if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80)
            return '?';
        *step = 4;
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    }
    if ((p[0] & 0xf0) == 0xe0) {
        if ((p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
            return '?';
        *step = 3;
        return ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    }
    if ((p[0] & 0xe0) == 0xc0) {
        if ((p[1] & 0xc0) != 0x80)
            return '?';
        *step = 2;
        return ((p[0] & 0x1f) << 6) | (p[1] & 0x3f);
    }
    if ((p[0] & 0x80) == 0)
        return p[0];
    return '?';
}

int gui_event(const REvent *e)
{
    switch (e->type) {
    case R_EVENT_MOUSE_MOVE:
        gui_input.mx = e->x;
        gui_input.my = e->y;
        return 1;
    case R_EVENT_MOUSE_DOWN:
        gui_input.mx = e->x;
        gui_input.my = e->y;
        if (e->button != R_MOUSE_LEFT)
            return 0;
        gui_input.down = 1;
        gui_input.clicked = 1;
        return 1;
    case R_EVENT_MOUSE_UP:
        gui_input.mx = e->x;
        gui_input.my = e->y;
        if (e->button != R_MOUSE_LEFT)
            return 0;
        gui_input.down = 0;
        gui_input.released = 1;
        return 1;
    case R_EVENT_MOUSE_WHEEL:
        /* wheel_y is in lines for a notched and a trackpad wheel alike. */
        gui_input.wheel += e->wheel_y;
        return 1;
    case R_EVENT_KEY_DOWN:
        /* Backspace fires on the press and on auto-repeat, as the old
         * GetKeyPressed plus IsKeyPressedRepeat did. Each event counts, so
         * none is lost when several arrive before one paint. The other keys
         * fire on the press edge only, as the old IsKeyPressed did. */
        if (e->key == R_KEY_BACKSPACE) {
            gui_input.key_backspace++;
            return 1;
        }
        if (e->repeat)
            return 0;
        if (e->key == R_KEY_ENTER || e->key == R_KEY_KP_ENTER) {
            gui_input.key_enter = 1;
            return 1;
        }
        if (e->key == R_KEY_ESCAPE) {
            gui_input.key_escape = 1;
            return 1;
        }
        if (e->key == R_KEY_V && (e->modifiers & (R_MOD_CTRL | R_MOD_SUPER))) {
            gui_input.key_paste = 1;
            return 1;
        }
        return 0;
    case R_EVENT_TEXT: {
        char bytes[4];
        size_t len = strlen(gui_input.text);
        int n = e->codepoint ? utf8_encode(e->codepoint, bytes) : 0;
        if (n > 0 && len + (size_t)n < sizeof gui_input.text) {
            memcpy(gui_input.text + len, bytes, (size_t)n);
            gui_input.text[len + (size_t)n] = 0;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static char *gui_clipboard_text(void)
{
    const char *clip = Rclipboard_get_text();
    if (!clip || !clip[0])
        return NULL;
    size_t n = strlen(clip) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, clip, n);
    return copy;
}

/* --- fonts and text ------------------------------------------------------ */

static unsigned be16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* unitsPerEm / (ascent - descent) from the head and hhea tables. The old
 * build sized a font by that pixel height (stbtt_ScaleForPixelHeight). */
static int ttf_em_per_px(const unsigned char *ttf, size_t len, float *out)
{
    if (len < 12)
        return -1;
    size_t tables = be16(ttf + 4);
    if (12 + 16 * tables > len)
        return -1;
    long upm = 0, ascent = 0, descent = 0;
    int have = 0;
    for (size_t i = 0; i < tables; i++) {
        const unsigned char *rec = ttf + 12 + 16 * i;
        size_t off = be32(rec + 8);
        if (memcmp(rec, "head", 4) == 0 && off + 20 <= len) {
            upm = (long)be16(ttf + off + 18);
            have |= 1;
        } else if (memcmp(rec, "hhea", 4) == 0 && off + 8 <= len) {
            ascent = (int16_t)be16(ttf + off + 4);
            descent = (int16_t)be16(ttf + off + 6);
            have |= 2;
        }
    }
    if (have != 3 || upm <= 0 || ascent - descent <= 0)
        return -1;
    *out = (float)upm / (float)(ascent - descent);
    return 0;
}

int gui_font_init(const unsigned char *ttf, size_t len)
{
    float ratio;
    gui_font_shutdown();
    if (ttf_em_per_px(ttf, len, &ratio) != 0)
        return -1;
    font = Rfont_create((const char *)ttf, len);
    if (font == R_INVALID_ID)
        return -1;
    em_per_px = ratio;
    return 0;
}

void gui_font_shutdown(void)
{
    if (font != R_INVALID_ID)
        Rfont_destroy(font);
    font = R_INVALID_ID;
}

/* No letter spacing: a text's width is the sum of its glyphs' advances. */
float gui_text_width(float size, const char *text)
{
    float w = 0.0f;
    Rfont_measure_text(font, size * em_per_px, text, &w, NULL);
    return w;
}

/* One line is exactly size tall: em_per_px makes Raven's line box size, the
 * same as the old one-line text measure. Line breaks add the gap in
 * gui_text and in the callers' own line steps. */
float gui_text_height(float size)
{
    return size;
}

/* Lines are drawn one by one so a line break moves down size + LINE_GAP,
 * as the old build's text drawing did; Rdraw_text would step by the font's line. */
void gui_text(float size, float x, float y, const char *text, GuiColor c)
{
    float em = size * em_per_px;
    if (!strchr(text, '\n')) {
        Rdraw_text(painter, font, em, x, y, text, c.r, c.g, c.b, c.a);
        return;
    }
    char buf[512];
    for (const char *s = text;; y += size + LINE_GAP) {
        size_t n = strcspn(s, "\n");
        char *line = n < sizeof buf ? buf : malloc(n + 1);
        if (!line)
            return; /* out of memory: stop rather than skip a line silently */
        memcpy(line, s, n);
        line[n] = 0;
        Rdraw_text(painter, font, em, x, y, line, c.r, c.g, c.b, c.a);
        if (line != buf)
            free(line);
        if (!s[n])
            return;
        s += n + 1;
    }
}

void gui_text_center(float size, float cx, float y, const char *text, GuiColor c)
{
    gui_text(size, cx - gui_text_width(size, text) / 2.0f, y, text, c);
}

/* Cut out, in place, at a UTF-8 boundary with an ellipsis so it fits
 * max_w. Widths add up (see gui_text_width), so one pass finds the cut. */
static void fit_cut(float size, char *out, float max_w, size_t cap)
{
    if (gui_text_width(size, out) <= max_w)
        return;
    float room = max_w - gui_text_width(size, ELLIPSIS), w = 0.0f;
    size_t n = 0;
    while (out[n]) {
        char ch[5];
        size_t len = 1;
        while (len < 4 && ((unsigned char)out[n + len] & 0xC0) == 0x80)
            len++;
        memcpy(ch, out + n, len);
        ch[len] = 0;
        if ((w += gui_text_width(size, ch)) > room)
            break;
        n += len;
    }
    while (n > 0 && n + sizeof ELLIPSIS > cap) { /* no room for the ellipsis */
        do
            n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80);
    }
    if (n + sizeof ELLIPSIS <= cap)
        memcpy(out + n, ELLIPSIS, sizeof ELLIPSIS);
}

char *gui_fit_text(float size, const char *text, float max_w, char *out, size_t cap)
{
    snprintf(out, cap, "%s", text);
    fit_cut(size, out, max_w, cap);
    return out;
}

static int count_lines(const char *s)
{
    int n = 1;
    for (; *s; s++)
        n += *s == '\n';
    return n;
}

/* Word-wrap src into out at the spaces, so lines fit max_w where a word
 * break allows; src's own line breaks stay. */
static void wrap_words(float size, const char *src, float max_w, char *out, size_t cap)
{
    snprintf(out, cap, "%s", src);
    size_t line = 0, space = 0; /* start of the current line; its last space if > line */
    for (size_t i = 0;; i++) {
        char c = out[i];
        if (c && c != ' ' && c != '\n')
            continue;
        out[i] = 0;
        int over = gui_text_width(size, out + line) > max_w;
        out[i] = c;
        if (over && space > line) { /* the word just ended starts a new line */
            out[space] = '\n';
            line = space + 1;
        }
        if (!c)
            return;
        if (c == '\n')
            line = i + 1;
        else
            space = i;
    }
}

/* The first n bytes of src as the last line: newlines become blanks, and the
 * text is cut with an ellipsis to fit max_w. */
static void cut_line(float size, const char *src, size_t n, float max_w, char *out, size_t cap)
{
    if (n > cap - 1)
        n = cap - 1;
    memcpy(out, src, n);
    out[n] = 0;
    for (char *p = out; *p; p++)
        if (*p == '\n')
            *p = ' ';
    fit_cut(size, out, max_w, cap);
}

/* At most max_lines lines (max_lines > 0). This is the old fit_two_lines for
 * any count: a line ends at a '\n' or at its last space that fits. The last
 * line takes the rest, cut with an ellipsis; a word that cannot break is
 * cut the same way. */
static int wrap_capped(float size, const char *text, float max_w, int max_lines, char *out, size_t cap)
{
    const char *src = text;
    size_t o = 0;
    for (int lines = 1;; lines++) {
        size_t seg = strcspn(src, "\n"), room = cap - o;
        if (lines == max_lines) {
            cut_line(size, src, strlen(src), max_w, out + o, room);
            return lines;
        }
        if (seg + 2 > room) { /* out of room: the segment is the last line */
            cut_line(size, src, seg, max_w, out + o, room);
            return lines;
        }
        snprintf(out + o, room, "%.*s", (int)seg, src);
        if (gui_text_width(size, out + o) <= max_w) {
            if (!src[seg])
                return lines;
            out[o + seg] = '\n';
            o += seg + 1;
            src += seg + 1;
            continue;
        }
        size_t best = 0;
        for (size_t i = 0; i < seg; i++) {
            if (src[i] != ' ')
                continue;
            memcpy(out + o, src, i);
            out[o + i] = 0;
            if (gui_text_width(size, out + o) > max_w)
                break;
            best = i;
        }
        if (best == 0) { /* no word break fits: one line, cut */
            cut_line(size, src, seg, max_w, out + o, room);
            return lines;
        }
        memcpy(out + o, src, best);
        out[o + best] = '\n';
        o += best + 1;
        src += best + 1;
    }
}

int gui_wrap_text(float size, const char *text, float max_w, int max_lines, char *out, size_t cap)
{
    if (cap == 0)
        return 0;
    if (max_lines <= 0) {
        wrap_words(size, text, max_w, out, cap);
        return count_lines(out);
    }
    return wrap_capped(size, text, max_w, max_lines, out, cap);
}

/* The old wrap_text: hard-wrap at any character, spaces included. */
int gui_wrap_path(float size, const char *text, float max_w, char *out, size_t cap)
{
    size_t o = 0, line = 0, i = 0;
    int lines = 1;
    if (cap == 0)
        return 0;
    out[0] = 0;
    while (text[i]) {
        size_t len = 1;
        while (text[i + len] && ((unsigned char)text[i + len] & 0xC0) == 0x80)
            len++;
        if (o + len + 2 > cap)
            break;
        memcpy(out + o, text + i, len);
        out[o + len] = 0;
        if (o > line && gui_text_width(size, out + line) > max_w) {
            memmove(out + o + 1, out + o, len);
            out[o] = '\n';
            line = ++o;
            lines++;
        }
        o += len;
        out[o] = 0;
        i += len;
    }
    return lines;
}

/* --- shapes -------------------------------------------------------------- */

/* Corner radius of a rounded rectangle: roundness (at most 1) of half the
 * shorter side. */
static float radius_of(GuiRect r, float roundness)
{
    if (roundness >= 1.0f)
        roundness = 1.0f;
    return (r.w > r.h ? r.h : r.w) * roundness / 2.0f;
}

void gui_fill(GuiRect r, float roundness, GuiColor c)
{
    float radius = 0.0f;
    if (roundness > 0.0f && r.w >= 1.0f && r.h >= 1.0f)
        radius = radius_of(r, roundness);
    Rdraw_rect(painter, r.x, r.y, r.w, r.h, radius, c.r, c.g, c.b, c.a);
}

/* The old outline lay outside r (its ring ran from r's edge out by the
 * thickness); Raven draws it inside its bounds, so the bounds grow by the
 * thickness. */
void gui_outline(GuiRect r, float roundness, float thickness, GuiColor c)
{
    if (thickness < 0.0f)
        thickness = 0.0f;
    float radius = 0.0f;
    if (roundness > 0.0f) {
        radius = radius_of(r, roundness);
        if (radius <= 0.0f)
            return;
        radius += thickness;
    }
    Rdraw_rect_outline(painter, r.x - thickness, r.y - thickness, r.w + 2.0f * thickness,
                       r.h + 2.0f * thickness, radius, thickness, c.r, c.g, c.b, c.a);
}

void gui_circle(float cx, float cy, float radius, GuiColor c)
{
    Rdraw_circle(painter, cx, cy, radius, c.r, c.g, c.b, c.a);
}

void gui_line(float x0, float y0, float x1, float y1, float thickness, GuiColor c)
{
    Rdraw_line(painter, x0, y0, x1, y1, thickness, c.r, c.g, c.b, c.a);
}

void gui_triangle(float x0, float y0, float x1, float y1, float x2, float y2, GuiColor c)
{
    Rdraw_triangle(painter, x0, y0, x1, y1, x2, y2, c.r, c.g, c.b, c.a);
}

void gui_image(Rid texture, GuiRect r, GuiColor tint)
{
    Rdraw_image(painter, texture, r.x, r.y, r.w, r.h, tint.r, tint.g, tint.b, tint.a);
}

void gui_clip_begin(GuiRect r)
{
    Rdraw_push_clip_rect(painter, r.x, r.y, r.w, r.h);
}

void gui_clip_end(void)
{
    Rdraw_pop_clip_rect(painter);
}

/* Point-in-rectangle test. */
int gui_hit(GuiRect r)
{
    return gui_input.mx >= r.x && gui_input.mx < r.x + r.w &&
           gui_input.my >= r.y && gui_input.my < r.y + r.h;
}

/* --- text field ---------------------------------------------------------- */

/* Remove the last UTF-8 character of s. Returns 1 if s was not empty. */
static int pop_utf8(char *s)
{
    size_t n = strlen(s);
    if (n == 0)
        return 0;
    while (n > 0 && ((unsigned char)s[--n] & 0xC0) == 0x80)
        ;
    s[n] = 0;
    return 1;
}

/* Append codepoint cp if it fits whole. key_chars keeps only what an API
 * key holds (printable ASCII, no blanks), so a pasted line break or space
 * is dropped. Returns 1 when appended. */
static int append_cp(char *buf, size_t cap, size_t *len, int cp, int key_chars)
{
    /* AppKit reports arrows, function and editing keys as U+F700-U+F8FF */
    if (cp < ' ' || cp == 0x7f || (cp >= 0xF700 && cp <= 0xF8FF) || (key_chars && (cp == ' ' || cp > '~')))
        return 0;
    char bytes[4];
    int n = utf8_encode((uint32_t)cp, bytes);
    if (n > 0 && *len + (size_t)n < cap) {
        memcpy(buf + *len, bytes, (size_t)n);
        *len += (size_t)n;
        buf[*len] = 0;
        return 1;
    }
    return 0;
}

/* Keyboard input for a one-line field: typing, Backspace, and paste with
 * Cmd+V (macOS) or Ctrl+V, from this frame's gui_input. */
void gui_edit_line(char *buf, size_t cap, int key_chars)
{
    int step = 1;
    size_t len = strlen(buf);
    for (const char *t = gui_input.text; *t; t += step)
        append_cp(buf, cap, &len, utf8_next(t, &step), key_chars);
    for (int k = 0; k < gui_input.key_backspace; k++) {
        pop_utf8(buf);
        len = strlen(buf);
    }
    if (gui_input.key_paste) {
        char *clip = gui_clipboard_text();
        for (const char *p = clip; p && *p; p += step) {
            append_cp(buf, cap, &len, utf8_next(p, &step), key_chars);
        }
        free(clip);
    }
}
