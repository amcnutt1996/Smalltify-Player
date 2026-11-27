/* player.c: the main area of the player page (player.h): the album art
 * under clear glass, the track and its heart, the seek bar with its times,
 * the transport with shuffle and repeat, the volume, the pairing tip, the
 * status line and, when Soloist is down, the way to restart it.
 *
 * Two arrangements, chosen by the room the main area has: stacked (the art
 * over the controls) or side by side (the controls beside the art) when
 * that makes the art clearly bigger, as when the sidebar slides away from a
 * wide window. The art is as large as the room allows either way.
 *
 * The state and the commands stay in ui.c; the handlers start the same
 * optimistic edits the player always made (pend_begin, then cmd_post), and
 * the update pass pushes only what changed. */
#include "screens/player.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gui/components.h"
#include "gui/shell.h"
#include "screens/player_seek.h"
#include "ui_internal.h"

#define ART_MIN   72.0f  /* less room than this: no art */
#define ART_MAX   720.0f
#define COL_MIN   320.0f /* the controls' narrowest: the transport fits */
#define SIDE_MAX  420.0f /* the controls beside the art: at most this wide */
#define GAP_STACK 18.0f  /* between the art and the controls under it */
#define GAP_SIDE  40.0f  /* between the art and the controls beside it */

static const char *const g_repeat_tip[] = { "Repeat: off", "Repeat: all", "Repeat: one" };

static struct {
    Rid stage, grid, controls;
    Rid art, note, img; /* art: the glass mat; note: the icon shown without art */
    Rid title, artists, album, heart;
    Rid elapsed, total, times;
    Rid shuffle, prev, play, next, repeat;
    Rid mute, volume;
    CompStatus tip, status;
    Rid soloist;

    /* what the widgets show */
    uint64_t art_gen;
    int      pos_sec, dur, can_seek; /* the elapsed second, the length, seeking possible */
    float    seek_t;                 /* the seek bar's value while dragged */
    float    vol_t;                  /* the volume slider's value while dragged */
    int      unmute;                 /* the volume a click on the speaker brings back */
    uint64_t note_until;             /* the "Added to queue" toasted last */
    char     status_text[sizeof ((Ui *)0)->status]; /* the status line shown */
    int      status_error;

    /* the arrangement */
    int   side; /* 1 side by side, 0 stacked, -1 not yet */
    int   art_shown;
    float art_size, col_w, mat, radius, note_size;
    int   big; /* the title in the large size */
    float inset; /* the times' padding: the seek track's ends */
} g;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---- what the controls do ------------------------------------------------------------- */

static void play_pause(Ui *u)
{
    uint64_t now = plat_now_ms();
    const PlaybackState *s = &u->st;
    int play = !(s->active && s->is_playing);
    u->pend.playing = play;
    u->pend.progress_ms = progress_at(s, now);
    u->pend.progress_at = now;
    pend_begin(u, PEND_PLAY, now);
    pend_reapply(u);
    cmd_post(&u->cmd, play ? CMD_PLAY : CMD_PAUSE, 0);
}

static void skip(Ui *u, CmdKind kind)
{
    if (!u->st.active)
        return;
    u->pend.mask &= ~PEND_SEEK; /* the next track starts where it starts */
    cmd_post(&u->cmd, kind, 0);
}

/* Save the track playing to Liked Songs, or take it out: shown at once, the
 * library worker's answer confirms it or puts it back (heart_sync). */
static void like(Ui *u)
{
    if (!likeable(&u->st) || !u->heart_uri[0])
        return;
    u->heart = u->heart != 1;
    lib_post_save(&u->libw, u->heart_uri, u->heart, ++u->like_sent);
}

static void set_volume(Ui *u, int volume)
{
    uint64_t now = plat_now_ms();
    u->pend.volume = volume;
    pend_begin(u, PEND_VOLUME, now);
    pend_reapply(u);
    cmd_post(&u->cmd, CMD_VOLUME, volume);
}

static void on_play(Rid button, void *ud)
{
    (void)button;
    play_pause(ud);
}

