/*
 * components.c: the reusable pieces of components.h. Each one is made of
 * Raven widgets with the classes of the theme (theme.h); the one custom
 * widget is comp_text, a line of text cut with an ellipsis, which Raven's
 * Label cannot do.
 */
#include "components.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "window.h"

#define ELLIPSIS "\xE2\x80\xA6" /* U+2026 */

static Rid add(Rid parent, Rid widget)
{
    if (parent != R_INVALID_ID && widget != R_INVALID_ID)
        Rwidget_add_child(parent, widget);
    return widget;
}

/* ---- layout ----------------------------------------------------------------- */

Rid comp_vbox(Rid parent, float spacing)
{
    Rid box = add(parent, Rwidget_create(R_WIDGET_VBOX));
    Rpanel_set_spacing(box, spacing);
    return box;
}

Rid comp_hbox(Rid parent, float spacing)
{
    Rid box = add(parent, Rwidget_create(R_WIDGET_HBOX));
    Rpanel_set_spacing(box, spacing);
    return box;
}

Rid comp_fill(Rid parent)
{
    Rid spacer = add(parent, Rwidget_create(R_WIDGET_SPACER));
    Rslot_set_fill(spacer, 1.0f);
    return spacer;
}

Rid comp_scroll(Rid parent, float spacing)
{
    Rid scroll = add(parent, Rwidget_create(R_WIDGET_SCROLL_BOX));
    return comp_vbox(scroll, spacing);
}

/* ---- text ------------------------------------------------------------------- */

static void set_role(Rid widget, TextRole role)
{
    const char *cls = theme_text_class(role);
    if (cls[0])
        Rwidget_add_class(widget, cls);
}

Rid comp_label(Rid parent, const char *text, TextRole role)
{
    Rid label = add(parent, Rlabel_create(text ? text : ""));
    set_role(label, role);
    if (theme_text_bold(role))
        Rlabel_set_font(label, theme_font(1));
    Rlabel_set_wrap(label, R_TRUE);
    return label;
}

/* comp_text: what it shows, and the cut its last paint made. */
typedef struct {
    char *text;
    TextRole role;
    RTextAlign align;
    float fit_w, fit_size, fit_scale; /* what the cut was made for */
    size_t fit_len;                   /* bytes of text kept; all of them: no ellipsis */
    float shown_w, line_h;            /* width of what is drawn, height of a line */
} Text;

static Rid text_font(const Text *t)
{
    return theme_font(theme_text_bold(t->role));
}

/* Width of the first n bytes of s, followed by an ellipsis when cut. */
static float cut_width(Rid font, float size, const char *s, size_t n, int cut, char *buf)
{
    float w = 0.0f;
    memcpy(buf, s, n);
    strcpy(buf + n, cut ? ELLIPSIS : "");
    Rfont_measure_text(font, size, buf, &w, NULL);
    return w;
}

/* The longest start of the text, in whole characters and without blanks
 * before the ellipsis, that fits `width` with an ellipsis after it. Widths
 * grow with the length, so a binary search over the characters finds it. */
static void text_fit(Text *t, Rid font, float size, float width, char *buf)
{
    size_t len = strlen(t->text);
    t->fit_len = len;
    t->shown_w = cut_width(font, size, t->text, len, 0, buf);
    if (t->shown_w <= width + 0.5f)
        return;
    /* starts[k]: the bytes of the first k characters; a malformed
     * sequence counts as one character with what precedes it */
    size_t small[256], count = 1;
    size_t *starts = len < 256 ? small : malloc(len * sizeof *starts);
    if (!starts)
        return;
    starts[0] = 0;
    for (size_t i = 1; i < len; i++)
        if (((unsigned char)t->text[i] & 0xC0) != 0x80)
            starts[count++] = i;
    /* all `count` characters do not fit: keep 0 to count - 1 of them */
    size_t lo = 0, hi = count - 1;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        if (cut_width(font, size, t->text, starts[mid], 1, buf) <= width)
            lo = mid;
        else
            hi = mid - 1;
    }
    size_t keep = starts[lo];
    if (starts != small)
        free(starts);
    while (keep > 0 && t->text[keep - 1] == ' ')
        keep--;
    t->fit_len = keep;
    t->shown_w = cut_width(font, size, t->text, keep, 1, buf);
}

