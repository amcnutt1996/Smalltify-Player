/* Spotify OAuth (Authorization Code + PKCE, no client secret) and a
 * thread-safe token store shared by all threads. */
#ifndef AUTH_H
#define AUTH_H

#include "config.h"
#include "http.h"

/* Scopes: playback read/modify, currently-playing, playlists, library,
 * plus "streaming" so the same token can log librespot in. */
#define AUTH_SCOPES "streaming user-read-playback-state user-modify-playback-state " \
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
 * worker thread (GUI login screen does this). 0 ok. */
int auth_store_login(AuthStore *s, const volatile int *cancel);

/* Copy a valid access token (refreshes if expiring within 60 s). 0 ok. */
int auth_store_access_token(AuthStore *s, char *buf, size_t cap);

/* Logout: forget tokens in memory and on disk. */
void auth_store_logout(AuthStore *s);

/* Callbacks for http_client_new(). */
HttpAuth auth_store_http(AuthStore *s);

/* Pure helpers (exposed for tests). */
int auth_pkce_challenge(const char *verifier, char *out, size_t cap); /* base64url(sha256) */

#endif
