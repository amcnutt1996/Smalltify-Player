/* Spotify Authorization Code + PKCE (no client secret) and the shared,
 * thread-safe token store. */
#include "auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"

#define AUTH_URL  "https://accounts.spotify.com/authorize"
#define TOKEN_URL "https://accounts.spotify.com/api/token"
#define FORM_TYPE "application/x-www-form-urlencoded"

#define LOGIN_TIMEOUT_MS  (5u * 60u * 1000u)
#define CANCEL_POLL_MS    250u   /* how quickly *cancel is noticed */
#define CLIENT_RECV_MS    2000u  /* per browser connection, to read the request */
#define EXPIRY_MARGIN_S   60     /* refresh this long before expiry */
#define RECENT_REFRESH_MS 10000u /* 401s right after a refresh reuse it */

/* ---------------------------------------------------------------------- */
/* SHA-256 (FIPS 180-4) + base64url: only needed for the S256 challenge,  */
/* so a compact one-shot version beats pulling in a crypto library.       */
/* ---------------------------------------------------------------------- */
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

static void sha256(const void *data, size_t len, unsigned char out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const unsigned char *p = data;
    size_t left = len;
    for (; left >= 64; p += 64, left -= 64)
        sha256_block(h, p);
    unsigned char tail[128] = { 0 };
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tlen = left < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8u;
    for (int i = 0; i < 8; i++)
        tail[tlen - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tlen == 128)
        sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (unsigned char)(h[i] >> 24);
        out[4 * i + 1] = (unsigned char)(h[i] >> 16);
        out[4 * i + 2] = (unsigned char)(h[i] >> 8);
        out[4 * i + 3] = (unsigned char)h[i];
    }
}

/* Unpadded base64url (RFC 4648 §5). -1 if out is too small. */
static int base64url(const unsigned char *in, size_t len, char *out, size_t cap)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t need = (len * 4 + 2) / 3;
    if (cap <= need)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = tbl[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = tbl[v & 63];
    }
    out[o] = '\0';
    return 0;
}

int auth_pkce_challenge(const char *verifier, char *out, size_t cap)
{
    unsigned char d[32];
    sha256(verifier, strlen(verifier), d);
    return base64url(d, sizeof d, out, cap);
}

/* n random bytes -> base64url text (48 bytes -> the 64-char verifier). */
static int random_b64url(size_t n, char *out, size_t cap)
{
    unsigned char raw[48];
    if (n > sizeof raw || plat_random_bytes(raw, n) != 0)
        return -1;
    return base64url(raw, n, out, cap);
}

/* ---------------------------------------------------------------------- */
/* Token store                                                            */
/* ---------------------------------------------------------------------- */
struct AuthStore {
    Config      cfg;
    plat_mutex *lock;      /* guards tok, gen, refreshed_ms */
    plat_mutex *flight;    /* single-flight for token endpoint calls + http */
    HttpClient *http;      /* unauthenticated, used only under flight */
    Token       tok;
    unsigned    gen;       /* bumped on every token change */
    uint64_t    refreshed_ms;
    AuthUrlFn   on_url;    /* optional sign-in URL observer */
    void       *on_url_ud;
};

AuthStore *auth_store_new(const Config *cfg)
{
    AuthStore *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    s->lock = plat_mutex_new();
    s->flight = plat_mutex_new();
    if (!s->lock || !s->flight) {
        auth_store_free(s);
        return NULL;
    }
    if (config_load_token(cfg, &s->tok) != 0)
        memset(&s->tok, 0, sizeof s->tok);
    return s;
}

void auth_store_free(AuthStore *s)
{
    if (!s)
        return;
    http_client_free(s->http);
    plat_mutex_free(s->lock);
    plat_mutex_free(s->flight);
    memset(&s->tok, 0, sizeof s->tok);
    free(s);
}

void auth_store_set_url_callback(AuthStore *s, AuthUrlFn fn, void *ud)
{
    s->on_url = fn;
    s->on_url_ud = ud;
}

int auth_store_logged_in(AuthStore *s)
{
    plat_mutex_lock(s->lock);
    int in = s->tok.refresh[0] != '\0';
    plat_mutex_unlock(s->lock);
    return in;
}