static int enabled_chain(Rid widget)
{
    for (Rid w = widget; w != R_INVALID_ID && Rwidget_is_valid(w); w = Rwidget_get_parent(w))
        if (!Rwidget_is_enabled(w))
            return 0;
    return 1;
}

static void text_measure(Rid widget, float *width, float *height, void *ud)
{
    Text *t = ud;
    Rid font = text_font(t);
    float size = Rwidget_get_metric(widget, R_METRIC_FONT_SIZE), w = 0.0f, h = 0.0f;
    Rfont_measure_text(font, size, t->text, &w, NULL);
    Rfont_measure_text(font, size, "Ag", NULL, &h); /* a line, even when empty */
    *width = ceilf(w);
    *height = ceilf(h);
}

static void text_paint(Rid widget, RPainter *painter, float width, float height, void *ud)
{
    Text *t = ud;
    if (!t->text[0])
        return;
    Rid font = text_font(t);
    float size = Rwidget_get_metric(widget, R_METRIC_FONT_SIZE), scale = Raccessibility_get_text_scale();
    size_t len = strlen(t->text);
    char small[512], *buf = len + sizeof ELLIPSIS <= sizeof small ? small : malloc(len + sizeof ELLIPSIS);
    if (!buf)
        return;
    if (t->fit_w != width || t->fit_size != size || t->fit_scale != scale) {
        text_fit(t, font, size, width, buf);
        Rfont_measure_text(font, size, "Ag", NULL, &t->line_h);
        t->fit_w = width;
        t->fit_size = size;
        t->fit_scale = scale;
    }
    memcpy(buf, t->text, t->fit_len);
    strcpy(buf + t->fit_len, t->fit_len < len ? ELLIPSIS : "");
    float r, g, b, a;
    Rwidget_get_color(widget, enabled_chain(widget) ? R_COLOR_TEXT : R_COLOR_TEXT_DISABLED, &r, &g, &b, &a);
    float x = t->align == R_TEXT_ALIGN_CENTER ? (width - t->shown_w) * 0.5f
            : t->align == R_TEXT_ALIGN_RIGHT  ? width - t->shown_w
                                              : 0.0f;
    Rdraw_text(painter, font, size, floorf(fmaxf(x, 0.0f)), floorf((height - t->line_h) * 0.5f), buf, r, g, b, a);
    if (buf != small)
        free(buf);
}

static void text_destroyed(Rid widget, void *ud)
{
    (void)widget;
    Text *t = ud;
    free(t->text);
    free(t);
}

static char *dup_text(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, s, n);
    return copy;
}

Rid comp_text(Rid parent, const char *text, TextRole role)
{
    Text *t = calloc(1, sizeof *t);
    char *copy = dup_text(text ? text : "");
    if (!t || !copy) {
        free(t);
        free(copy);
        return comp_label(parent, text, role); /* out of memory: no ellipsis */
    }
    t->text = copy;
    t->role = role;
    t->align = R_TEXT_ALIGN_LEFT;
    t->fit_w = -1.0f;
    Rid widget = Rwidget_create(R_WIDGET_CUSTOM);
    set_role(widget, role);
    Rwidget_set_user_handle(widget, t);
    Rcustom_set_measure_callback(widget, text_measure, t);
    Rcustom_set_paint_callback(widget, text_paint, t);
    Rwidget_set_destroy_callback(widget, text_destroyed, t);
    Rwidget_set_hit_testable(widget, R_FALSE);
    Rwidget_set_accessible_role(widget, R_ROLE_TEXT);
    Rwidget_set_accessible_name(widget, t->text);
    return add(parent, widget);
}

