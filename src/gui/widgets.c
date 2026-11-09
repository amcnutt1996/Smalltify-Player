/*
 * widgets.c: the GUI kit's widgets (buttons, transport icons, slider,
 * heart). Ported from the old ui.c; draws and reads input only
 * through gui.h.
 */
#include "gui.h"
#include "window.h"

#include <math.h>

/* Press inside r makes id active; release inside while active is a click.
 * A press outside never activates, as the old clicked(). */
int gui_clicked(int id, GuiRect r)
{
    int hot = gui_hit(r);
    int click = 0;
    if (gui_active == 0 && hot && gui_input.clicked)
        gui_active = id;
    if (gui_active == id && !gui_input.down) {
        gui_active = 0;
        click = hot;
        /* the old code redrew after a click so the new state shows at once */
        if (click)
            window_redraw();
    }
    return click;
}

/* The button background in its state; no input. */
static void face(int id, GuiRect r, int on)
{
    int hot = gui_hit(r);
    GuiColor bg = on ? GUI_ON : GUI_PANEL;
    if (gui_active == id && hot)
        bg = GUI_PRESS;
    else if (hot && !on)
        bg = GUI_HOVER;
    gui_fill(r, 0.3f, bg);
}

/* The button body in its state; returns 1 on a click. */
static int body(int id, GuiRect r, int on)
{
    int click = gui_clicked(id, r);
    face(id, r, on);
    return click;
}

int gui_button(int id, GuiRect r, const char* label, float size, int on)
{
    int click = body(id, r, on);
    /* the old code centred a line of the font size's height, not the font's own line height */
    if (label)
        gui_text_center(size, r.x + r.w / 2.0f, r.y + (r.h - size) / 2.0f, label, GUI_TEXT);
    return click;
}

void gui_icon(GuiRect r, GuiIcon icon, GuiColor c)
{
    float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, s = r.h * 0.22f;
    switch (icon) {
    case GUI_ICON_PLAY:
        gui_triangle(cx - s * 0.8f, cy - s, cx - s * 0.8f, cy + s, cx + s, cy, c);
        break;
    case GUI_ICON_PAUSE:
        gui_fill((GuiRect){ cx - s * 0.8f, cy - s, s * 0.55f, s * 2.0f }, 0.0f, c);
        gui_fill((GuiRect){ cx + s * 0.25f, cy - s, s * 0.55f, s * 2.0f }, 0.0f, c);
        break;
    case GUI_ICON_PREV:
        gui_fill((GuiRect){ cx - s, cy - s, s * 0.3f, s * 2.0f }, 0.0f, c);
        gui_triangle(cx + s, cy - s, cx - s * 0.6f, cy, cx + s, cy + s, c);
        break;
    case GUI_ICON_NEXT:
        gui_triangle(cx - s, cy - s, cx - s, cy + s, cx + s * 0.6f, cy, c);
        gui_fill((GuiRect){ cx + s * 0.7f, cy - s, s * 0.3f, s * 2.0f }, 0.0f, c);
        break;
    }
}

int gui_icon_button(int id, GuiRect r, GuiIcon icon)
{
    int click = body(id, r, 0);
    gui_icon(r, icon, GUI_TEXT);
    return click;
}

void gui_icon_face(int id, GuiRect r, GuiIcon icon, int on)
{
    face(id, r, on);
    gui_icon(r, icon, GUI_TEXT);
}

void gui_slider(int id, GuiRect r, float* value, int enabled, int* released)
{
    /* a hit area larger than the thin bar, as before */
    GuiRect hit = { r.x - 6, r.y - 8, r.w + 12, r.h + 16 };
    int hot = gui_hit(hit);
    *released = 0;
    if (enabled && gui_active == 0 && hot && gui_input.clicked)
        gui_active = id;
    if (gui_active == id) {
        float v = (gui_input.mx - r.x) / r.w;
        *value = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        if (!gui_input.down) {
            gui_active = 0;
            *released = 1;
            window_redraw();
        }
    }
    float fill = r.w * *value;
    gui_fill(r, 1.0f, GUI_PANEL);
    if (enabled) {
        int lit = gui_active == id || hot;
        gui_fill((GuiRect){ r.x, r.y, fill, r.h }, 1.0f, lit ? GUI_ACCENT : GUI_TEXT);
        if (lit)
            gui_circle(r.x + fill, r.y + r.h / 2.0f, r.h + 1.0f, GUI_TEXT);
    }
}

/* A heart in the middle of r: solid when on, else its outline. */
static void draw_heart(GuiRect r, int on, float th, GuiColor c)
{
    enum { N = 48 };
    const float pi = 3.14159265358979f;
    float px[N + 1], py[N + 1];
    float k = 0.7f * r.w / 32.0f, cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
    /* x = 16 sin^3 t, y = 13 cos t - 5 cos 2t - 2 cos 3t - cos 4t (32 wide,
     * its middle at y = -2.6), centred on (cx, cy). */
    for (int i = 0; i <= N; i++) {
        float t = -2.0f * pi * (float)i / N, s = sinf(t);
        float y = 13.0f * cosf(t) - 5.0f * cosf(2 * t) - 2.0f * cosf(3 * t) - cosf(4 * t);
        px[i] = cx + k * 16.0f * s * s * s;
        py[i] = cy - k * (y + 2.6f);
    }
    if (on) {
        for (int i = 0; i < N; i++)
            gui_triangle(cx, cy, px[i], py[i], px[i + 1], py[i + 1], c);
    } else {
        for (int i = 0; i < N; i++) {
            gui_circle(px[i], py[i], th / 2.0f, c); /* round joints, as DrawSplineLinear */
            gui_line(px[i], py[i], px[i + 1], py[i + 1], th, c);
        }
        gui_circle(px[N], py[N], th / 2.0f, c);
    }
}

void gui_heart(GuiRect r, int on, float thickness)
{
    GuiColor c = on ? GUI_ACCENT : gui_hit(r) ? GUI_TEXT : GUI_DIM;
    draw_heart(r, on, thickness, c);
}