static void on_previous(Rid button, void *ud)
{
    (void)button;
    skip(ud, CMD_PREV);
}

static void on_next(Rid button, void *ud)
{
    (void)button;
    skip(ud, CMD_NEXT);
}

static void on_heart(Rid button, void *ud)
{
    (void)button;
    like(ud);
}

static void on_shuffle(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    if (!u->st.active)
        return;
    uint64_t now = plat_now_ms();
    u->pend.shuffle = !u->st.shuffle;
    pend_begin(u, PEND_SHUFFLE, now);
    pend_reapply(u);
    cmd_post(&u->cmd, CMD_SHUFFLE, u->pend.shuffle);
}

/* Off, all (the context), one (the track), and round again. */
static void on_repeat(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    if (!u->st.active)
        return;
    uint64_t now = plat_now_ms();
    unsigned rep = (unsigned)u->st.repeat <= REPEAT_TRACK ? (unsigned)u->st.repeat : 0;
    u->pend.repeat = (RepeatMode)((rep + 1) % 3);
    pend_begin(u, PEND_REPEAT, now);
    pend_reapply(u);
    cmd_post(&u->cmd, CMD_REPEAT, (int)u->pend.repeat);
}

static void on_seek_change(Rid slider, float value, void *ud)
{
    (void)slider;
    (void)ud;
    g.seek_t = value; /* the elapsed time follows the drag */
}

static void on_seek_commit(Rid slider, float value, void *ud)
{
    (void)slider;
    Ui *u = ud;
    int dur = u->st.track.duration_ms;
    if (!u->st.active || dur <= 0)
        return;
    uint64_t now = plat_now_ms();
    u->pend.progress_ms = (int)(value * (float)dur);
    u->pend.progress_at = now;
    pend_begin(u, PEND_SEEK, now);
    pend_reapply(u);
    cmd_post(&u->cmd, CMD_SEEK, u->pend.progress_ms);
}

static void on_volume_change(Rid slider, float value, void *ud)
{
    (void)slider;
    (void)ud;
    g.vol_t = value; /* the speaker follows the drag */
}

static void on_volume_commit(Rid slider, float value, void *ud)
{
    (void)slider;
    Ui *u = ud;
    if (u->st.active)
        set_volume(u, (int)(value * 100.0f + 0.5f));
}

/* The speaker: mute, or back to the volume before. */
static void on_mute(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    const PlaybackState *s = &u->st;
    if (!s->active || s->volume < 0)
        return;
    set_volume(u, s->volume > 0 ? 0 : g.unmute > 0 ? g.unmute : 50);
}

static void on_tip_close(Rid button, void *ud)
{
    (void)button;
    ((Ui *)ud)->hint_hidden = 1; /* for this run; Soloist stays discoverable */
}

/* Soloist stopped or failed while paired: only on request, as an automatic
 * restart could loop forever. A key fixed in .env takes effect. */
static void on_restart(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    reload_key(u);
    start_engine(u, 0);
}

static void on_change_key(Rid button, void *ud)
{
    (void)button;
    Ui *u = ud;
    u->setup_msg = NULL;
    set_screen(u, SCR_KEY);
}

/* The Playback menu and its shortcuts (Space, Cmd+Left, Cmd+Right). */
static void on_act_play_pause(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    Ui *u = ud;
    if (u->screen == SCR_PLAYER)
        play_pause(u);
}

static void on_act_previous(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    Ui *u = ud;
    if (u->screen == SCR_PLAYER)
        skip(u, CMD_PREV);
}

static void on_act_next(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    Ui *u = ud;
    if (u->screen == SCR_PLAYER)
        skip(u, CMD_NEXT);
}

static void on_act_like(Rid sender, const char *action, void *ud)
{
    (void)sender;
    (void)action;
    Ui *u = ud;
    if (u->screen == SCR_PLAYER)
        like(u);
}

/* ---- building --------------------------------------------------------------------------- */

static void center(Rid widget)
{
    Rslot_set_halign(widget, R_HALIGN_CENTER);
    Rslot_set_valign(widget, R_VALIGN_CENTER);
}

