/* Spotify OAuth (Authorization Code + PKCE, no client secret) and a
 * thread-safe token store shared by all threads. */
#ifndef AUTH_H
#define AUTH_H

#include "config.h"
#include "http.h"

/* Scopes: playback read/modify, currently-playing, playlists, the library
 * (read, save/remove), followed artists (read, follow/unfollow) and recently
 * played. No "streaming": Soloist plays with its own paired session
 * (soloist.h). A scope added here makes every older sign-in ask for a new
 * consent (auth_store_needs_reconsent). */
#define AUTH_SCOPES "user-read-playback-state user-modify-playback-state " \
                    "user-read-currently-playing playlist-read-private " \
                    "playlist-read-collaborative user-library-read " \
                    "user-library-modify user-follow-read user-follow-modify " \
                    "user-read-recently-played"

typedef struct AuthStore AuthStore;

/* Loads the saved token if present. Never NULL unless OOM. */
AuthStore *auth_store_new(const Config *cfg);
void       auth_store_free(AuthStore *s);

/* 1 if a refresh token is available (login not needed). */
int auth_store_logged_in(AuthStore *s);

/* 1 when the saved sign-in lacks a scope of AUTH_SCOPES; a token saved
 * before the granted scopes were recorded counts as lacking them all. It
 * keeps working for what it was granted (auth_store_logged_in stays 1) and
 * calls needing the rest get SP_ERR_SCOPE, so ask the user once to sign in
 * again (auth_store_login), saying AUTH_RECONSENT_REASON. A new sign-in (or
 * refresh) records what Spotify granted. 0 when complete or signed out. */
int auth_store_needs_reconsent(AuthStore *s);
#define AUTH_RECONSENT_REASON "New features need permission. Please sign in again."

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
/* 1 if every space-separated scope in want is in granted (space-separated,
 * any order; NULL = ""), else 0. */
int auth_scopes_cover(const char *granted, const char *want);

#endif
