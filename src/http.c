#include "http.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#define HTTP_USER_AGENT        "spotify-c-player/0.1"
#define HTTP_CONNECT_TIMEOUT_MS 5000L
#define HTTP_TOTAL_TIMEOUT_MS   15000L
#define HTTP_RETRY_AFTER_MAX_S  5
#define HTTP_BUF_INITIAL        16384
#define HTTP_TOKEN_LEN          2048 /* matches Token.access in config.h */

struct HttpClient {
    CURL    *h;
    HttpAuth auth;
    int      has_auth;
    /* Receive buffer. Kept across attempts and empty responses; a filled
     * buffer is handed to the caller, so the next body starts a new one. */
    char    *buf;
    size_t   len;
    size_t   cap;
};

int http_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

void http_global_cleanup(void)
{
    curl_global_cleanup();
}

static size_t on_write(char *data, size_t size, size_t nmemb, void *ud)
{
    HttpClient *c = ud;
    size_t n = size * nmemb;
    if (n == 0)
        return 0;
    if (c->cap - c->len <= n) {  /* keep room for the NUL */
        size_t cap = c->cap ? c->cap : HTTP_BUF_INITIAL;
        while (cap - c->len <= n) {
            if (cap > ((size_t)-1) / 2)
                return 0;
            cap *= 2;
        }
        char *p = realloc(c->buf, cap);
        if (!p) {
            return 0; /* aborts the transfer with CURLE_WRITE_ERROR */
        }
        c->buf = p;
        c->cap = cap;
    }
    memcpy(c->buf + c->len, data, n);
    c->len += n;
    return n;
}

HttpClient *http_client_new(const HttpAuth *auth)
{
    HttpClient *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->h = curl_easy_init();
    if (!c->h) {
        free(c);
        return NULL;
    }
    if (auth && auth->get_token) {
        c->auth = *auth;
        c->has_auth = 1;
    }

    CURL *h = c->h;
    int bad = 0;
    /* Signals are not thread-safe; this client runs on worker threads. */
    bad |= curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, HTTP_CONNECT_TIMEOUT_MS) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, HTTP_TOTAL_TIMEOUT_MS) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_USERAGENT, HTTP_USER_AGENT) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_write) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_WRITEDATA, c) != CURLE_OK;
    if (bad) {
        http_client_free(c);
        return NULL;
    }
    /* Nice-to-haves: a libcurl built without zlib/nghttp2/keepalive
     * support still works, just less efficiently. */
    (void)curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");
    (void)curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    (void)curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    return c;
}

void http_client_free(HttpClient *c)
{
    if (!c)
        return;
    curl_easy_cleanup(c->h);
    free(c->buf);
    free(c);
}

void http_response_free(HttpResponse *r)
{
    if (!r)
        return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
    r->status = 0;
}

/* GET and bodiless DELETE go without a request body; everything else is
 * sent POST-style (PUT/DELETE via CUSTOMREQUEST) so no read callback is
 * needed, and an empty body still sends "Content-Length: 0", which Spotify
 * requires for bodiless PUT/POST commands (411 otherwise). */
static int sends_body(const char *method, const char *body)
{
    return strcmp(method, "GET") != 0 && (body || strcmp(method, "DELETE") != 0);
}

/* The handle is reused, so every per-request option is set on every call:
 * method, body, URL and header list. */
static int set_method(CURL *h, const char *method, const char *body)
{
    int bad = 0;
    if (!sends_body(method, body)) {
        bad |= curl_easy_setopt(h, CURLOPT_HTTPGET, 1L) != CURLE_OK;
        bad |= curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST,
                                strcmp(method, "GET") == 0 ? NULL : method) != CURLE_OK;
        return bad ? -1 : 0;
    }
    const char *data = body ? body : "";
    bad |= curl_easy_setopt(h, CURLOPT_POSTFIELDS, data) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(data)) != CURLE_OK;
    bad |= curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST,
                            strcmp(method, "POST") == 0 ? NULL : method) != CURLE_OK;
    return bad ? -1 : 0;
}

