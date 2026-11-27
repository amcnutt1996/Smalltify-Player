/* sidebar_devices.c: the Devices tab of the sidebar (sidebar_tabs.h): the
 * Spotify Connect devices of the account, this computer first once it is
 * paired, the active one marked; a click moves playback there. Over them,
 * while Soloist has no key, the way to play on this computer. */
#include "screens/sidebar_tabs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gui/components.h"
#include "soloist.h"
#include "ui_internal.h"

static struct {
    Ui *u;
    Rid enable, list, message;
    CompRow row[SP_MAX_DEVICES];
    int index[SP_MAX_DEVICES]; /* row k shows u->devs.devs[index[k]] */
    int n, built;
    uint64_t seq;              /* u->devs_seq shown */
    int paired;                /* u->paired shown: names this computer */
    char active[SP_ID_LEN];    /* the device marked active; "" = by the list */
} g;

/* ASCII, case ignored. */
static int same_word(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

Icon device_icon(const char *type)
{
    static const struct {
        const char *type;
        Icon icon;
    } map[] = {
        { "Computer", ICON_COMPUTER },  { "Smartphone", ICON_PHONE },    { "Tablet", ICON_PHONE },
        { "Speaker", ICON_SPEAKER },    { "AVR", ICON_SPEAKER },         { "AudioDongle", ICON_SPEAKER },
        { "CastAudio", ICON_SPEAKER },  { "Automobile", ICON_SPEAKER },
    };
    for (size_t i = 0; type && i < sizeof map / sizeof map[0]; i++)
        if (same_word(type, map[i].type))
            return map[i].icon;
    return ICON_TV; /* TVs, consoles, anything else */
}

/* Our own Soloist device, once paired. */
static int is_ours(const Ui *u, const Device *d)
{
    return u->paired && strcmp(d->name, u->cfg.device_name) == 0;
}

/* The device playing: the poller's, fresher than the list's mark. */
static int is_active(const Ui *u, const Device *d)
{
    if (u->st.active && u->st.device_id[0])
        return strcmp(d->id, u->st.device_id) == 0;
    return d->is_active;
}

static void on_device(Rid button, void *ud)
{
    (void)button;
    Ui *u = g.u;
    int i = (int)(intptr_t)ud;
    if (i < u->devs.ndev && u->devs.devs[i].id[0])
        cmd_post_uri(&u->cmd, CMD_TRANSFER, 0, u->devs.devs[i].id, NULL);
}

static void on_refresh(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    cmd_post(&g.u->cmd, CMD_DEVICES, 0);
}

/* The only way to the key screen: nobody is nagged for it. */
static void on_enable(Rid button, void *ud)
{
    (void)button;
    (void)ud;
    if (g.u->screen != SCR_PLAYER)
        return;
    g.u->setup_msg = NULL;
    set_screen(g.u, SCR_KEY);
}

void devices_tab_build(Ui *u, Rid parent)
{
    g.u = u;
    Rid head = comp_hbox(parent, COMP_GAP);
    Rid title = comp_text(head, "Devices", TEXT_LARGE);
    Rslot_set_fill(title, 1.0f);
    Rslot_set_valign(title, R_VALIGN_CENTER);
    Rid refresh = comp_icon_button(head, ICON_REFRESH, ICON_BUTTON_MEDIUM, "Refresh", on_refresh, NULL);
    Rslot_set_valign(refresh, R_VALIGN_CENTER);

    Rid body = comp_scroll(parent, 2.0f);
    Rslot_set_fill(Rwidget_get_parent(body), 1.0f);
    Rslot_set_padding(Rwidget_get_parent(body), 0.0f, 4.0f, 0.0f, 0.0f);
    CompRow enable = comp_row(body, "Enable playback on this computer", "Play here with Spotify Soloist", on_enable,
                              NULL);
    comp_row_icon(&enable, ICON_PLUS);
    comp_set_class(enable.row, "on", 1);
    g.enable = enable.row;
    comp_set_visible(g.enable, 0);
    g.list = comp_vbox(body, 2.0f);
    g.message = comp_label(body, "", TEXT_CAPTION);
    Rwidget_add_class(g.message, "muted");
    Rslot_set_padding(g.message, 10.0f, 6.0f, 10.0f, 6.0f);
}

/* Row k as device d: its name ("This computer" for ours), what it is, and
 * whether it plays. */
static void show_row(const Ui *u, int k, const Device *d)
{
    int ours = is_ours(u, d), on = is_active(u, d);
    comp_row_set(&g.row[k], ours ? "This computer" : d->name[0] ? d->name : "Unnamed device",
                 ours ? d->name : d->type, on ? "Active" : "");
    comp_row_set_current(&g.row[k], on);
    comp_set_class(g.row[k].row, "current", on); /* its icon too */
}

static void rebuild(Ui *u)
{
    if (Rwidget_get_child_count(g.list))
        Rwidget_clear_children(g.list);
    g.n = 0;
    /* ours first (once paired), then Spotify's order */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < u->devs.ndev && g.n < SP_MAX_DEVICES; i++) {
            const Device *d = &u->devs.devs[i];
            if (is_ours(u, d) != !pass)
                continue;
            int k = g.n++;
            g.index[k] = i;
            g.row[k] = comp_row(g.list, "", NULL, on_device, (void *)(intptr_t)i);
            comp_row_icon(&g.row[k], is_ours(u, d) ? ICON_COMPUTER : device_icon(d->type));
            comp_set_enabled(g.row[k].row, d->id[0] != 0); /* Spotify lists some it won't play on */
            show_row(u, k, d);
        }
    }
    comp_text_set(g.message, u->devs.loaded ? "No devices. Open Spotify on a phone or computer."
                                            : "Loading devices" "\xE2\x80\xA6");
    comp_set_visible(g.message, g.n == 0);
}

void devices_tab_update(Ui *u)
{
    comp_set_visible(g.enable, soloist_supported() && !u->cfg.soloist_key[0]);
    const char *active = u->st.active ? u->st.device_id : "";
    if (!g.built || u->devs_seq != g.seq || u->paired != g.paired) {
        g.built = 1;
        g.seq = u->devs_seq;
        g.paired = u->paired;
        snprintf(g.active, sizeof g.active, "%s", active);
        rebuild(u);
    } else if (strcmp(active, g.active) != 0) {
        snprintf(g.active, sizeof g.active, "%s", active);
        for (int k = 0; k < g.n; k++)
            show_row(u, k, &u->devs.devs[g.index[k]]);
    }
}