void comp_text_set(Rid widget, const char *s)
{
    if (!s)
        s = "";
    if (Rwidget_get_type(widget) == R_WIDGET_LABEL) {
        Rlabel_set_text(widget, s); /* Raven compares */
        return;
    }
    Text *t = Rwidget_get_user_handle(widget);
    if (!t || strcmp(t->text, s) == 0)
        return;
    char *copy = dup_text(s);
    if (!copy)
        return;
    free(t->text);
    t->text = copy;
    t->fit_w = -1.0f;
    Rwidget_set_accessible_name(widget, s);
    Rwidget_invalidate(widget);
}

void comp_text_align(Rid widget, RTextAlign align)
{
    Text *t = Rwidget_get_user_handle(widget);
    if (Rwidget_get_type(widget) == R_WIDGET_LABEL) {
        Rlabel_set_text_align(widget, align);
    } else if (t && t->align != align) {
        t->align = align;
        Rwidget_invalidate(widget);
    }
}

void comp_set_class(Rid widget, const char *cls, int on)
{
    if (!Rwidget_has_class(widget, cls) == !on)
        return;
    if (on)
        Rwidget_add_class(widget, cls);
    else
        Rwidget_remove_class(widget, cls);
}

void comp_set_visible(Rid widget, int visible)
{
    Rwidget_set_visibility(widget, visible ? R_VISIBLE : R_COLLAPSED); /* Raven compares */
}

void comp_set_enabled(Rid widget, int enabled)
{
    Rwidget_set_enabled(widget, enabled); /* Raven compares */
}

/* ---- surfaces ----------------------------------------------------------------- */

Rid comp_panel(Rid parent, PanelKind kind, float radius, float padding)
{
    static const RMaterial material[] = { R_MATERIAL_FROSTED_GLASS, R_MATERIAL_GLASS, R_MATERIAL_TINTED_GLASS };
    Rid border = add(parent, Rwidget_create(R_WIDGET_BORDER));
    Rborder_set_material(border, material[kind]);
    Rborder_set_corner_radius(border, radius, radius, radius, radius);
    Rborder_set_padding(border, padding, padding, padding, padding);
    Rborder_set_shadow(border, 28.0f, 0.0f, 8.0f);
    return comp_vbox(border, COMP_GAP);
}

/* ---- headers ------------------------------------------------------------------ */

CompHeader comp_header(Rid parent, const char *title, const char *link, RButtonClickedCallback cb, void *ud)
{
    CompHeader h;
    h.row = comp_hbox(parent, COMP_GAP);
    h.title = comp_text(h.row, title, TEXT_HEADLINE);
    Rslot_set_fill(h.title, 1.0f);
    Rslot_set_valign(h.title, R_VALIGN_CENTER);
    h.link = R_INVALID_ID;
    if (link) {
        h.link = comp_button(h.row, link, BUTTON_LINK, cb, ud);
        Rslot_set_valign(h.link, R_VALIGN_CENTER);
    }
    return h;
}

/* ---- buttons ------------------------------------------------------------------ */

static void button_common(Rid button, RButtonClickedCallback cb, void *ud)
{
    Rwidget_set_focusable(button, R_FALSE);
    if (cb)
        Rbutton_set_clicked_callback(button, cb, ud);
}

Rid comp_button(Rid parent, const char *text, ButtonKind kind, RButtonClickedCallback cb, void *ud)
{
    Rid button = add(parent, Rbutton_create(text ? text : ""));
    Rbutton_set_style(button, kind == BUTTON_PRIMARY ? R_BUTTON_PRIMARY
                              : kind == BUTTON_LINK  ? R_BUTTON_FLAT
                                                     : R_BUTTON_DEFAULT);
    if (kind == BUTTON_LINK)
        Rwidget_add_class(button, "on"); /* the accent */
    button_common(button, cb, ud);
    return button;
}

void comp_button_set_text(Rid button, const char *text)
{
    Rbutton_set_text(button, text ? text : "");
}