/* The art on a mat of glass, which catches the light at its rim around the
 * picture; without art, the glass shows a note. (Glass over the picture
 * would soften it: the art stays sharp on top.) */
static void build_art(Rid parent)
{
    Rid panel = comp_panel(parent, PANEL_CLEAR, 16.0f, 8.0f);
    g.art = Rwidget_get_parent(panel);
    Rslot_set_grid(g.art, 0, 0);
    center(g.art);
    Rid stack = Rwidget_add_child(panel, Rwidget_create(R_WIDGET_OVERLAY));
    Rslot_set_fill(stack, 1.0f);
    g.note = icon_image(stack, ICON_NOTE, 64.0f);
    Rwidget_add_class(g.note, "muted");
    center(g.note);
    g.img = Rwidget_add_child(stack, Rwidget_create(R_WIDGET_IMAGE));
    Rimage_set_stretch(g.img, R_STRETCH_COVER);
    Rwidget_set_hit_testable(g.img, R_FALSE);
    Rwidget_set_accessible_hidden(g.img, R_TRUE);
    comp_set_visible(g.img, 0);
}

static void build_controls(Ui *u, Rid parent)
{
    g.controls = comp_vbox(parent, 10.0f);
    Rslot_set_grid(g.controls, 0, 1);
    center(g.controls);

    /* the track, and its heart */
    Rid info = comp_hbox(g.controls, 12.0f);
    Rid texts = comp_vbox(info, 2.0f);
    Rslot_set_fill(texts, 1.0f);
    g.title = comp_text(texts, "", TEXT_TITLE);
    g.artists = comp_text(texts, "", TEXT_BODY);
    g.album = comp_text(texts, "", TEXT_BODY);
    Rwidget_add_class(g.album, "muted");
    g.heart = comp_icon_button(info, ICON_HEART, ICON_BUTTON_MEDIUM, "Save to Liked Songs", on_heart, u);
    Rslot_set_valign(g.heart, R_VALIGN_CENTER);

    /* where it is: the seek bar, the elapsed time under its start and the
     * length under its end */
    Rid seek = comp_vbox(g.controls, 0.0f);
    seek_build(seek, on_seek_change, on_seek_commit, u);
    g.times = comp_hbox(seek, 0.0f);
    g.elapsed = comp_text(g.times, "0:00", TEXT_CAPTION);
    Rwidget_add_class(g.elapsed, "muted");
    comp_fill(g.times);
    g.total = comp_text(g.times, "0:00", TEXT_CAPTION);
    Rwidget_add_class(g.total, "muted");

    /* the transport, with shuffle and repeat at its ends */
    Rid row = comp_hbox(g.controls, 14.0f);
    Rslot_set_halign(row, R_HALIGN_CENTER);
    g.shuffle = comp_icon_button(row, ICON_SHUFFLE, ICON_BUTTON_MEDIUM, "Shuffle: off", on_shuffle, u);
    g.prev = comp_icon_button(row, ICON_PREVIOUS, ICON_BUTTON_LARGE, "Previous", on_previous, u);
    g.play = comp_icon_button(row, ICON_PLAY, ICON_BUTTON_HERO, "Play", on_play, u);
    g.next = comp_icon_button(row, ICON_NEXT, ICON_BUTTON_LARGE, "Next", on_next, u);
    g.repeat = comp_icon_button(row, ICON_REPEAT, ICON_BUTTON_MEDIUM, g_repeat_tip[0], on_repeat, u);
    Rslot_set_padding(g.prev, 6.0f, 0.0f, 0.0f, 0.0f);
    Rslot_set_padding(g.next, 0.0f, 0.0f, 6.0f, 0.0f);
    Rid transport[] = { g.shuffle, g.prev, g.play, g.next, g.repeat };
    for (size_t i = 0; i < sizeof transport / sizeof transport[0]; i++)
        Rslot_set_valign(transport[i], R_VALIGN_CENTER);

    /* the volume */
    Rid volume = comp_hbox(g.controls, 2.0f);
    g.mute = comp_icon_button(volume, ICON_VOLUME, ICON_BUTTON_SMALL, "Mute", on_mute, u);
    Rslot_set_valign(g.mute, R_VALIGN_CENTER);
    g.volume = comp_slider(volume, 0.0f, 1.0f, on_volume_change, on_volume_commit, u);
    Rwidget_set_accessible_name(g.volume, "Volume");
    Rslot_set_fill(g.volume, 1.0f);
    Rslot_set_valign(g.volume, R_VALIGN_CENTER);

    /* the pairing tip, then the status line */
    g.tip = comp_status(g.controls, on_tip_close, u);
    g.status = comp_status(g.controls, NULL, NULL);
    g.soloist = comp_hbox(g.controls, COMP_GAP);
    Rslot_set_halign(g.soloist, R_HALIGN_CENTER);
    comp_button(g.soloist, "Restart Soloist", BUTTON_PRIMARY, on_restart, u);
    comp_button(g.soloist, "Change key", BUTTON_SECONDARY, on_change_key, u);
    comp_set_visible(g.soloist, 0);
}

