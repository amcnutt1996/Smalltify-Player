/* Shared UI types: the worker structs and the Ui struct. Types and macros
 * only; the logic stays in ui.c, and the widgets of each screen stay in its
 * module (screens/), file-static. */
#ifndef SMP_UI_STATE_H
#define SMP_UI_STATE_H

#include <stdint.h>

#include <Raven.h>

#include "art.h"
#include "auth.h"
#include "browse.h"
#include "config.h"
#include "model.h"
#include "platform.h"
#include "poller.h"
#include "soloist.h"
#include "spotify.h"

/* The window: landscape, and the smallest it can be made. */
#define WIN_W 1000
#define WIN_H 660
#define MIN_W 360 /* until the shell sets the real one (shell_set_main_min) */
#define MIN_H 320

/* Optimistic edits survive contradicting snapshots this long: right after a
 * command Spotify often still reports the old state. */
#define PENDING_MS        3000
#define DEVICE_WAIT_TRIES 15
#define DEVICE_FIRST_MS   1500 /* Soloist needs a moment to register */
#define DEVICE_RETRY_MS   1000
#define PAIR_FAST_MS      2000   /* pairing: look for our device this often... */
#define PAIR_FAST_FOR_MS  120000 /* ...for the first 2 minutes, */
#define PAIR_SLOW_MS      15000  /* then (or while minimized) this often */
#ifndef PAIR_IDLE_STOP_MS
/* Unpaired Soloist stops when no other device of the account has been
 * listed this long (overridable at compile time for tests). */
#define PAIR_IDLE_STOP_MS 600000
#endif
#define IDLE_WAIT_MS      3600000u
#define SIGNIN_URL_LEN    4096 /* authorize URLs with many scopes are long */

/* --- command worker: playback commands, the queue, devices --------------- */
/* Commands wait in a short FIFO. A playback command replaces one still
 * waiting at its end (latest wins, so rapid clicks collapse into one), but
 * queue adds are never replaced and keep their order. */
typedef enum {
    CMD_PLAY, CMD_PAUSE, CMD_NEXT, CMD_PREV,
    CMD_SEEK, CMD_VOLUME, CMD_SHUFFLE, CMD_REPEAT,
    CMD_PLAY_URI,  /* ctx and uri as in PlayTarget; arg = 1: ctx is Liked Songs */
    CMD_QUEUE,     /* uri: add it to the queue */
    CMD_DEVICES,   /* results land in CmdWorker.devs */
    CMD_TRANSFER   /* uri = device id; plays there, then refreshes devices */
} CmdKind;

typedef struct {
    CmdKind kind;
    int     arg;
    char    uri[SP_URI_LEN]; /* a track or episode, or a device id */
    char    ctx[SP_URI_LEN];
} Cmd;

_Static_assert(SP_URI_LEN >= SP_ID_LEN, "a device id fits Cmd.uri");

#define CMD_FIFO 16

/* Spotify Connect devices, filled by the command worker. */
typedef struct {
    Device devs[SP_MAX_DEVICES];
    int    ndev, loaded;
} DeviceList;

typedef struct {
    AuthStore   *auth;
    Poller      *poller;
    const char  *name;   /* our device name */
    plat_mutex  *mu;
    plat_event  *ev;
    plat_thread *th;
    User         me;     /* worker thread only: names Liked Songs; fetched once */
    /* Guarded by mu. */
    Cmd        fifo[CMD_FIFO];
    int        head, len;
    int        stop;
    int        local;    /* our Soloist device is running and paired */
    int        me_stale; /* a new sign-in: look the account up again */
    int        err;      /* result of the last command */
    CmdKind    done;     /* which command that was */
    uint64_t   done_at;  /* plat_now_ms() when it finished */
    char       played_on[SP_NAME_LEN]; /* "" unless it played on another, inactive device */
    uint64_t   err_seq;  /* bumped after every command */
    DeviceList devs;     /* the latest device list */
    uint64_t   devs_seq; /* bumped when devs changes */
} CmdWorker;

/* --- library worker: lists, search and the heart -------------------------- */
/* A thread and HTTP connection of its own, so a list never waits behind a
 * playback command, nor the other way round. Each kind of request has a
 * slot keeping only the newest: a list page (its answer carries the
 * request's seq, so the UI drops pages for screens it has left), "is this
 * track saved?" and "save or remove it". */
#define LIB_PAGE (BROWSE_GROUPS * SP_SEARCH_LIMIT) /* the most one request brings: a search of every kind */
_Static_assert(LIB_PAGE >= SP_PAGE_MAX, "a list page fits LIB_PAGE");

typedef struct {
    uint64_t seq;
    int      search;             /* sp_search(query, types), else sp_get_list(list, id) */
    SpList   list;
    unsigned types;
    char     id[SP_URI_LEN];
    char     query[SP_NAME_LEN];
    SpPage   page;               /* where it starts */
    int      cap;                /* at most LIB_PAGE */
} LibReq;

typedef struct {
    AuthStore   *auth;
    plat_mutex  *mu;
    plat_event  *ev;
    plat_thread *th;
    /* Guarded by mu: the slots... */
    int      stop;
    LibReq   req;
    int      has_req;
    char     check[SP_URI_LEN];  /* is it saved? */
    int      has_check;
    char     save[SP_URI_LEN];   /* save (save_on) or remove it */
    int      save_on, has_save;
    uint64_t save_seq;
    /* ...and the answers */
    uint64_t res_seq;            /* the LibReq.seq the page answers, 0 = none */
    int      res_rc, res_n;
    SpPage   res_page;
    Item     res[LIB_PAGE];
    char     checked[SP_URI_LEN];
    int      checked_saved, checked_rc;
    uint64_t checked_gen;        /* bumped with every answer */
    char     saved[SP_URI_LEN];
    int      saved_on, saved_rc;
    uint64_t saved_seq;          /* the save_seq it answers */
} LibWorker;