static const struct {
    float side, icon;
    RButtonStyle style;
} g_icon_buttons[] = {
    [ICON_BUTTON_SMALL] = { 28.0f, 16.0f, R_BUTTON_FLAT },
    [ICON_BUTTON_MEDIUM] = { 34.0f, 20.0f, R_BUTTON_FLAT },
    [ICON_BUTTON_LARGE] = { 52.0f, 26.0f, R_BUTTON_DEFAULT },
    [ICON_BUTTON_HERO] = { 68.0f, 30.0f, R_BUTTON_PRIMARY },
};

Rid comp_icon_button(Rid parent, Icon icon, IconButtonSize size, const char *tip, RButtonClickedCallback cb,
                     void *ud)
{
    Rid button = add(parent, Rwidget_create(R_WIDGET_BUTTON));
    Rbutton_set_style(button, g_icon_buttons[size].style);
    Rwidget_add_class(button, "round");
    Rwidget_set_size(button, g_icon_buttons[size].side, g_icon_buttons[size].side);
    Rwidget_set_metric(button, R_METRIC_PADDING_X, 0.0f);
    Rwidget_set_metric(button, R_METRIC_PADDING_Y, 0.0f);
    Rid image = icon_image(button, icon, g_icon_buttons[size].icon);
    Rslot_set_halign(image, R_HALIGN_CENTER);
    Rslot_set_valign(image, R_VALIGN_CENTER);
    Rwidget_set_user_handle(image, (void *)(intptr_t)(icon + 1));
    if (tip) {
        Rwidget_set_tooltip(button, tip);
        Rwidget_set_accessible_name(button, tip);
    }
    button_common(button, cb, ud);
    return button;
}

void comp_icon_button_set(Rid button, Icon icon, const char *tip)
{
    Rid image = Rwidget_get_child(button, 0);
    if (image != R_INVALID_ID && (intptr_t)Rwidget_get_user_handle(image) != (intptr_t)(icon + 1)) {
        icon_set(image, icon);
        Rwidget_set_user_handle(image, (void *)(intptr_t)(icon + 1));
    }
    if (tip && strcmp(Rwidget_get_tooltip(button), tip) != 0) {
        Rwidget_set_tooltip(button, tip);
        Rwidget_set_accessible_name(button, tip);
    }
}

void comp_icon_button_set_on(Rid button, int on)
{
    comp_set_class(button, "on", on);
}

/* comp_segmented: the callback and the choice shown, kept on the capsule. */
typedef struct {
    CompChoiceFn cb;
    void *ud;
    int selected;
} Choice;

static void choice_show(Rid segmented, int selected)
{
    Choice *c = Rwidget_get_user_handle(segmented);
    Rid row = Rwidget_get_child(segmented, 0);
    if (!c || row == R_INVALID_ID)
        return;
    c->selected = selected;
    for (int i = 0, n = Rwidget_get_child_count(row); i < n; i++)
        Rbutton_set_style(Rwidget_get_child(row, i), i == selected ? R_BUTTON_PRIMARY : R_BUTTON_FLAT);
}

static void choice_clicked(Rid button, void *ud)
{
    Rid segmented = (Rid)(uintptr_t)ud;
    Choice *c = Rwidget_get_user_handle(segmented);
    Rid row = Rwidget_get_parent(button);
    if (!c)
        return;
    for (int i = 0, n = Rwidget_get_child_count(row); i < n; i++) {
        if (Rwidget_get_child(row, i) != button)
            continue;
        if (i != c->selected) {
            choice_show(segmented, i);
            if (c->cb)
                c->cb(i, c->ud);
        }
        return;
    }
}

static void choice_destroyed(Rid widget, void *ud)
{
    (void)widget;
    free(ud);
}

