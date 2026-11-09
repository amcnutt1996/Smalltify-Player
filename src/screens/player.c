/* The player screen: the art, the track, the seek and volume bars, the
 * transport, the modes, the status line and the bottom bar. The Library,
 * Devices and Settings panels draw over its area. Drawing and layout only;
 * the state and the engine stay in ui.c. */
#include "screens/player.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "../ui_internal.h"
#include "gui/window.h"
#include "screens/common.h"
#include "screens/library.h"
#include "screens/panels.h"

/* Player rows below the art, as offsets from its bottom edge. */
typedef struct {
    float title, artists, album, seek, times, transport, modes, volume, end;
} PlayerRows;

static PlayerRows player_rows(const Metrics *m)
{
    PlayerRows r;
    r.title = 14;
    r.artists = r.title + (float)m->fl + 6;
    r.album = r.artists + m->line;
    r.seek = r.album + (float)m->fs + 12;
    r.times = r.seek + 12;
    r.transport = r.times + (float)m->fs + 12;
    r.modes = r.transport + 56;
    r.volume = r.modes + m->row + 12; /* the "Vol" label; its bar is centered on it */
    r.end = r.volume + (float)m->fs;
    return r;
}

/* Two lines of body text: the status, or the pairing tip. */
static float two_lines(const Metrics *m)
{
    return 2.0f * (float)m->fs + LINE_GAP;
}

/* The heart's square at the end of the title's line. */
float heart_side(const Metrics *m)
{
    return (float)m->fl + 6;
}

/* The player's height without the art: padding, the rows below the art,
 * the status lines and the bottom bar. */
float player_reserve(const Metrics *m)
{
    return PAD + player_rows(m).end + 4 + two_lines(m) + 4 + m->bar + PAD;
}

/* New image bytes: decode and upload once; frames then only draw the
 * texture, which the GPU shrinks to the slot. Empty bytes clear the slot. */
static void update_art(Ui *u)
{
    const unsigned char *data;
    size_t len;
    if (!art_take(u->art, &data, &len, &u->art_seq))
        return;
    if (Rtexture_is_valid(u->art_tex))
        Rtexture_destroy(u->art_tex);
    u->art_tex = R_INVALID_ID;
    int jpg = len > 2 && data[0] == 0xFF && data[1] == 0xD8;
    int png = len > 4 && !memcmp(data, "\x89PNG", 4);
    if (!jpg && !png)
        return;
    u->art_tex = Rtexture_create((const char *)data, len);
}

