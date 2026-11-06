/* Spotify OAuth (Authorization Code + PKCE, no client secret) and a
 * thread-safe token store shared by all threads. */
#ifndef AUTH_H
#define AUTH_H

#include "config.h"
#include "http.h"

/* Scopes: playback read/modify, currently-playing, playlists, library.
 * No "streaming": Soloist plays with its own paired session (soloist.h). */
#define AUTH_SCOPES "user-read-playback-state user-modify-playback-state " \
                    "user-read-currently-playing playlist-read-private " \
                    "playlist-read-collaborative user-library-read"

typedef struct AuthStore AuthStore;

/* Loads the saved token if present. Never NULL unless OOM. */
AuthStore *auth_store_new(const Config *cfg);
void       auth_store_free(AuthStore *s);

/* 1 if a refresh token is available (login not needed). */
int auth_store_logged_in(AuthStore *s);

/* Interactive PKCE login: opens browser, waits on the loopback redirect,
 * exchanges code, saves token. Blocks up to 5 min; returns early with -1
 * when *cancel becomes nonzero (cancel may be NULL). Safe to run on a
 * worker thread (GUI login screen does this). 0 ok, else a negative
 * AUTH_ERR_* saying why (auth_login_strerror words it for users). */
enum {
    AUTH_ERR_OTHER   = -1, /* includes cancel */
    AUTH_ERR_PORT    = -2, /* the redirect port is taken */
    AUTH_ERR_TIMEOUT = -3, /* no redirect in 5 min (e.g. a wrong client ID) */
    AUTH_ERR_DENIED  = -4, /* the user (or Spotify) refused */
    AUTH_ERR_TOKEN   = -5  /* the code exchange failed (network, client ID) */
};
int auth_store_login(AuthStore *s, const volatile int *cancel);
const char *auth_login_strerror(int rc);

/* Optional: receive the authorize URL (e.g. for a "copy link" fallback).
 * Called on the auth_store_login thread once the loopback listener is up,
 * right before the browser is opened. Without a callback the URL is
 * printed to stdout instead. Set before auth_store_login; fn may be NULL. */
typedef void (*AuthUrlFn)(const char *url, void *ud);
void auth_store_set_url_callback(AuthStore *s, AuthUrlFn fn, void *ud);

/* Logout: forget tokens in memory and on disk. */
void auth_store_logout(AuthStore *s);

/* Callbacks for http_client_new(). */
HttpAuth auth_store_http(AuthStore *s);

/* Pure helpers (exposed for tests). */
int auth_pkce_challenge(const char *verifier, char *out, size_t cap); /* base64url(sha256) */

#endif