Rid comp_segmented(Rid parent, const char *const *labels, int count, int selected, CompChoiceFn cb, void *ud)
{
    Choice *c = calloc(1, sizeof *c);
    Rid capsule = add(parent, Rwidget_create(R_WIDGET_BORDER));
    Rborder_set_material(capsule, R_MATERIAL_FROSTED_GLASS);
    Rborder_set_corner_radius(capsule, 999.0f, 999.0f, 999.0f, 999.0f);
    Rborder_set_padding(capsule, 3.0f, 3.0f, 3.0f, 3.0f);
    Rid row = comp_hbox(capsule, 2.0f);
    for (int i = 0; i < count; i++) {
        Rid button = Rbutton_create(labels[i]);
        Rwidget_add_child(row, button);
        Rwidget_add_class(button, "round");
        Rslot_set_fill(button, 1.0f);
        button_common(button, choice_clicked, (void *)(uintptr_t)capsule);
    }
    if (c) {
        c->cb = cb;
        c->ud = ud;
        Rwidget_set_user_handle(capsule, c);
        Rwidget_set_destroy_callback(capsule, choice_destroyed, c);
    }
    choice_show(capsule, selected);
    return capsule;
}

void comp_segmented_set(Rid segmented, int selected)
{
    Choice *c = Rwidget_get_user_handle(segmented);
    if (c && c->selected != selected)
        choice_show(segmented, selected);
}

/* ---- rows ----------------------------------------------------------------------- */

CompRow comp_row(Rid parent, const char *title, const char *detail, RButtonClickedCallback cb, void *ud)
{
    CompRow r;
    r.row = add(parent, Rwidget_create(R_WIDGET_BUTTON));
    Rbutton_set_style(r.row, R_BUTTON_FLAT);
    Rwidget_add_class(r.row, "row");
    Rwidget_set_metric(r.row, R_METRIC_PADDING_X, 10.0f);
    Rwidget_set_metric(r.row, R_METRIC_PADDING_Y, 6.0f);
    button_common(r.row, cb, ud);
    Rid line = comp_hbox(r.row, 10.0f);
    r.lead = icon_image(line, ICON_NOTE, 20.0f);
    Rslot_set_valign(r.lead, R_VALIGN_CENTER);
    comp_set_visible(r.lead, 0);
    Rid texts = comp_vbox(line, 1.0f);
    Rslot_set_fill(texts, 1.0f);
    Rslot_set_valign(texts, R_VALIGN_CENTER);
    r.title = comp_text(texts, title, TEXT_BODY);
    r.detail = comp_text(texts, detail, TEXT_CAPTION);
    Rwidget_add_class(r.detail, "muted");
    comp_set_visible(r.detail, detail && detail[0]);
    r.info = comp_text(line, "", TEXT_CAPTION);
    Rwidget_add_class(r.info, "muted");
    Rslot_set_valign(r.info, R_VALIGN_CENTER);
    comp_set_visible(r.info, 0);
    r.action = R_INVALID_ID;
    return r;
}

void comp_row_set(const CompRow *r, const char *title, const char *detail, const char *info)
{
    comp_text_set(r->title, title);
    comp_text_set(r->detail, detail);
    comp_set_visible(r->detail, detail && detail[0]);
    comp_text_set(r->info, info);
    comp_set_visible(r->info, info && info[0]);
}

void comp_row_icon(const CompRow *r, Icon icon)
{
    if ((intptr_t)Rwidget_get_user_handle(r->lead) != (intptr_t)(icon + 1)) {
        icon_set(r->lead, icon);
        Rwidget_set_user_handle(r->lead, (void *)(intptr_t)(icon + 1));
    }
    comp_set_visible(r->lead, 1);
}

Rid comp_row_action(CompRow *r, Icon icon, const char *tip, RButtonClickedCallback cb, void *ud)
{
    if (r->action == R_INVALID_ID) {
        r->action = comp_icon_button(Rwidget_get_child(r->row, 0), icon, ICON_BUTTON_SMALL, tip, cb, ud);
        Rslot_set_valign(r->action, R_VALIGN_CENTER);
    }
    return r->action;
}

void comp_row_set_current(const CompRow *r, int current)
{
    comp_set_class(r->title, "current", current);
}

/* ---- accordion -------------------------------------------------------------------- */