/* --- device worker ------------------------------------------------------- */
/* Transfer: one shot at engine start, make our device the active one when
 * nothing else plays. Pair: poll the device list until our device shows
 * up, which is the only sign that the user picked it in a Spotify app;
 * meanwhile report another device of the account to name in the hint,
 * and give up once none has been listed for PAIR_IDLE_STOP_MS. */
typedef struct {
    AuthStore   *auth;
    Poller      *poller;
    const char  *name;      /* our device name */
    int          pair;
    volatile int slow;      /* set by the UI: minimized */
    volatile int found;     /* pair: our device is listed */
    volatile int idle_out;  /* pair: no other device for too long */
    plat_mutex  *mu;
    char         other[SP_NAME_LEN]; /* pair: another device, guarded by mu */
    plat_event  *stop;      /* signaled = give up now */
    plat_thread *th;
} DeviceWorker;

/* --- Soloist install worker: check the build, download it if needed ------ */
#define ERR_LEN 256

typedef struct {
    const Config *cfg;
    int           force;  /* re-download even if the build is fine */
    volatile int  cancel;
    plat_mutex   *mu;
    plat_thread  *th;
    /* Guarded by mu. */
    int           downloading, percent, done, rc;
    char          err[ERR_LEN];
} Installer;

/* --- UI state ------------------------------------------------------------ */
typedef enum { SCR_MOVE, SCR_SETUP, SCR_LOGIN, SCR_KEY, SCR_PLAYER } Screen;

enum { PEND_PLAY = 1, PEND_SEEK = 2, PEND_SHUFFLE = 4, PEND_REPEAT = 8, PEND_VOLUME = 16 };

typedef struct {
    int        mask;
    int        playing, shuffle, volume;
    RepeatMode repeat;
    int        progress_ms;
    uint64_t   progress_at;
    uint64_t   until;
} Pending;

/* What every screen shows comes from here: the update pass (ui.c) pulls
 * the workers' news into it, then each screen module pushes what changed
 * into its own widgets. Widgets live in the modules, file-static. */
typedef struct {
    Config     cfg;
    AuthStore *auth;
    Screen     screen;
    int        preview;      /* SMP_UI_PREVIEW set: the player page without a session */
    int        size_unsaved; /* Settings could not write UI_FONT_SIZE */

    /* setup and key screens */
    const char *setup_msg;   /* "" or NULL = none; an error to show */

    /* sign-in screen: our PKCE login */
    plat_mutex  *mu;
    plat_thread *login_th;
    volatile int login_cancel;
    int          login_done, login_rc;     /* guarded by mu */
    char         login_url[SIGNIN_URL_LEN]; /* guarded by mu */
    const char  *login_msg;
    int          reauth;        /* the sign-in screen is over the running player */
    int          consent_asked; /* the new permissions were asked for this run */

    /* Soloist, run from the player: always once paired, else only while
     * another device of the account is online so it can pick us */
    Installer    inst;
    Soloist     *sol;
    int          paired;         /* data/soloist-paired exists */
    int          sol_was_paired; /* paired when this Soloist started */
    int          transferred;    /* startup transfer tried for this Soloist */
    int          auto_failed;    /* an unpaired start failed: not again this run */
    int          reinstalled;    /* an expired build was replaced once */
    int          hint_hidden;    /* pairing tip dismissed this run */
    char         other_dev[SP_NAME_LEN]; /* another device of the account, for the tip */
    char         eng_err[ERR_LEN]; /* "" = no error to show */

    /* player screen */
    Poller       *poller;
    CmdWorker     cmd;
    DeviceWorker  dev;
    PlaybackState st;          /* the poller's snapshot, optimistic edits on top */
    uint64_t      seq;         /* the snapshot taken (poller_snapshot) */
    Pending       pend;
    uint64_t      cmd_seq;
    int           cmd_err, poll_err, background;
    char          played_on[SP_NAME_LEN];
    char          status[320];  /* the one status line (status_message) */
    int           status_error; /* status is an error */
    char          hint[320];    /* pairing tip; "" = none */
    uint64_t      note_until;   /* "Added to queue" shows until then */

    /* the heart by the title: is the playing track saved? */
    char     heart_uri[SP_URI_LEN]; /* the track it is for, "" = none */
    int      heart;                 /* 1 saved, 0 not, -1 unknown */
    uint64_t heart_gen;             /* libw.checked_gen taken */
    uint64_t like_sent, like_done;  /* saves asked for / answered (save_seq) */
    int      like_err;              /* SP_ERR_* of a failed save */

    /* album art: decoded once per image into a texture */
    ArtLoader *art;
    uint64_t   art_seq; /* art_take's */
    uint64_t   art_gen; /* bumped whenever art_tex changes */
    Rid        art_tex; /* the art playing; R_INVALID_ID = none */
    Rid        art_old; /* the one before, deleted at the end of the update pass */

    /* Spotify Connect devices, from the command worker */
    uint64_t   devs_seq;      /* cmd.devs_seq of devs: changes with the list */
    uint64_t   dev_seq_noted; /* cmd.devs_seq last checked by note_other */
    DeviceList devs;

    /* lists, search and the heart; the Library's screens are library.c's */
    LibWorker  libw;

    char key_input[sizeof(((Config *)0)->soloist_key)]; /* key screen; secret */
    char id_input[sizeof(((Config *)0)->client_id)];    /* setup screen; public */
} Ui;

#endif /* SMP_UI_STATE_H */
