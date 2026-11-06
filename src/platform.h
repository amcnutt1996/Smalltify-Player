/* The ONLY place OS-specific code lives (POSIX + Win32 via #ifdef in
 * platform.c). Every other module stays portable C11. */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"
#define PLAT_SEP '\\'
#else
#define PLAT_EXE_SUFFIX ""
#define PLAT_SEP '/'
#endif

/* --- paths / files ------------------------------------------------------ */
/* Absolute directory of the running executable, no trailing separator.
 * All app data lives under here (portable: nothing written elsewhere). */
const char *plat_exe_dir(void);
/* out = a + SEP + b. Returns 0, or -1 if truncated. */
int  plat_path_join(char *out, size_t cap, const char *a, const char *b);
/* Create dir if missing (owner-only perms). 0 ok/exists, -1 error. */
int  plat_mkdir(const char *path);
/* Atomically replace path with data (write tmp in same dir + rename),
 * owner-only perms. 0 ok, -1 error. */
int  plat_write_file_atomic(const char *path, const void *data, size_t len);
/* Read a whole file (at most max_len bytes, else error) into a malloc'd,
 * NUL-terminated buffer; *out_len gets the length (may be NULL). Caller
 * frees. NULL if missing/unreadable/too big. UTF-8 paths on every OS. */
char *plat_read_file(const char *path, size_t max_len, size_t *out_len);
/* Delete a file or a directory tree (symlinks are not followed).
 * 0 if gone afterwards (including "did not exist"), -1 error. */
int  plat_remove_tree(const char *path);
/* Fill buf with cryptographically secure random bytes. 0 ok, -1 error. */
int  plat_random_bytes(void *buf, size_t len);
/* Open a URL in the default browser. 0 ok. */
int  plat_open_url(const char *url);

/* --- process lifetime --------------------------------------------------- */
/* Call first in main(). Win32: UTF-8 console output, and *argv rebuilt as
 * UTF-8 from the wide command line (the CRT's argv uses the ANSI code
 * page). POSIX: ignore SIGPIPE, since a socket closed by the peer must not
 * kill us and curl's NOSIGNAL mode leaves that to the application.
 * 0 ok, -1 error. */
int  plat_startup(int *argc, char ***argv);
/* Turn SIGINT/SIGTERM/SIGHUP (Win32: console Ctrl-C/Break/close) into a quit
 * request instead of instant death, so the caller can stop its children.
 * on_quit(ud) then runs on a helper thread, never in signal context, and
 * may be NULL. Later calls only swap the callback: once a call returns, the
 * previous callback will not run again. 0 ok, -1 error. */
int  plat_quit_watch(void (*on_quit)(void *ud), void *ud);
/* 1 once a quit was requested through plat_quit_watch's signals. */
int  plat_quit_requested(void);

/* --- time --------------------------------------------------------------- */
uint64_t plat_now_ms(void);      /* monotonic */
int64_t  plat_unix_time(void);   /* wall clock seconds */
void     plat_sleep_ms(unsigned ms);

/* --- threads ------------------------------------------------------------ */
typedef struct plat_thread plat_thread;
typedef struct plat_mutex  plat_mutex;
typedef struct plat_event  plat_event; /* auto-reset signal for timed waits */

plat_thread *plat_thread_start(void (*fn)(void *), void *arg);
void         plat_thread_join(plat_thread *t); /* joins and frees */

plat_mutex *plat_mutex_new(void);
void        plat_mutex_lock(plat_mutex *m);
void        plat_mutex_unlock(plat_mutex *m);
void        plat_mutex_free(plat_mutex *m);

plat_event *plat_event_new(void);
void        plat_event_signal(plat_event *e);
/* Wait until signaled or timeout. Returns 1 if signaled, 0 on timeout. */
int         plat_event_wait(plat_event *e, unsigned timeout_ms);
void        plat_event_free(plat_event *e);

/* --- child processes ---------------------------------------------------- */
typedef struct plat_proc plat_proc;
/* Spawn argv[0] (absolute path) with NULL-terminated argv. The child's
 * stdout+stderr are merged into a pipe readable with plat_proc_read.
 * Stop it with plat_proc_kill on shutdown. If we die without that (crash,
 * SIGKILL) the child is still terminated: a kill-on-close job (Win32),
 * PR_SET_PDEATHSIG tied to the calling thread (Linux: spawn from the main
 * thread), or a tiny watchdog process (other POSIX). NULL on error. */
plat_proc *plat_proc_spawn(const char *const argv[]);
/* Blocking read of child output. Returns bytes read, 0 on EOF, -1 error. */
long       plat_proc_read(plat_proc *p, char *buf, size_t cap);
/* Wait up to timeout_ms for the child to exit on its own. Returns its exit
 * code (0-255), or -1 if still running, killed by a signal, or on error. */
int        plat_proc_wait(plat_proc *p, unsigned timeout_ms);
/* Terminate (graceful, then forced after 2 s) and reap, but keep p valid:
 * a reader thread blocked in plat_proc_read then sees EOF and can be
 * joined before plat_proc_kill frees p. Idempotent. */
void       plat_proc_terminate(plat_proc *p);
/* Terminate (graceful, then forced), reap, free. NULL-safe. */
void       plat_proc_kill(plat_proc *p);
/* Extract a .tar.gz (POSIX) or .zip (Win32) into dir with the OS's own
 * tar (built into macOS, Linux and Windows 10 1803+). Blocks until tar
 * exits. 0 ok, -1 error. */
int        plat_untar(const char *archive, const char *dir);

/* --- loopback TCP (OAuth redirect listener) ----------------------------- */
typedef intptr_t plat_sock;
#define PLAT_SOCK_INVALID ((plat_sock)-1)
/* Listen on 127.0.0.1:port only. */
plat_sock plat_listen_loopback(int port);
/* Accept with timeout. Returns client socket, or PLAT_SOCK_INVALID on timeout/error. */
plat_sock plat_accept(plat_sock s, unsigned timeout_ms);
long      plat_recv(plat_sock s, char *buf, size_t cap, unsigned timeout_ms);
long      plat_send(plat_sock s, const char *buf, size_t len);
void      plat_close(plat_sock s);

#endif