CompSection comp_section(Rid parent, const char *title, int open, RExpanderChangedCallback cb, void *ud)
{
    CompSection s;
    s.expander = add(parent, Rwidget_create(R_WIDGET_EXPANDER));
    Rexpander_set_title(s.expander, title ? title : "");
    Rexpander_set_expanded(s.expander, open);
    Rwidget_set_focusable(s.expander, R_FALSE);
    if (cb)
        Rexpander_set_changed_callback(s.expander, cb, ud);
    s.body = comp_vbox(s.expander, 2.0f);
    Rslot_set_padding(s.body, 0.0f, 4.0f, 0.0f, 4.0f);
    return s;
}

void comp_section_set_title(const CompSection *s, const char *title)
{
    if (!title)
        title = "";
    if (strcmp(Rexpander_get_title(s->expander), title) != 0)
        Rexpander_set_title(s->expander, title);
}

void comp_section_set_open(const CompSection *s, int open)
{
    if (!Rexpander_is_expanded(s->expander) != !open)
        Rexpander_set_expanded(s->expander, open);
}

/* ---- fields --------------------------------------------------------------------------- */

static void field_common(Rid input, const char *placeholder, RTextChangedCallback on_change,
                         RTextChangedCallback on_commit, void *ud)
{
    Rtext_input_set_placeholder(input, placeholder ? placeholder : "");
    if (on_change)
        Rtext_input_set_changed_callback(input, on_change, ud);
    if (on_commit)
        Rtext_input_set_committed_callback(input, on_commit, ud);
}

Rid comp_field(Rid parent, const char *placeholder, int secret, RTextChangedCallback on_change,
               RTextChangedCallback on_commit, void *ud)
{
    Rid input = add(parent, Rwidget_create(R_WIDGET_TEXT_INPUT));
    Rtext_input_set_password(input, secret);
    field_common(input, placeholder, on_change, on_commit, ud);
    return input;
}

Rid comp_search_field(Rid parent, const char *placeholder, RTextChangedCallback on_change,
                      RTextChangedCallback on_commit, void *ud)
{
    Rid capsule = add(parent, Rwidget_create(R_WIDGET_BORDER));
    Rborder_set_material(capsule, R_MATERIAL_FROSTED_GLASS);
    Rborder_set_corner_radius(capsule, 999.0f, 999.0f, 999.0f, 999.0f);
    Rborder_set_padding(capsule, 12.0f, 0.0f, 6.0f, 0.0f);
    Rid row = comp_hbox(capsule, 6.0f);
    Rid glass = icon_image(row, ICON_SEARCH, 16.0f);
    Rwidget_add_class(glass, "muted");
    Rslot_set_valign(glass, R_VALIGN_CENTER);
    Rid input = add(row, Rwidget_create(R_WIDGET_TEXT_INPUT));
    Rwidget_add_class(input, "bare");
    Rslot_set_fill(input, 1.0f);
    Rslot_set_valign(input, R_VALIGN_CENTER);
    field_common(input, placeholder, on_change, on_commit, ud);
    return input;
}

void comp_field_set(Rid field, const char *text)
{
    Rtext_input_set_text(field, text ? text : ""); /* Raven compares */
}

/* ---- switch ------------------------------------------------------------------------------ */

Rid comp_switch(Rid parent, const char *text, int on, RCheckboxChangedCallback cb, void *ud)
{
    Rid toggle = add(parent, Rcheckbox_create(text ? text : ""));
    Rcheckbox_set_style(toggle, R_CHECKBOX_SWITCH);
    Rcheckbox_set_checked(toggle, on);
    Rwidget_set_focusable(toggle, R_FALSE);
    if (cb)
        Rcheckbox_set_changed_callback(toggle, cb, ud);
    return toggle;
}

void comp_switch_set(Rid toggle, int on)
{
    if (!Rcheckbox_is_checked(toggle) != !on)
        Rcheckbox_set_checked(toggle, on);
}

/* ---- slider -------------------------------------------------------------------------------- */

/* Raven reports every move of a drag and nothing when it ends: the press
 * and release are watched here, so a seek is sent once, when let go. */
typedef struct {
    CompSliderFn change, commit;
    void *ud;
    int held;
} Slider;