void player_build(Ui *u, Rid parent)
{
    memset(&g, 0, sizeof g);
    g.art_gen = (uint64_t)-1;
    g.pos_sec = g.dur = g.can_seek = -1;
    g.side = g.art_shown = -1;
    g.stage = Rwidget_add_child(parent, Rwidget_create(R_WIDGET_OVERLAY));
    Rslot_set_fill(g.stage, 1.0f);
    g.grid = Rwidget_add_child(g.stage, Rwidget_create(R_WIDGET_GRID));
    center(g.grid);
    Rpanel_set_spacing(g.grid, GAP_STACK);
    build_art(g.grid);
    build_controls(u, g.grid);

    Raction_bind(ACT_PLAY_PAUSE, on_act_play_pause, u);
    Raction_bind(ACT_PREVIOUS, on_act_previous, u);
    Raction_bind(ACT_NEXT, on_act_next, u);
    Raction_bind(ACT_LIKE, on_act_like, u);
}

/* ---- the arrangement -------------------------------------------------------------------------- */

/* Puts what arrange() chose; returns 1 if anything moved. */
static int set_arrangement(int side, int shown, float art, float col)
{
    int changed = 0;
    if (side != g.side || shown != g.art_shown) {
        int first = g.side < 0;
        if (side != g.side && !first)
            Rwidget_play_transition(g.controls, R_TRANSITION_FADE, 0.0f);
        g.side = side;
        g.art_shown = shown;
        comp_set_visible(g.art, shown);
        Rslot_set_grid(g.controls, side ? 1 : 0, side || !shown ? 0 : 1);
        Rpanel_set_spacing(g.grid, side ? GAP_SIDE : GAP_STACK);
        changed = 1;
    }
    if (shown && art != g.art_size) {
        g.art_size = art;
        Rwidget_set_size(g.art, art, art);
        /* the mat's margin and corners grow with the art; its corners are
         * the picture's, rounded by the margin */
        float mat = roundf(clampf(art * 0.028f, 6.0f, 14.0f));
        float radius = roundf(clampf((art - 2.0f * mat) * 0.04f, 8.0f, 18.0f)), note = roundf(art * 0.3f);
        if (mat != g.mat || radius != g.radius) {
            g.mat = mat;
            g.radius = radius;
            Rborder_set_padding(g.art, mat, mat, mat, mat);
            Rborder_set_corner_radius(g.art, radius + mat, radius + mat, radius + mat, radius + mat);
            Rimage_set_corner_radius(g.img, radius);
        }
        if (note != g.note_size) {
            g.note_size = note;
            Rwidget_set_size(g.note, note, note);
        }
        changed = 1;
    }
    if (col != g.col_w) {
        g.col_w = col;
        Rwidget_set_size(g.controls, col, -1.0f);
        changed = 1;
    }
    /* a big picture gets a big title (with a margin, as the title's height
     * changes the room the art has) */
    int big = shown && (g.big == 1 ? art >= 420.0f : art >= 460.0f);
    if (big != g.big) {
        g.big = big;
        comp_set_class(g.title, "title", !big);
        comp_set_class(g.title, "large-title", big);
        changed = 1;
    }
    return changed;
}