void draw_player(Ui *u)
{
    uint64_t now = plat_now_ms();
    PlaybackState snap;

    /* Only minimized counts as background: a visible window, focused or
     * not, keeps its 1 Hz progress tick. */
    int bg = window_is_minimized();
    if (bg != u->background) {
        u->background = bg;
        poller_set_background(u->poller, bg);
    }
    u->dev.slow = bg;

    if (poller_snapshot(u->poller, &snap, &u->seq)) {
        apply_snapshot(u, &snap, now);
        art_request(u->art, snap.track.art_url); /* no-op unless the URL changed */
        if (snap.active)
            note_other(u, snap.device_name);
    }
    if (u->pend.mask && now >= u->pend.until)
        resync(u);

    plat_mutex_lock(u->cmd.mu);
    uint64_t cseq = u->cmd.err_seq, done_at = u->cmd.done_at;
    int cerr = u->cmd.err, queued = u->cmd.done == CMD_QUEUE;
    char played_on[SP_NAME_LEN], other[SP_NAME_LEN] = "";
    memcpy(played_on, u->cmd.played_on, sizeof played_on);
    int fresh = u->cmd.devs_seq != u->dev_seq_noted;
    u->dev_seq_noted = u->cmd.devs_seq;
    if (fresh) /* a fresh device list names another device? */
        for (int i = 0; i < u->cmd.devs.ndev && !other[0]; i++)
            if (u->cmd.devs.devs[i].id[0] && strcmp(u->cmd.devs.devs[i].name, u->cfg.device_name))
                memcpy(other, u->cmd.devs.devs[i].name, sizeof other);
    plat_mutex_unlock(u->cmd.mu);
    note_other(u, other);
    if (cseq != u->cmd_seq) {
        u->cmd_seq = cseq;
        u->cmd_err = cerr;
        memcpy(u->played_on, played_on, sizeof u->played_on);
        /* The worker wakes us PENDING_MS after its last command, which
         * takes the note down again. */
        u->note_until = queued && cerr == SP_OK ? done_at + PENDING_MS : 0;
        u->text_dirty = 1;
        if (cerr != SP_OK)
            resync(u);
    }
    int perr = poller_last_error(u->poller);
    if (perr != u->poll_err) {
        u->poll_err = perr;
        u->text_dirty = 1;
    }
    /* auth.c forgets a revoked refresh token; only a new sign-in helps. */
    if ((perr == SP_ERR_AUTH || u->cmd_err == SP_ERR_AUTH) && !auth_store_logged_in(u->auth)) {
        stop_session(u);
        u->login_msg = "Session expired. Please sign in again.";
        advance_signin(u);
        return;
    }
    if (heart_sync(u))
        return;
    poll_engine(u);
    plat_mutex_lock(u->cmd.mu);
    u->cmd.local = local_ready(u);
    plat_mutex_unlock(u->cmd.mu);

    char msg[sizeof u->msg];
    int red = status_message(u, msg, sizeof msg);
    if (strcmp(msg, u->msg_shown) != 0 || red != u->status_red) {
        memcpy(u->msg_shown, msg, sizeof u->msg_shown);
        u->status_red = red;
        u->text_dirty = 1;
    }
    /* The one pairing UI: a dismissible tip while an unpaired Soloist is
     * discoverable. */
    char hint[sizeof u->hint] = "";
    if (u->sol && !u->paired && !u->hint_hidden)
        snprintf(hint, sizeof hint, "Tip: in Spotify on %s, choose '%s' to play on this computer.",
                 u->other_dev[0] ? u->other_dev : "your phone or computer", u->cfg.device_name);
    if (strcmp(hint, u->hint) != 0) {
        memcpy(u->hint, hint, sizeof u->hint);
        u->text_dirty = 1;
    }

    /* Layout (logical pixels). The art square takes the slack height. */
    const Metrics *m = &u->m;
    PlayerRows rows = player_rows(m);
    float w = (float)u->w, h = (float)u->h, cw = w - 2 * PAD;
    float hint_h = two_lines(m) + 6, art_min = 2 * m->row + 6; /* Restart and Change key fit */
    u->need_h = player_reserve(m) + hint_h + art_min;
    float art = h - player_reserve(m) - (u->hint[0] ? hint_h : 0.0f);
    if (art > cw)
        art = cw;
    if (art < art_min)
        art = art_min;
    /* Bottom bar: Devices and Library on the left, Settings and Log out on
     * the right; each button is its label plus a margin that narrows to fit. */
    float lw[BAR_BUTTONS], labels = 0;
    for (int i = 0; i < BAR_BUTTONS; i++) {
        lw[i] = text_w(u, bar_label[i], FS_S);
        labels += lw[i];
    }
    float margin = (cw - labels - (BAR_BUTTONS - 1) * BAR_GAP) / (2 * BAR_BUTTONS);
    margin = fmaxf(BAR_MARGIN, fminf(margin, roundf(0.8f * (float)m->fs)));
    float by = h - PAD - m->bar;
    GuiRect devices = { PAD, by, lw[0] + 2 * margin, m->bar };
    GuiRect library = { devices.x + devices.w + BAR_GAP, by, lw[1] + 2 * margin, m->bar };
    GuiRect logout = { w - PAD - lw[3] - 2 * margin, by, lw[3] + 2 * margin, m->bar };
    GuiRect settings = { logout.x - BAR_GAP - lw[2] - 2 * margin, by, lw[2] + 2 * margin, m->bar };
    float status_y = by - two_lines(m) - 4; /* up to two lines */
    float xw = (float)m->fs + 8;              /* the tip's close button */
    if (u->text_dirty)
        refresh_player_text(u, cw, cw, cw - xw - 6);

    /* Paired, but Soloist is down (stopped, failed to start or update). */
    int sol_down = u->paired && !u->sol && !u->inst.th && u->eng_err[0];
    text(u, u->status, PAD, status_y, FS_S, u->status_red ? GUI_ERR : GUI_DIM);
    if (u->hint[0]) {
        float hy = status_y - hint_h;
        text(u, u->hint_fit, PAD, hy, FS_S, GUI_DIM);
        GuiRect rx = { w - PAD - xw, hy, xw, (float)m->fs + 6 };
        if (gui_button(W_HINT_CLOSE, rx, "x", px_of(u, FS_S), 0))
            u->hint_hidden = 1; /* Soloist stays discoverable */
    }
    if (gui_button(W_LOGOUT, logout, "Log out", px_of(u, FS_S), 0)) {
        stop_session(u); /* Soloist first: logout deletes its pairing */
        auth_store_logout(u->auth);
        u->reinstalled = 0;
        u->login_msg = NULL;
        advance_signin(u);
        return;
    }
    if (gui_button(W_DEVICES, devices, "Devices", px_of(u, FS_S), u->panel == PANEL_DEVICES)) {
        u->panel = u->panel == PANEL_DEVICES ? PANEL_NONE : PANEL_DEVICES;
        if (u->panel == PANEL_DEVICES)
            cmd_post(&u->cmd, CMD_DEVICES, 0); /* devices come and go: always refresh */
    }
    if (gui_button(W_LIBRARY, library, "Library", px_of(u, FS_S), u->panel == PANEL_LIBRARY)) {
        u->panel = u->panel == PANEL_LIBRARY ? PANEL_NONE : PANEL_LIBRARY;
        if (u->panel == PANEL_LIBRARY)
            lib_opened(u);
    }
    if (gui_button(W_SETTINGS, settings, "Settings", px_of(u, FS_S), u->panel == PANEL_SETTINGS)) {
        u->panel = u->panel == PANEL_SETTINGS ? PANEL_NONE : PANEL_SETTINGS;
        u->size_unsaved = 0;
    }
    GuiRect panel = { PAD, PAD, cw, status_y - 8 - PAD };
    if (u->panel == PANEL_LIBRARY) {
        draw_library(u, panel);
        return;
    }
    if (u->panel == PANEL_DEVICES) {
        draw_devices(u, panel);
        return;
    }
    if (u->panel == PANEL_SETTINGS) {
        draw_settings(u, panel);
        return;
    }

    float y = PAD;
    GuiRect art_r = { (w - art) / 2.0f, y, art, art };
    update_art(u);
    gui_fill(art_r, 0.03f, GUI_PANEL);
    if (Rtexture_is_valid(u->art_tex))
        gui_image(u->art_tex, art_r, GUI_WHITE);
    gui_outline(art_r, 0.03f, 1.0f, GUI_HOVER);
    if (sol_down) {
        /* Only on request: an automatic restart could loop forever. */
        float bw = fmaxf(btn_w(u, "Restart Soloist", 140), btn_w(u, "Change key", 140));
        float ry = y + (art - art_min) / 2.0f;
        GuiRect rr = { (w - bw) / 2.0f, ry, bw, m->row };
        GuiRect rk = { (w - bw) / 2.0f, ry + m->row + 6, bw, m->row };
        if (gui_button(W_RESTART, rr, "Restart Soloist", px_of(u, FS_S), 1)) {
            reload_key(u); /* a key fixed in .env takes effect */
            start_engine(u, 0);
            return;
        }
        if (gui_button(W_CHANGE_KEY, rk, "Change key", px_of(u, FS_S), 0)) {
            u->setup_msg = NULL;
            set_screen(u, SCR_KEY);
            return;
        }
    }
    y += art; /* the rows below are offsets from the art's bottom edge */

    text(u, u->title, PAD, y + rows.title, FS_L, GUI_TEXT);
    if (likeable(&u->st)) {
        /* The heart at the end of the title's line: save it or remove it. */
        float hs = heart_side(m);
        GuiRect rh = { w - PAD - hs, y + rows.title + ((float)m->fl - hs) / 2.0f, hs, hs };
        if (gui_clicked(W_HEART, rh)) {
            u->heart = u->heart != 1;
            lib_post_save(&u->libw, u->heart_uri, u->heart, ++u->like_sent);
        }
        gui_heart(rh, u->heart == 1, fmaxf(1.5f, u->m.fs / 8.0f));
    }
    text(u, u->artists, PAD, y + rows.artists, FS_S, GUI_TEXT);
    text(u, u->album, PAD, y + rows.album, FS_S, GUI_DIM);

    PlaybackState *s = &u->st;
    int dur = s->track.duration_ms;
    int can_seek = s->active && dur > 0;
    GuiRect bar = { PAD, y + rows.seek, cw, 6 };
    if (gui_active != W_SEEK)
        u->seek_t = can_seek ? (float)progress_at(s, now) / (float)dur : 0.0f;
    int seek_done = 0;
    gui_slider(W_SEEK, bar, &u->seek_t, can_seek, &seek_done);
    if (seek_done) {
        u->pend.progress_ms = (int)(u->seek_t * (float)dur);
        u->pend.progress_at = now;
        pend_begin(u, PEND_SEEK, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_SEEK, u->pend.progress_ms);
    }
    int pos = can_seek ? (int)(u->seek_t * (float)dur) : 0;
    if (pos / 1000 != u->pos_sec) {
        u->pos_sec = pos / 1000;
        sp_fmt_time(u->t_pos, sizeof u->t_pos, pos);
    }
    text(u, u->t_pos, PAD, y + rows.times, FS_S, GUI_DIM);
    text(u, u->t_dur, w - PAD - text_w(u, u->t_dur, FS_S), y + rows.times, FS_S, GUI_DIM);

    float ty = y + rows.transport;
    GuiRect rprev = { w / 2.0f - 28 - 16 - 44, ty + 2, 44, 44 };
    GuiRect rplay = { w / 2.0f - 28, ty, 56, 48 };
    GuiRect rnext = { w / 2.0f + 28 + 16, ty + 2, 44, 44 };
    if (gui_icon_button(W_PREV, rprev, GUI_ICON_PREV)) {
        u->pend.mask &= ~PEND_SEEK;
        cmd_post(&u->cmd, CMD_PREV, 0);
    }
    if (gui_clicked(W_PLAY, rplay)) {
        int play = !(s->active && s->is_playing);
        u->pend.playing = play;
        u->pend.progress_ms = progress_at(s, now);
        u->pend.progress_at = now;
        pend_begin(u, PEND_PLAY, now);
        pend_reapply(u);
        cmd_post(&u->cmd, play ? CMD_PLAY : CMD_PAUSE, 0);
    }
    gui_icon_face(W_PLAY, rplay, s->active && s->is_playing ? GUI_ICON_PAUSE : GUI_ICON_PLAY, 1);
    if (gui_icon_button(W_NEXT, rnext, GUI_ICON_NEXT)) {
        u->pend.mask &= ~PEND_SEEK;
        cmd_post(&u->cmd, CMD_NEXT, 0);
    }

    static const char *const repeat_label[] = { "Repeat: off", "Repeat: all", "Repeat: one" };
    float half = (cw - 8) / 2.0f;
    GuiRect rshuf = { PAD, y + rows.modes, half, m->row };
    GuiRect rrep = { PAD + half + 8, y + rows.modes, half, m->row };
    if (gui_button(W_SHUFFLE, rshuf, "Shuffle", px_of(u, FS_S), s->shuffle)) {
        u->pend.shuffle = !s->shuffle;
        pend_begin(u, PEND_SHUFFLE, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_SHUFFLE, u->pend.shuffle);
    }
    unsigned rep = (unsigned)s->repeat <= REPEAT_TRACK ? (unsigned)s->repeat : 0;
    if (gui_button(W_REPEAT, rrep, repeat_label[rep], px_of(u, FS_S), rep != REPEAT_OFF)) {
        u->pend.repeat = (RepeatMode)((rep + 1) % 3);
        pend_begin(u, PEND_REPEAT, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_REPEAT, (int)u->pend.repeat);
    }

    float vy = y + rows.volume, vx = PAD + text_w(u, "Vol", FS_S) + 14;
    text(u, "Vol", PAD, vy, FS_S, GUI_DIM);
    GuiRect rvol = { vx, vy + ((float)m->fs - 6) / 2.0f, w - PAD - vx, 6 };
    if (gui_active != W_VOLUME)
        u->vol_t = s->volume >= 0 ? (float)s->volume / 100.0f : 0.0f;
    int vol_done = 0;
    gui_slider(W_VOLUME, rvol, &u->vol_t, s->active, &vol_done);
    if (vol_done) {
        u->pend.volume = (int)(u->vol_t * 100.0f + 0.5f);
        pend_begin(u, PEND_VOLUME, now);
        pend_reapply(u);
        cmd_post(&u->cmd, CMD_VOLUME, u->pend.volume);
    }
}
