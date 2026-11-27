/*
 * components.h: the pieces every screen is built from, on Raven's retained
 * widgets, one place each. A screen never styles a widget by hand where a
 * component exists; a new kind of piece is added here, not in a screen.
 *
 * Conventions:
 *  - Builders add what they make to `parent` (unless it is R_INVALID_ID)
 *    and return it. Callbacks are Raven's own types; `ud` is passed back.
 *  - Setters compare before they touch a widget, so an update pass may call
 *    them with the same values on every frame: nothing is laid out or
 *    painted again unless something changed.
 *  - Buttons and sliders do not take the keyboard focus when clicked (as on
 *    macOS), so Space stays Play/Pause; text fields do.
 *  - Sizes are logical units at text scale 1; Raven scales text and control
 *    heights with the text size setting.
 *
 * Threading: UI thread only, like all of Raven.
 */
#ifndef SMP_GUI_COMPONENTS_H
#define SMP_GUI_COMPONENTS_H

#include <Raven.h>

#include "icons.h"
#include "theme.h"

/* ---- layout ----------------------------------------------------------------- */

#define COMP_GAP     8.0f  /* between the pieces of a group */
#define COMP_PAD     16.0f /* inside panels and cards */
#define COMP_RADIUS  20.0f /* glass panels */
#define COMP_MARGIN  8.0f  /* floating panels from the window's edges */

Rid comp_vbox(Rid parent, float spacing);
Rid comp_hbox(Rid parent, float spacing);
/* Empty space that takes what is left along an HBox or a VBox. */
Rid comp_fill(Rid parent);
/* A vertical scroll area; returns the VBox inside it, where the content
 * goes. The ScrollBox is its parent (Rwidget_get_parent), e.g. for
 * Slot.Fill or Rscroll_box_set_offset. */
Rid comp_scroll(Rid parent, float spacing);

/* ---- text ------------------------------------------------------------------- */

/* A paragraph: a Raven Label in a text role that wraps to the width it is
 * given. */
Rid comp_label(Rid parent, const char *text, TextRole role);

/* One line cut with an ellipsis to the width it gets (track, artist and
 * device names). Its width comes from its parent: as the child of a VBox it
 * takes the VBox's width; in an HBox give it Rslot_set_fill. Its color
 * comes from its own classes ("muted", "current", "error"), not from the
 * widgets around it. */
Rid comp_text(Rid parent, const char *text, TextRole role);
/* comp_text and comp_label alike; NULL is "". */
void comp_text_set(Rid text, const char *s);
/* Where a comp_text line sits in its width (R_TEXT_ALIGN_LEFT by default). */
void comp_text_align(Rid text, RTextAlign align);

/* Adds or removes a class ("muted", "error", "on", "current"...) only when
 * that changes it. */
void comp_set_class(Rid widget, const char *cls, int on);
/* Shown, or collapsed (takes no room). */
void comp_set_visible(Rid widget, int visible);
void comp_set_enabled(Rid widget, int enabled);

/* ---- surfaces ----------------------------------------------------------------- */

typedef enum {
    PANEL_FROSTED, /* blurred glass: sidebars, cards */
    PANEL_CLEAR,   /* clear glass: over the album art */
    PANEL_TINTED   /* frosted, tinted with the accent: what must stand out */
} PanelKind;

/* A floating panel of glass with rounded corners and a soft shadow. Returns
 * its VBox (spacing COMP_GAP), where the content goes; the Border is its
 * parent. */
Rid comp_panel(Rid parent, PanelKind kind, float radius, float padding);

/* ---- headers ------------------------------------------------------------------ */

typedef struct {
    Rid row;   /* HBox */
    Rid title; /* comp_text, TEXT_HEADLINE */
    Rid link;  /* comp_button BUTTON_LINK at the end, or R_INVALID_ID */
} CompHeader;

/* A section title, with a link at its end ("See all") when link is not
 * NULL. */
CompHeader comp_header(Rid parent, const char *title, const char *link, RButtonClickedCallback cb,
                       void *ud);

/* ---- buttons ------------------------------------------------------------------ */

typedef enum {
    BUTTON_PRIMARY,   /* tinted with the accent: the one thing to do */
    BUTTON_SECONDARY, /* glass */
    BUTTON_LINK       /* text in the accent, no surface */
} ButtonKind;

Rid comp_button(Rid parent, const char *text, ButtonKind kind, RButtonClickedCallback cb, void *ud);
void comp_button_set_text(Rid button, const char *text);

typedef enum {
    ICON_BUTTON_SMALL,  /* 28, icon 16: in rows, dismiss */
    ICON_BUTTON_MEDIUM, /* 34, icon 20: toolbars, toggles, the sidebar footer */
    ICON_BUTTON_LARGE,  /* 52, icon 26, glass: previous / next */
    ICON_BUTTON_HERO    /* 68, icon 30, tinted glass: play / pause */
} IconButtonSize;

/* A round button showing an icon; tip is its tooltip and what VoiceOver
 * says. */
Rid comp_icon_button(Rid parent, Icon icon, IconButtonSize size, const char *tip, RButtonClickedCallback cb,
                     void *ud);