/* Length of the scope word at s (up to a blank or the end). */
static size_t scope_len(const char *s)
{
    return strcspn(s, " \t");
}

int auth_scopes_cover(const char *granted, const char *want)
{
    const char *w = want ? want : "";
    if (!granted)
        granted = "";
    for (;;) {
        w += strspn(w, " \t");
        if (!*w)
            return 1;
        size_t n = scope_len(w);
        const char *g = granted + strspn(granted, " \t");
        while (*g && !(scope_len(g) == n && !strncmp(g, w, n))) {
            g += scope_len(g);
            g += strspn(g, " \t");
        }
        if (!*g)
            return 0;
        w += n;
    }
}

int auth_store_needs_reconsent(AuthStore *s)
{
    plat_mutex_lock(s->lock);
    int need = s->tok.refresh[0] && !auth_scopes_cover(s->tok.scope, AUTH_SCOPES);
    plat_mutex_unlock(s->lock);
    return need;
}

/* Caller holds s->flight. POST a form to the token endpoint and merge the
 * reply into a copy of the current token. 0 ok, -1 transport/HTTP error,
 * -2 when Spotify rejected the grant (revoked refresh token, bad code). */
static int token_request(AuthStore *s, const char *form, Token *out)
{
    if (!s->http && !(s->http = http_client_new(NULL)))
        return -1;
    HttpResponse r = { 0, NULL, 0 };
    if (http_request(s->http, "POST", TOKEN_URL, FORM_TYPE, form, &r) != 0) {
        http_response_free(&r);
        fprintf(stderr, "auth: cannot reach accounts.spotify.com\n");
        return -1;
    }
    int rc = -1;
    cJSON *root = r.body ? cJSON_Parse(r.body) : NULL;
    if (r.status == 200 && root) {
        const cJSON *acc = cJSON_GetObjectItemCaseSensitive(root, "access_token");
        const cJSON *ref = cJSON_GetObjectItemCaseSensitive(root, "refresh_token");
        const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
        const cJSON *scope = cJSON_GetObjectItemCaseSensitive(root, "scope");
        if (cJSON_IsString(acc) && strlen(acc->valuestring) < sizeof out->access) {
            snprintf(out->access, sizeof out->access, "%s", acc->valuestring);
            /* Spotify may rotate the refresh token; keep the old one otherwise. */
            if (cJSON_IsString(ref) && ref->valuestring[0] &&
                strlen(ref->valuestring) < sizeof out->refresh)
                snprintf(out->refresh, sizeof out->refresh, "%s", ref->valuestring);
            /* What the grant covers (refreshes repeat it); kept if absent. */
            if (cJSON_IsString(scope) && strlen(scope->valuestring) < sizeof out->scope)
                snprintf(out->scope, sizeof out->scope, "%s", scope->valuestring);
            out->expires_at = plat_unix_time() + (cJSON_IsNumber(exp) ? (int64_t)exp->valuedouble : 3600);
            rc = out->refresh[0] ? 0 : -1;
        }
    } else {
        const cJSON *err = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
        const char *code = cJSON_IsString(err) ? err->valuestring : "unknown";
        fprintf(stderr, "auth: token request failed (HTTP %ld, %s)\n", r.status, code);
        if (r.status == 400 && !strcmp(code, "invalid_grant"))
            rc = -2;
    }
    cJSON_Delete(root);
    http_response_free(&r);
    return rc;
}

/* Caller holds s->flight: publish a new token in memory and on disk. */
static void store_token(AuthStore *s, const Token *t)
{
    plat_mutex_lock(s->lock);
    s->tok = *t;
    s->gen++;
    s->refreshed_ms = plat_now_ms();
    plat_mutex_unlock(s->lock);
    if (config_save_token(&s->cfg, t) != 0)
        fprintf(stderr, "auth: cannot save %s\n", s->cfg.token_path);
}

/* Refresh unless someone else already did since generation `seen`
 * (single-flight: concurrent callers wait on `flight` and reuse it). */