/* One round-trip. Returns 0 with *status set, -1 on transport error. */
static int perform(HttpClient *c, const char *method, const char *url,
                   const char *content_type, const char *body, long *status)
{
    struct curl_slist *hdrs = NULL, *tmp;
    char line[HTTP_TOKEN_LEN + 32];
    int rc = -1;

    *status = 0;
    c->len = 0;

    if (c->has_auth) {
        char token[HTTP_TOKEN_LEN];
        /* Not logged in: send without a token and let the server answer
         * 401, which drives the refresh path below. */
        if (c->auth.get_token(c->auth.ud, token, sizeof token) == 0) {
            int n = snprintf(line, sizeof line, "Authorization: Bearer %s", token);
            if (n < 0 || (size_t)n >= sizeof line)
                goto out;
            if (!(tmp = curl_slist_append(hdrs, line)))
                goto out;
            hdrs = tmp;
        }
    }
    if (content_type) {
        int n = snprintf(line, sizeof line, "Content-Type: %s", content_type);
        if (n < 0 || (size_t)n >= sizeof line)
            goto out;
        if (!(tmp = curl_slist_append(hdrs, line)))
            goto out;
        hdrs = tmp;
    } else if (!body && sends_body(method, body)) {
        /* Drop libcurl's default form Content-Type on bodiless PUT/POST. */
        if (!(tmp = curl_slist_append(hdrs, "Content-Type:")))
            goto out;
        hdrs = tmp;
    }

    if (set_method(c->h, method, body) != 0 ||
        curl_easy_setopt(c->h, CURLOPT_URL, url) != CURLE_OK ||
        curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, hdrs) != CURLE_OK)
        goto out;

    if (curl_easy_perform(c->h) == CURLE_OK &&
        curl_easy_getinfo(c->h, CURLINFO_RESPONSE_CODE, status) == CURLE_OK)
        rc = 0;
    else
        *status = 0;

out:
    /* Never leave the handle pointing at a list we are about to free. */
    (void)curl_easy_setopt(c->h, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(hdrs);
    return rc;
}

static unsigned retry_after_ms(CURL *h)
{
    curl_off_t s = 0;
    if (curl_easy_getinfo(h, CURLINFO_RETRY_AFTER, &s) != CURLE_OK || s <= 0)
        s = 1; /* no usable header: back off briefly rather than hammer */
    if (s > HTTP_RETRY_AFTER_MAX_S)
        s = HTTP_RETRY_AFTER_MAX_S;
    return (unsigned)s * 1000u;
}

int http_request(HttpClient *c, const char *method, const char *url,
                 const char *content_type, const char *body,
                 HttpResponse *out)
{
    int refreshed = 0, rate_retried = 0;
    long status = 0;

    if (!out)
        return -1;
    out->status = 0;
    out->body = NULL;
    out->len = 0;
    if (!c || !method || !url)
        return -1;

    for (;;) {
        if (perform(c, method, url, content_type, body, &status) != 0)
            return -1;
        if (status == 401 && c->has_auth && !refreshed && c->auth.refresh) {
            refreshed = 1;
            if (c->auth.refresh(c->auth.ud) == 0)
                continue;
        } else if (status == 429 && !rate_retried) {
            rate_retried = 1;
            plat_sleep_ms(retry_after_ms(c->h));
            continue;
        }
        break;
    }

    out->status = status;
    if (c->len > 0) {
        /* Hand the buffer over instead of copying it. */
        c->buf[c->len] = '\0';
        out->body = c->buf;
        out->len = c->len;
        c->buf = NULL;
        c->cap = 0;
        c->len = 0;
    }
    return 0;
}

int http_urlencode(const char *src, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (!out || cap == 0)
        return -1;
    out[0] = '\0';
    if (!src)
        return -1;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        unsigned char ch = *p;
        int plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '-' || ch == '.' || ch == '_' || ch == '~';
        size_t need = plain ? 1 : 3;
        if (cap - o <= need) {
            out[o] = '\0'; /* never leave a partial %XX */
            return -1;
        }
        if (plain) {
            out[o++] = (char)ch;
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 0x0F];
        }
    }
    out[o] = '\0';
    return 0;
}