/* Another icon and tip (play / pause, heart / filled heart); tip NULL keeps it. */
void comp_icon_button_set(Rid button, Icon icon, const char *tip);
/* A toggle that is on (shuffle, repeat, liked): the icon in the accent. */
void comp_icon_button_set_on(Rid button, int on);

/* A row of choices, one of them selected (the text size, Library /
 * Devices). The callback gets the index of the choice clicked. */
typedef void (*CompChoiceFn)(int index, void *ud);
Rid comp_segmented(Rid parent, const char *const *labels, int count, int selected, CompChoiceFn cb, void *ud);
void comp_segmented_set(Rid segmented, int selected);

/* ---- rows ----------------------------------------------------------------------- */

typedef struct {
    Rid row;    /* the Button: the whole row clicks */
    Rid lead;   /* icon at the start (a device), collapsed until comp_row_icon */
    Rid title;  /* comp_text, body */
    Rid detail; /* comp_text, caption, muted; collapsed while empty */
    Rid info;   /* comp_text, caption, muted, at the end (a length, a count) */
    Rid action; /* trailing icon button (+Queue), or R_INVALID_ID */
} CompRow;

/* A list row: a title over a detail line, an optional info at the end. */
CompRow comp_row(Rid parent, const char *title, const char *detail, RButtonClickedCallback cb, void *ud);
void comp_row_set(const CompRow *row, const char *title, const char *detail, const char *info);
void comp_row_icon(const CompRow *row, Icon icon);
/* Adds the trailing icon button (once); returns it. */
Rid comp_row_action(CompRow *row, Icon icon, const char *tip, RButtonClickedCallback cb, void *ud);
/* The item playing now: its title in the accent. */
void comp_row_set_current(const CompRow *row, int current);

/* ---- accordion -------------------------------------------------------------------- */

typedef struct {
    Rid expander; /* the header: title, chevron */
    Rid body;     /* VBox: the rows go here */
} CompSection;

/* A section that opens and closes under its header (Liked Songs,
 * Playlists...). cb (may be NULL) gets the new state when the user toggles it. */
CompSection comp_section(Rid parent, const char *title, int open, RExpanderChangedCallback cb, void *ud);
void comp_section_set_title(const CompSection *section, const char *title);
void comp_section_set_open(const CompSection *section, int open);

/* ---- fields --------------------------------------------------------------------------- */

/* A one-line text input. secret hides what is typed. on_change runs on
 * every edit, on_commit on Enter (either may be NULL). */
Rid comp_field(Rid parent, const char *placeholder, int secret, RTextChangedCallback on_change,
               RTextChangedCallback on_commit, void *ud);
/* The same with a magnifier in front, for searching. Returns the input. */
Rid comp_search_field(Rid parent, const char *placeholder, RTextChangedCallback on_change,
                      RTextChangedCallback on_commit, void *ud);
void comp_field_set(Rid field, const char *text);

/* ---- switch ------------------------------------------------------------------------------ */

Rid comp_switch(Rid parent, const char *text, int on, RCheckboxChangedCallback cb, void *ud);
void comp_switch_set(Rid toggle, int on);

/* ---- slider -------------------------------------------------------------------------------- */

typedef void (*CompSliderFn)(Rid slider, float value, void *ud);

/* A slider from min to max. on_change follows a drag (may be NULL);
 * on_commit runs once it is let go, or for each key press: that is when a
 * seek or a volume is sent. */
Rid comp_slider(Rid parent, float min, float max, CompSliderFn on_change, CompSliderFn on_commit, void *ud);
/* Moves it, unless the user holds it. */
void comp_slider_set(Rid slider, float value);
/* 1 while the user drags it. */
int comp_slider_held(Rid slider);

/* ---- status line ------------------------------------------------------------------------------ */

typedef struct {
    Rid row;   /* HBox, collapsed while there is no text */
    Rid text;  /* comp_label, caption */
    Rid close; /* dismiss button, shown when closable */
} CompStatus;

/* A line of status under a part of the screen: an error in red, a tip that
 * can be dismissed (on_close). */
CompStatus comp_status(Rid parent, RButtonClickedCallback on_close, void *ud);
/* text NULL or "" hides it. */
void comp_status_set(const CompStatus *status, const char *text, int error, int closable);

/* ---- sheet ----------------------------------------------------------------------------------- */

typedef struct {
    Rid root;    /* what Rdialog_open shows; Raven draws the glass sheet around it */
    Rid title;   /* comp_text, TEXT_HEADLINE */
    Rid body;    /* VBox: the content goes here */
    Rid buttons; /* HBox at the bottom, right-aligned: add comp_buttons */
} CompSheet;

/* A sheet over the window: a title, a body, buttons. Build its content,
 * then comp_sheet_open. The window owns it from then on and deletes it
 * when it closes (Escape closes it with result 0). */
CompSheet comp_sheet(const char *title, float width);
void comp_sheet_open(const CompSheet *sheet, RDialogClosedCallback closed, void *ud);
/* Closes the sheet holding `inside` (any widget of it) with `result`. */
void comp_sheet_close(Rid inside, int result);

#endif /* SMP_GUI_COMPONENTS_H */