static int refresh_since(AuthStore *s, unsigned seen, int force)
{
    plat_mutex_lock(s->flight);
    plat_mutex_lock(s->lock);
    Token t = s->tok;
    int fresh = s->gen != seen ||
                (!force && t.access[0] && t.expires_at - EXPIRY_MARGIN_S > plat_unix_time());
    plat_mutex_unlock(s->lock);
    int rc = 0;
    if (!fresh) {
        char ref[3 * sizeof t.refresh], cid[3 * sizeof s->cfg.client_id];
        char form[sizeof ref + sizeof cid + 64];
        if (!t.refresh[0] || !s->cfg.client_id[0] ||
            http_urlencode(t.refresh, ref, sizeof ref) != 0 ||
            http_urlencode(s->cfg.client_id, cid, sizeof cid) != 0) {
            rc = -1;
        } else {
            snprintf(form, sizeof form, "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                     ref, cid);
            rc = token_request(s, form, &t);
            if (rc == 0) {
                store_token(s, &t);
            } else if (rc == -2) {
                /* Revoked: forget it so the UI asks for a new login. */
                Token none;
                memset(&none, 0, sizeof none);
                plat_mutex_lock(s->lock);
                s->tok = none;
                s->gen++;
                plat_mutex_unlock(s->lock);
                plat_remove_tree(s->cfg.token_path);
                rc = -1;
            }
        }
        memset(form, 0, sizeof form);
    }
    memset(&t, 0, sizeof t);
    plat_mutex_unlock(s->flight);
    return rc;
}

/* HttpAuth.get_token: copy a valid access token, refreshing first if it
 * expires within EXPIRY_MARGIN_S. 0 ok. */
static int http_get_token(void *ud, char *buf, size_t cap)
{
    AuthStore *s = ud;
    for (int attempt = 0; attempt < 2; attempt++) {
        plat_mutex_lock(s->lock);
        int ok = s->tok.access[0] && s->tok.expires_at - EXPIRY_MARGIN_S > plat_unix_time();
        unsigned gen = s->gen;
        int n = -1;
        if (ok)
            n = snprintf(buf, cap, "%s", s->tok.access);
        int have_refresh = s->tok.refresh[0] != '\0';
        plat_mutex_unlock(s->lock);
        if (ok)
            return (n >= 0 && (size_t)n < cap) ? 0 : -1;
        if (!have_refresh || attempt == 1 || refresh_since(s, gen, 0) != 0)
            break;
    }
    return -1;
}

void auth_store_logout(AuthStore *s)
{
    plat_mutex_lock(s->flight);
    plat_mutex_lock(s->lock);
    memset(&s->tok, 0, sizeof s->tok);
    s->gen++;
    plat_mutex_unlock(s->lock);
    if (config_clear_login(&s->cfg) != 0)
        fprintf(stderr, "auth: could not remove all login data under %s\n", s->cfg.data_dir);
    plat_mutex_unlock(s->flight);
}

/* After a 401 the access token is bad even if not expired. Several threads
 * can hit that at once; the first refreshes, the rest reuse its result. */
static int http_refresh(void *ud)
{
    AuthStore *s = ud;
    plat_mutex_lock(s->lock);
    unsigned gen = s->gen;
    int recent = s->refreshed_ms && plat_now_ms() - s->refreshed_ms < RECENT_REFRESH_MS;
    plat_mutex_unlock(s->lock);
    if (recent)
        return 0;
    return refresh_since(s, gen, 1);
}

HttpAuth auth_store_http(AuthStore *s)
{
    HttpAuth a = { http_get_token, http_refresh, s };
    return a;
}

/* ---------------------------------------------------------------------- */
/* Interactive login: loopback redirect listener                          */
/* ---------------------------------------------------------------------- */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Find `key` in a query string [q, end) and percent-decode its value.
 * 0 found, -1 absent/too long. */
static int query_param(const char *q, const char *end, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    while (q < end) {
        const char *amp = memchr(q, '&', (size_t)(end - q));
        const char *pe = amp ? amp : end;
        if ((size_t)(pe - q) > klen && !strncmp(q, key, klen) && q[klen] == '=') {
            size_t o = 0;
            for (const char *v = q + klen + 1; v < pe; v++) {
                char ch = *v;
                if (ch == '+') {
                    ch = ' ';
                } else if (ch == '%' && pe - v > 2 && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    ch = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 2;
                }
                if (o + 1 >= cap)
                    return -1;
                out[o++] = ch;
            }
            out[o] = '\0';
            return 0;
        }
        q = pe + 1;
    }
    return -1;
}

static void reply(plat_sock c, const char *status, const char *msg)
{
    char body[512], head[256];
    int bn = snprintf(body, sizeof body,
                      "<!doctype html><html><head><meta charset=\"utf-8\">"
                      "<title>Smalltify Player</title></head>"
                      "<body style=\"font-family:sans-serif;text-align:center;margin-top:3em\">"
                      "<h2>Smalltify Player</h2><p>%s</p><p>You can close this tab.</p>"
                      "</body></html>", msg);
    if (bn < 0 || (size_t)bn >= sizeof body)
        return;
    int hn = snprintf(head, sizeof head,
                      "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Content-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      status, bn);
    if (hn < 0 || (size_t)hn >= sizeof head)
        return;
    if (plat_send(c, head, (size_t)hn) >= 0)
        (void)plat_send(c, body, (size_t)bn);
}

typedef enum { CB_IGNORE, CB_CODE, CB_DENIED } CallbackResult;

/* Handle one browser connection. Only a /callback carrying our `state`
 * counts; anything else (favicon, stale tabs, forged requests) is answered
 * and ignored so it cannot end or hijack the login. */
static CallbackResult handle_client(plat_sock c, const char *state, char *code, size_t cap)
{
    char req[4096];
    size_t len = 0;
    /* Total budget, so a trickling peer cannot stall cancel handling. */
    uint64_t until = plat_now_ms() + CLIENT_RECV_MS;
    while (len + 1 < sizeof req) {
        uint64_t now = plat_now_ms();
        if (now >= until)
            break;
        long n = plat_recv(c, req + len, sizeof req - 1 - len, (unsigned)(until - now));
        if (n <= 0)
            break;
        len += (size_t)n;
        req[len] = '\0';
        if (strchr(req, '\n'))
            break; /* the request line is all we need */
    }
    req[len] = '\0';
    const char *eol = strpbrk(req, "\r\n");
    if (!eol || strncmp(req, "GET ", 4)) {
        reply(c, "400 Bad Request", "Unexpected request.");
        return CB_IGNORE;
    }
    const char *path = req + 4;
    const char *pend = memchr(path, ' ', (size_t)(eol - path));
    if (!pend)
        pend = eol;
    if ((size_t)(pend - path) < 10 || strncmp(path, "/callback?", 10)) {
        reply(c, "404 Not Found", "Nothing here.");
        return CB_IGNORE;
    }
    const char *q = path + 10;
    char got_state[128], err[128];
    if (query_param(q, pend, "state", got_state, sizeof got_state) != 0 ||
        strcmp(got_state, state) != 0) {
        reply(c, "400 Bad Request", "Login rejected: state mismatch. Start the login again from the app.");
        return CB_IGNORE;
    }
    if (query_param(q, pend, "error", err, sizeof err) == 0) {
        reply(c, "200 OK", "Login was cancelled or denied.");
        fprintf(stderr, "auth: authorization denied (%s)\n", err);
        return CB_DENIED;
    }
    if (query_param(q, pend, "code", code, cap) != 0 || !code[0]) {
        reply(c, "400 Bad Request", "Login failed: no authorization code.");
        return CB_IGNORE;
    }
    reply(c, "200 OK", "Login complete.");
    return CB_CODE;
}

/* Wait for the redirect; 0 with code filled, else AUTH_ERR_*. */
static int wait_for_code(plat_sock ls, const char *state, char *code, size_t cap,
                         const volatile int *cancel)
{
    uint64_t deadline = plat_now_ms() + LOGIN_TIMEOUT_MS;
    while (plat_now_ms() < deadline) {
        if (cancel && *cancel)
            return AUTH_ERR_OTHER;
        plat_sock c = plat_accept(ls, CANCEL_POLL_MS);
        if (c == PLAT_SOCK_INVALID)
            continue;
        CallbackResult r = handle_client(c, state, code, cap);
        plat_close(c);
        if (r == CB_CODE)
            return 0;
        if (r == CB_DENIED)
            return AUTH_ERR_DENIED;
    }
    fprintf(stderr, "auth: login timed out\n");
    return AUTH_ERR_TIMEOUT;
}

const char *auth_login_strerror(int rc)
{
    switch (rc) {
    case AUTH_ERR_PORT:
        return "Port 8888 is in use by another program.";
    case AUTH_ERR_TIMEOUT:
        return "Timed out. Check SPOTIFY_CLIENT_ID in .env.";
    case AUTH_ERR_DENIED:
        return "Access was denied in the browser.";
    case AUTH_ERR_TOKEN:
        return "Spotify did not accept the sign-in.";
    default:
        return "Sign-in failed. Please try again.";
    }
}

int auth_store_login(AuthStore *s, const volatile int *cancel)
{
    const Config *cfg = &s->cfg;
    if (!cfg->client_id[0]) {
        fprintf(stderr, "auth: SPOTIFY_CLIENT_ID is not set in %s%c.env\n", cfg->app_dir, PLAT_SEP);
        return -1;
    }
    char verifier[72], challenge[48], state[32];
    if (random_b64url(48, verifier, sizeof verifier) != 0 ||
        random_b64url(16, state, sizeof state) != 0 ||
        auth_pkce_challenge(verifier, challenge, sizeof challenge) != 0) {
        fprintf(stderr, "auth: no secure random source\n");
        return -1;
    }
    char cid[3 * sizeof cfg->client_id], redir[3 * sizeof cfg->redirect_uri], scope[512];
    if (http_urlencode(cfg->client_id, cid, sizeof cid) != 0 ||
        http_urlencode(cfg->redirect_uri, redir, sizeof redir) != 0 ||
        http_urlencode(AUTH_SCOPES, scope, sizeof scope) != 0)
        return -1;
    char url[2048];
    int n = snprintf(url, sizeof url,
                     AUTH_URL "?client_id=%s&response_type=code&redirect_uri=%s"
                     "&code_challenge_method=S256&code_challenge=%s&state=%s&scope=%s",
                     cid, redir, challenge, state, scope);
    if (n < 0 || (size_t)n >= sizeof url)
        return -1;

    /* Listen before opening the browser so a fast redirect can't miss us. */
    plat_sock ls = plat_listen_loopback(CFG_REDIRECT_PORT);
    if (ls == PLAT_SOCK_INVALID) {
        fprintf(stderr, "auth: cannot listen on 127.0.0.1:%d (port in use?)\n", CFG_REDIRECT_PORT);
        return AUTH_ERR_PORT;
    }
    if (s->on_url) {
        s->on_url(url, s->on_url_ud);
    } else {
        printf("If your browser did not open, visit this URL to sign in:\n%s\n", url);
        fflush(stdout);
    }
    if (plat_open_url(url) != 0)
        fprintf(stderr, "auth: could not open a browser\n");

    char code[1024];
    int rc = wait_for_code(ls, state, code, sizeof code, cancel);
    plat_close(ls);
    if (rc != 0)
        return rc;

    char ecode[3 * sizeof code], form[sizeof ecode + sizeof cid + sizeof redir + 160];
    if (http_urlencode(code, ecode, sizeof ecode) != 0)
        return -1;
    snprintf(form, sizeof form,
             "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
             ecode, redir, cid, verifier);
    Token t;
    memset(&t, 0, sizeof t);
    plat_mutex_lock(s->flight);
    rc = token_request(s, form, &t) == 0 ? 0 : AUTH_ERR_TOKEN;
    if (rc == 0) {
        store_token(s, &t);
        if (!auth_scopes_cover(t.scope, AUTH_SCOPES))
            fprintf(stderr, "auth: Spotify granted only \"%s\"\n", t.scope);
    }
    plat_mutex_unlock(s->flight);
    memset(&t, 0, sizeof t);
    memset(form, 0, sizeof form);
    return rc;
}
