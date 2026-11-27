/* player_seek.c: the player's seek bar (player_seek.h): an Overlay of a
 * custom widget that paints the bar at rest, and a comp_slider over it,
 * transparent until it is held. The bar is painted with the geometry and
 * colors of Raven's glass slider (track, accent fill, white capsule knob),
 * read from the slider itself, so it looks like the volume slider. */
#include "screens/player_seek.h"

#include <math.h>

static struct {
    Rid box, bar, slider;
    CompSliderFn change, commit;
    void *ud;
    float value;
    int enabled;
    int shown; /* the slider is painted: it is being dragged */
    int spent; /* it was painted: replace it */
} g;

/* The length of the knob along the track: Raven's glass slider makes it a
 * capsule half again as long as it is high. */
static float knob_length(void)
{
    return floorf(Rwidget_get_metric(g.slider, R_METRIC_SLIDER_HANDLE_SIZE) * 1.5f);
}

static void bar_paint(Rid widget, RPainter *p, float width, float height, void *ud)
{
    (void)widget;
    (void)ud;
    if (g.shown || g.slider == R_INVALID_ID)
        return; /* the slider paints itself while it is held */
    float knob_h = Rwidget_get_metric(g.slider, R_METRIC_SLIDER_HANDLE_SIZE);
    float knob_w = knob_length(), track = 6.0f;
    float travel = fmaxf(width - knob_w, 0.0f), center = knob_w * 0.5f + travel * g.value;
    float y = floorf((height - track) * 0.5f);
    float r, gr, b, a;
    Rwidget_get_color(g.slider, R_COLOR_TRACK, &r, &gr, &b, &a);
    Rdraw_rect(p, knob_w * 0.5f, y, travel, track, track * 0.5f, r, gr, b, a);
    if (g.value > 0.0f) {
        Rwidget_get_color(g.slider, R_COLOR_ACCENT, &r, &gr, &b, &a);
        Rdraw_rect(p, knob_w * 0.5f, y, travel * g.value, track, track * 0.5f, r, gr, b, g.enabled ? a : a * 0.4f);
    }
    /* the knob: a capsule over a soft shadow, a little below it */
    float kx = center - knob_w * 0.5f, ky = (height - knob_h) * 0.5f, kr = fminf(knob_w, knob_h) * 0.5f;
    for (float s = 3.0f; s >= 1.0f; s -= 1.0f)
        Rdraw_rect(p, kx - s, ky + 1.5f - s, knob_w + 2.0f * s, knob_h + 2.0f * s, kr + s, 0.0f, 0.0f, 0.0f, 0.05f);
    if (g.enabled)
        Rdraw_rect(p, kx, ky, knob_w, knob_h, kr, 1.0f, 1.0f, 1.0f, 1.0f);
    else
        Rdraw_rect(p, kx, ky, knob_w, knob_h, kr, 0.85f, 0.85f, 0.87f, 1.0f);
}

static void slider_change(Rid slider, float value, void *ud)
{
    (void)ud;
    if (!g.shown) {
        /* picked up: the slider shows, with its lens, from where it is */
        g.shown = 1;
        Rwidget_set_opacity(slider, 1.0f);
        Rwidget_invalidate(g.bar);
    }
    g.value = value;
    if (g.change)
        g.change(slider, value, g.ud);
}

static void slider_commit(Rid slider, float value, void *ud)
{
    (void)ud;
    g.value = value;
    if (g.shown) {
        g.shown = 0;
        g.spent = 1;
        Rwidget_set_opacity(slider, 0.0f);
    }
    Rwidget_invalidate(g.bar);
    if (g.commit)
        g.commit(slider, value, g.ud);
}

/* A slider that was never painted: its knob starts where its value is. */
static void new_slider(void)
{
    g.slider = comp_slider(g.box, 0.0f, 1.0f, slider_change, slider_commit, NULL);
    Rwidget_set_opacity(g.slider, 0.0f);
    Rwidget_set_accessible_name(g.slider, "Position");
    Rslider_set_value(g.slider, g.value);
    comp_set_enabled(g.slider, g.enabled);
}

Rid seek_build(Rid parent, CompSliderFn on_change, CompSliderFn on_commit, void *ud)
{
    g.change = on_change;
    g.commit = on_commit;
    g.ud = ud;
    g.value = 0.0f;
    g.enabled = 1;
    g.shown = g.spent = 0;
    g.box = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_OVERLAY));
    g.bar = Rwidget_add_child(g.box, Rwidget_create(R_WIDGET_CUSTOM));
    Rcustom_set_paint_callback(g.bar, bar_paint, NULL);
    Rwidget_set_hit_testable(g.bar, R_FALSE);
    Rwidget_set_accessible_hidden(g.bar, R_TRUE);
    new_slider();
    return g.box;
}

void seek_set(float value)
{
    value = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    if (seek_held() || value == g.value)
        return;
    g.value = value;
    comp_slider_set(g.slider, value); /* not painted: no spring, and what a screen reader reads */
    Rwidget_invalidate(g.bar);
}

void seek_set_enabled(int enabled)
{
    enabled = enabled != 0;
    if (enabled == g.enabled)
        return;
    g.enabled = enabled;
    comp_set_enabled(g.slider, enabled);
    Rwidget_invalidate(g.bar);
}

int seek_held(void)
{
    return g.shown || comp_slider_held(g.slider);
}

float seek_inset(void)
{
    return knob_length() * 0.5f;
}

void seek_update(void)
{
    if (!g.spent || comp_slider_held(g.slider))
        return;
    g.spent = 0;
    Rwidget_destroy(g.slider);
    new_slider();
}