/* The art as large as the room allows: under it the controls (stacked), or
 * beside it when the art is clearly bigger that way. The room is the
 * stage's, and the controls' height theirs, after the layout of what
 * changed in this pass (Rwidget_get_rect lays out first), so a resize or
 * the sidebar's slide is followed in the same frame. (Not
 * Rwidget_get_desired_size: called here, with the wrapping Label of the
 * status line inside, it kept the window laying out and painting on every
 * frame at a text scale other than 1.) Returns 1 if it changed the
 * arrangement. */
static int arrange(void)
{
    float x, y, w, h, cw, ch;
    Rwidget_get_rect(g.stage, &x, &y, &w, &h);
    if (w <= 0.0f || h <= 0.0f)
        return 0;
    Rwidget_get_rect(g.controls, &x, &y, &cw, &ch);
    /* the controls' narrowest follows the text size; the window can come
     * down to that, with no art (shell_set_main_min) */
    float col_min = ceilf(COL_MIN * Raccessibility_get_text_scale());
    shell_set_main_min(col_min + 24.0f, ceilf(ch) + 24.0f);
    float pad = clampf(fminf(w, h) * 0.035f, 12.0f, 28.0f);
    float aw = fmaxf(w - 2.0f * pad, 0.0f), ah = fmaxf(h - 2.0f * pad, 0.0f);
    float side_w = clampf(aw * 0.38f, col_min, fmaxf(SIDE_MAX, col_min));
    float stacked = fminf(fminf(aw, ah - ch - GAP_STACK), ART_MAX);
    float beside = fminf(fminf(ah, aw - side_w - GAP_SIDE), ART_MAX);
    int side = g.side == 1;
    /* a margin each way, so that a size near the edge does not flap; the
     * default window stays stacked */
    if (!side && beside >= ART_MIN && beside > stacked * 1.4f && ch <= ah)
        side = 1;
    else if (side && (beside < ART_MIN || beside < stacked * 1.25f))
        side = 0;
    float art = floorf(side ? beside : stacked);
    int shown = art >= ART_MIN;
    float col = side ? floorf(side_w) : floorf(fminf(fmaxf(shown ? art : 0.0f, col_min), aw));
    return set_arrangement(side, shown, shown ? art : g.art_size, col);
}

/* ---- the update pass ---------------------------------------------------------------------------- */

static void update_track(Ui *u)
{
    const PlaybackState *s = &u->st;
    if (s->active) {
        comp_text_set(g.title, s->track.name[0] ? s->track.name : "Unknown title");
        comp_text_set(g.artists, s->track.artists);
        comp_text_set(g.album, s->track.album);
    } else {
        comp_text_set(g.title, "Nothing playing");
        comp_text_set(g.artists, "Press play, or pick a device in Devices.");
        comp_text_set(g.album, "");
    }
    comp_set_visible(g.album, s->active && s->track.album[0]);

    int likes = likeable(s), liked = likes && u->heart == 1;
    comp_set_visible(g.heart, likes);
    comp_icon_button_set(g.heart, liked ? ICON_HEART_FILLED : ICON_HEART,
                         liked ? "Remove from Liked Songs" : "Save to Liked Songs");
    comp_icon_button_set_on(g.heart, liked);
    shell_menu_set_enabled(SHELL_MENU_LIKE, likes);
    shell_menu_set_checked(SHELL_MENU_LIKE, liked);

    /* the image ui.c made; the one before is deleted after this pass */
    if (u->art_gen != g.art_gen) {
        g.art_gen = u->art_gen;
        int has = u->art_tex != R_INVALID_ID;
        Rimage_set_texture(g.img, u->art_tex);
        comp_set_visible(g.img, has);
        comp_set_visible(g.note, !has);
    }
}

