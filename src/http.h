/* Thin libcurl wrapper. One HttpClient per thread (not thread-safe itself);
 * each keeps its curl easy handle alive for connection reuse (no TLS
 * handshake per request -> less CPU/battery). */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Bearer token provider. Callbacks must be thread-safe (AuthStore is). */
typedef struct {
    /* Write current access token into buf. 0 ok, -1 not logged in. */
    int (*get_token)(void *ud, char *buf, size_t cap);
    /* Called once after a 401. 0 if a fresh token is now available. */
    int (*refresh)(void *ud);
    void *ud;
} HttpAuth;

typedef struct {
    long   status;      /* HTTP status, 0 on transport error */
    char  *body;        /* NUL-terminated, may be NULL when len == 0 */
    size_t len;
} HttpResponse;

typedef struct HttpClient HttpClient;

int  http_global_init(void);   /* once at startup, before any thread */
void http_global_cleanup(void);

/* auth may be NULL (unauthenticated requests, e.g. token endpoint). */
HttpClient *http_client_new(const HttpAuth *auth);
void        http_client_free(HttpClient *c);
/* Abort any response body larger than max bytes while it downloads
 * (http_request then returns -1). 0 = no limit, the default. */
void        http_client_set_max_body(HttpClient *c, size_t max);
/* Large downloads: fn(ud, received, total) runs on the calling thread
 * while a response body arrives (total 0 while unknown); nonzero aborts
 * the transfer (http_request then returns -1). With fn set, the 15 s
 * request timeout is replaced by a stall timeout (under 1 KB/s for 30 s),
 * so slow links still finish. fn NULL restores the defaults. */
typedef int (*HttpProgressFn)(void *ud, size_t received, size_t total);
void        http_client_set_progress(HttpClient *c, HttpProgressFn fn, void *ud);

/* method: "GET" "PUT" "POST" "DELETE". body/content_type may be NULL.
 * With auth: adds "Authorization: Bearer", on 401 calls refresh once and
 * retries; on 429 honors Retry-After (capped) and retries once.
 * Returns 0 if a response was received (check out->status), -1 transport
 * error. out->body is the client's receive buffer handed over without a
 * copy: the caller owns it and must always call http_response_free. */
int  http_request(HttpClient *c, const char *method, const char *url,
                  const char *content_type, const char *body,
                  HttpResponse *out);
void http_response_free(HttpResponse *r);

/* URL-encode src into out. 0 ok, -1 truncated. */
int  http_urlencode(const char *src, char *out, size_t cap);

#endif