static void slider_changed(Rid slider, float value, void *ud)
{
    Slider *s = ud;
    if (s->held) {
        if (s->change)
            s->change(slider, value, s->ud);
    } else if (s->commit) {
        s->commit(slider, value, s->ud);
    }
}

static int slider_event(Rid slider, const REvent *event, void *ud)
{
    Slider *s = ud;
    if (event->button != R_MOUSE_LEFT || !Rwidget_is_enabled(slider))
        return R_FALSE;
    if (event->type == R_EVENT_MOUSE_DOWN) {
        s->held = 1;
    } else if (event->type == R_EVENT_MOUSE_UP && s->held) {
        /* the drag set the last value already; the release only ends it */
        s->held = 0;
        if (s->commit)
            s->commit(slider, Rslider_get_value(slider), s->ud);
    }
    return R_FALSE; /* the slider still handles the event */
}

static void slider_destroyed(Rid widget, void *ud)
{
    (void)widget;
    free(ud);
}

Rid comp_slider(Rid parent, float min, float max, CompSliderFn on_change, CompSliderFn on_commit, void *ud)
{
    Rid slider = add(parent, Rwidget_create(R_WIDGET_SLIDER));
    Rslider_set_range(slider, min, max);
    Rwidget_set_focusable(slider, R_FALSE);
    Slider *s = calloc(1, sizeof *s);
    if (!s)
        return slider;
    s->change = on_change;
    s->commit = on_commit;
    s->ud = ud;
    Rwidget_set_user_handle(slider, s);
    Rslider_set_changed_callback(slider, slider_changed, s);
    Rwidget_set_event_callback(slider, slider_event, s);
    Rwidget_set_destroy_callback(slider, slider_destroyed, s);
    return slider;
}

void comp_slider_set(Rid slider, float value)
{
    const Slider *s = Rwidget_get_user_handle(slider);
    if (s && s->held)
        return;
    if (Rslider_get_value(slider) != value)
        Rslider_set_value(slider, value);
}

int comp_slider_held(Rid slider)
{
    const Slider *s = Rwidget_get_user_handle(slider);
    return s && s->held;
}

/* ---- status line ------------------------------------------------------------------------------ */

CompStatus comp_status(Rid parent, RButtonClickedCallback on_close, void *ud)
{
    CompStatus s;
    s.row = comp_hbox(parent, 6.0f);
    s.text = comp_label(s.row, "", TEXT_CAPTION);
    Rwidget_add_class(s.text, "muted");
    Rslot_set_fill(s.text, 1.0f);
    Rslot_set_valign(s.text, R_VALIGN_CENTER);
    s.close = comp_icon_button(s.row, ICON_CLOSE, ICON_BUTTON_SMALL, "Dismiss", on_close, ud);
    Rslot_set_valign(s.close, R_VALIGN_CENTER);
    comp_set_visible(s.close, 0);
    comp_set_visible(s.row, 0);
    return s;
}

void comp_status_set(const CompStatus *s, const char *text, int error, int closable)
{
    int shown = text && text[0];
    comp_set_visible(s->row, shown);
    if (!shown)
        return;
    comp_text_set(s->text, text);
    comp_set_class(s->text, "error", error);
    comp_set_class(s->text, "muted", !error);
    comp_set_visible(s->close, closable);
}

/* ---- sheet ----------------------------------------------------------------------------------- */

CompSheet comp_sheet(const char *title, float width)
{
    CompSheet s;
    s.root = comp_vbox(R_INVALID_ID, 14.0f);
    Rwidget_set_size(s.root, width, -1.0f);
    s.title = comp_text(s.root, title, TEXT_HEADLINE);
    s.body = comp_vbox(s.root, COMP_GAP);
    s.buttons = comp_hbox(s.root, COMP_GAP);
    comp_fill(s.buttons);
    return s;
}

void comp_sheet_open(const CompSheet *s, RDialogClosedCallback closed, void *ud)
{
    Rdialog_open(window_id(), s->root, closed, ud);
}

void comp_sheet_close(Rid inside, int result)
{
    Rdialog_close(inside, result);
}