static void update_position(Ui *u, uint64_t now)
{
    const PlaybackState *s = &u->st;
    int dur = s->track.duration_ms, can_seek = s->active && dur > 0;
    int held = seek_held();
    seek_set_enabled(can_seek);
    int pos = !can_seek ? 0 : held ? (int)(g.seek_t * (float)dur) : progress_at(s, now);
    /* once per second of playback: the time and the bar */
    if (pos / 1000 != g.pos_sec || dur != g.dur || can_seek != g.can_seek) {
        char t[16];
        g.pos_sec = pos / 1000;
        g.can_seek = can_seek;
        sp_fmt_time(t, sizeof t, pos);
        comp_text_set(g.elapsed, t);
        if (!held)
            seek_set(can_seek ? (float)pos / (float)dur : 0.0f);
    }
    if (dur != g.dur) {
        char t[16];
        g.dur = dur;
        sp_fmt_time(t, sizeof t, s->active ? dur : 0);
        comp_text_set(g.total, t);
    }
    /* the times under the ends of the track, which move in with the knob's
     * size as the text grows */
    float inset = seek_inset();
    if (inset != g.inset) {
        g.inset = inset;
        Rslot_set_padding(g.times, inset, 0.0f, inset, 0.0f);
    }
}

static void update_transport(Ui *u)
{
    const PlaybackState *s = &u->st;
    int active = s->active, playing = active && s->is_playing;
    comp_icon_button_set(g.play, playing ? ICON_PAUSE : ICON_PLAY, playing ? "Pause" : "Play");
    shell_menu_set_text(SHELL_MENU_PLAY_PAUSE, playing ? "Pause" : "Play");
    /* nothing playing: only Play can do something (it starts a device) */
    comp_set_enabled(g.prev, active);
    comp_set_enabled(g.next, active);
    comp_set_enabled(g.shuffle, active);
    comp_set_enabled(g.repeat, active);
    shell_menu_set_enabled(SHELL_MENU_PREVIOUS, active);
    shell_menu_set_enabled(SHELL_MENU_NEXT, active);

    int shuffle = active && s->shuffle;
    comp_icon_button_set(g.shuffle, ICON_SHUFFLE, shuffle ? "Shuffle: on" : "Shuffle: off");
    comp_icon_button_set_on(g.shuffle, shuffle);
    unsigned rep = active && (unsigned)s->repeat <= REPEAT_TRACK ? (unsigned)s->repeat : 0;
    comp_icon_button_set(g.repeat, rep == REPEAT_TRACK ? ICON_REPEAT_ONE : ICON_REPEAT, g_repeat_tip[rep]);
    comp_icon_button_set_on(g.repeat, rep != REPEAT_OFF);

    int can = active && s->volume >= 0;
    if (s->volume > 0)
        g.unmute = s->volume;
    comp_set_enabled(g.volume, can);
    comp_set_enabled(g.mute, can);
    comp_slider_set(g.volume, can ? (float)s->volume / 100.0f : 0.0f); /* not while held */
    float shown = comp_slider_held(g.volume) ? g.vol_t : can ? (float)s->volume / 100.0f : 0.0f;
    int muted = can && shown < 0.005f;
    comp_icon_button_set(g.mute, muted ? ICON_VOLUME_OFF : ICON_VOLUME, muted ? "Unmute" : "Mute");
}

static void update_status(Ui *u, uint64_t now)
{
    /* "Added to queue" rises as a toast; the status line keeps what it said */
    int note = now < u->note_until;
    if (u->note_until != g.note_until) {
        g.note_until = u->note_until;
        if (note)
            shell_toast("Added to queue", 0);
    }
    if (!note || strcmp(u->status, "Added to queue") != 0) {
        memcpy(g.status_text, u->status, sizeof g.status_text);
        g.status_error = u->status_error;
    }
    comp_status_set(&g.status, g.status_text, g.status_error, 0);
    comp_status_set(&g.tip, u->hint, 0, 1);
    /* paired, but Soloist is down (stopped, failed to start or update) */
    comp_set_visible(g.soloist, u->paired && !u->sol && !u->inst.th && u->eng_err[0]);
}

void player_update(Ui *u)
{
    uint64_t now = plat_now_ms();
    seek_update();
    update_track(u);
    update_position(u, now);
    update_transport(u);
    update_status(u, now);
    for (int i = 0; i < 3 && arrange(); i++) {
    }
}
