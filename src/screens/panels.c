/* The Devices and Settings panels of the player. */
#include "screens/panels.h"

#include <string.h>

#include "../ui_internal.h"
#include "screens/common.h"

/* Spotify Connect devices; a click moves playback there via the worker. */
void draw_devices(Ui *u, GuiRect area)
{
    sync_devices(u);
    if (gui_input.key_escape) {
        u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    float row_h = u->m.row, gap = 6, bw = btn_w(u, "Refresh", 72);
    GuiRect refresh = { area.x + area.w - bw, area.y, bw, row_h };
    text(u, "Play on", area.x, mid_y(u, refresh), FS_S, GUI_TEXT);
    if (gui_button(W_DEV_REFRESH, refresh, "Refresh", px_of(u, FS_S), 0))
        cmd_post(&u->cmd, CMD_DEVICES, 0);

    float y = area.y + row_h + 10;
    if (soloist_supported() && !u->cfg.soloist_key[0]) {
        /* The only way to the key screen: nobody is nagged for it. */
        GuiRect r = { area.x, y, area.w, row_h };
        if (gui_button(W_LIB_ROW + SP_MAX_DEVICES, r, "Enable playback on this computer",
                       px_of(u, FS_S), 0)) {
            u->setup_msg = NULL;
            set_screen(u, SCR_KEY);
            return;
        }
        y += row_h + gap;
    }
    if (u->devs.ndev == 0) {
        para(u, u->devs.loaded ? "No devices. Open Spotify on a phone or computer."
                               : "Loading devices...", area.x, y + 4, area.w, GUI_DIM);
        return;
    }
    /* Our own device first (once paired), then Spotify's order. */
    int order[SP_MAX_DEVICES], n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < u->devs.ndev; i++)
            if ((u->paired && strcmp(u->devs.devs[i].name, u->cfg.device_name) == 0) == !pass)
                order[n++] = i;
    for (int k = 0; k < n && y + row_h <= area.y + area.h; k++, y += row_h + gap) {
        int i = order[k];
        const Device *d = &u->devs.devs[i];
        GuiRect r = { area.x, y, area.w, row_h };
        if (gui_button(W_LIB_ROW + i, r, NULL, px_of(u, FS_S), d->is_active) && d->id[0]) {
            cmd_post_uri(&u->cmd, CMD_TRANSFER, 0, d->id, NULL);
            u->panel = PANEL_NONE;
        }
        int ours = u->paired && strcmp(d->name, u->cfg.device_name) == 0;
        const char *sub = d->is_active ? "active" : ours ? "" : d->type;
        char fit[SP_NAME_LEN];
        float sw = text_w(u, sub, FS_S);
        gui_fit_text(px_of(u, FS_S), ours ? "This computer" : d->name, r.w - sw - 28, fit,
                     sizeof fit);
        text(u, fit, r.x + 8, mid_y(u, r), FS_S, GUI_TEXT);
        text(u, sub, r.x + r.w - 8 - sw, mid_y(u, r), FS_S, d->is_active ? GUI_TEXT : GUI_DIM);
    }
}

/* The text size: a pick applies at once, so the window is its own preview,
 * and is saved to .env as UI_FONT_SIZE. */
void draw_settings(Ui *u, GuiRect area)
{
    if (gui_input.key_escape) {
        u->panel = PANEL_NONE;
        u->redraw = 1;
        return;
    }
    float row_h = u->m.row, gap = 6;
    text(u, "Text size", area.x, mid_y(u, (GuiRect){ area.x, area.y, area.w, row_h }), FS_S,
         GUI_TEXT);
    float y = area.y + row_h + 10;
    for (int i = 0; i < CFG_FONT_SIZES && y + row_h <= area.y + area.h; i++, y += row_h + gap) {
        GuiRect r = { area.x, y, area.w, row_h };
        int on = i == u->m.size;
        if (gui_button(W_LIB_ROW + i, r, NULL, px_of(u, FS_S), on) && !on) {
            u->cfg.font_size = i; /* the next frame remakes the metrics */
            u->redraw = 1;
            u->size_unsaved =
                config_set_env_value(&u->cfg, "UI_FONT_SIZE", config_font_size_value(i)) != 0;
        }
        text(u, font_sizes[i].label, r.x + 8, mid_y(u, r), FS_S, GUI_TEXT);
    }
    if (u->size_unsaved)
        para(u, "Could not save this in .env: it applies until the app quits.", area.x, y + 4,
             area.w, GUI_ERR);
}
