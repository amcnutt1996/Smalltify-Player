/* App folder paths, .env parsing and token persistence (data/token.json). */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform.h"

#define ENV_MAX_BYTES   (64 * 1024)
#define TOKEN_MAX_BYTES (64 * 1024)

static int is_blank(char ch)
{
    return ch == ' ' || ch == '\t';
}

static const char *skip_blank(const char *p, const char *end)
{
    while (p < end && is_blank(*p))
        p++;
    return p;
}

/* Parse the value part of one line ([v, end) starts right after '=').
 * Sets *vs and *ve to the value bounds. -1 for an unterminated quote. */
static int parse_value(const char *v, const char *end, const char **vs, const char **ve)
{
    v = skip_blank(v, end);
    if (v < end && (*v == '"' || *v == '\'')) {
        const char *q = memchr(v + 1, *v, (size_t)(end - v - 1));
        if (!q)
            return -1;
        *vs = v + 1;
        *ve = q;
        return 0;
    }
    /* Unquoted: '#' starts a comment at the value start or after a blank,
     * so IDs/URLs containing '#' still survive. */
    const char *e = v;
    while (e < end && !(*e == '#' && (e == v || is_blank(e[-1]))))
        e++;
    while (e > v && is_blank(e[-1]))
        e--;
    *vs = v;
    *ve = e;
    return 0;
}

int config_env_lookup(const char *text, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    int found = -1;
    const char *p = text;
    if (!strncmp(p, "\xEF\xBB\xBF", 3)) /* Notepad-style UTF-8 BOM */
        p += 3;
    while (*p) {
        const char *eol = strchr(p, '\n');
        if (!eol)
            eol = p + strlen(p);
        const char *end = eol;
        if (end > p && end[-1] == '\r')
            end--;
        const char *s = skip_blank(p, end);
        if ((size_t)(end - s) > 7 && !strncmp(s, "export", 6) && is_blank(s[6]))
            s = skip_blank(s + 6, end);
        if ((size_t)(end - s) > klen && !strncmp(s, key, klen)) {
            const char *eq = skip_blank(s + klen, end), *vs, *ve;
            if (eq < end && *eq == '=' && parse_value(eq + 1, end, &vs, &ve) == 0) {
                size_t n = (size_t)(ve - vs);
                if (n < cap) {
                    memcpy(out, vs, n);
                    out[n] = '\0';
                    found = 0;
                } else {
                    found = -1; /* the last assignment wins, even a bad one */
                }
            }
        }
        p = *eol ? eol + 1 : eol;
    }
    return found;
}

/* The env var wins; else the .env line. Too long or absent leaves "". */
static void load_setting(const char *env_text, const char *key, char *out, size_t cap)
{
    const char *env = getenv(key);
    if (env && env[0]) {
        int n = snprintf(out, cap, "%s", env);
        if (n < 0 || (size_t)n >= cap)
            out[0] = '\0';
        return;
    }
    if (!env_text || config_env_lookup(env_text, key, out, cap) != 0)
        out[0] = '\0';
}

int config_load(Config *c)
{
    memset(c, 0, sizeof *c);
    const char *exe = plat_exe_dir();
    if (!exe)
        return -1;
    char env_path[CFG_PATH_LEN];
    int n = snprintf(c->app_dir, sizeof c->app_dir, "%s", exe);
    if (n < 0 || (size_t)n >= sizeof c->app_dir ||
        plat_path_join(c->data_dir, sizeof c->data_dir, c->app_dir, "data") != 0 ||
        plat_path_join(c->token_path, sizeof c->token_path, c->data_dir, "token.json") != 0 ||
        plat_path_join(c->soloist_dir, sizeof c->soloist_dir, c->app_dir, "soloist") != 0 ||
        plat_path_join(c->soloist_path, sizeof c->soloist_path, c->soloist_dir,
                       "soloist" PLAT_EXE_SUFFIX) != 0 ||
        plat_path_join(c->soloist_data, sizeof c->soloist_data, c->data_dir, "soloist") != 0 ||
        plat_path_join(c->soloist_cache, sizeof c->soloist_cache, c->data_dir,
                       "soloist-cache") != 0 ||
        plat_path_join(env_path, sizeof env_path, c->app_dir, ".env") != 0)
        return -1;
    snprintf(c->redirect_uri, sizeof c->redirect_uri, "http://127.0.0.1:%d/callback",
             CFG_REDIRECT_PORT);

    /* Not fatal: a read-only app folder still allows `help`; saving the
     * token reports the failure later. */
    (void)plat_mkdir(c->data_dir);

    /* Only these two keys are extracted; every other one (secrets
     * included) is ignored and the text is released right away. */
    char *text = plat_read_file(env_path, ENV_MAX_BYTES, NULL);
    load_setting(text, "SPOTIFY_CLIENT_ID", c->client_id, sizeof c->client_id);
    load_setting(text, "SOLOIST_API_KEY", c->soloist_key, sizeof c->soloist_key);
    free(text);
    return 0;
}

int config_load_token(const Config *c, Token *t)
{
    memset(t, 0, sizeof *t);
    char *text = plat_read_file(c->token_path, TOKEN_MAX_BYTES, NULL);
    if (!text)
        return -1;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root)
        return -1;
    const cJSON *acc = cJSON_GetObjectItemCaseSensitive(root, "access_token");
    const cJSON *ref = cJSON_GetObjectItemCaseSensitive(root, "refresh_token");
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires_at");
    int rc = -1;
    /* The refresh token is what keeps us logged in; the rest is optional. */
    if (cJSON_IsString(ref) && ref->valuestring[0] &&
        strlen(ref->valuestring) < sizeof t->refresh) {
        snprintf(t->refresh, sizeof t->refresh, "%s", ref->valuestring);
        if (cJSON_IsString(acc) && strlen(acc->valuestring) < sizeof t->access &&
            cJSON_IsNumber(exp)) {
            snprintf(t->access, sizeof t->access, "%s", acc->valuestring);
            t->expires_at = (int64_t)exp->valuedouble;
        }
        rc = 0;
    }
    cJSON_Delete(root);
    if (rc != 0)
        memset(t, 0, sizeof *t);
    return rc;
}

int config_save_token(const Config *c, const Token *t)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return -1;
    int rc = -1;
    if (cJSON_AddStringToObject(root, "access_token", t->access) &&
        cJSON_AddStringToObject(root, "refresh_token", t->refresh) &&
        cJSON_AddNumberToObject(root, "expires_at", (double)t->expires_at)) {
        char *json = cJSON_PrintUnformatted(root);
        if (json) {
            if (plat_mkdir(c->data_dir) == 0)
                rc = plat_write_file_atomic(c->token_path, json, strlen(json));
            cJSON_free(json);
        }
    }
    cJSON_Delete(root);
    return rc;
}

int config_clear_login(const Config *c)
{
    int a = plat_remove_tree(c->token_path);
    int b = plat_remove_tree(c->soloist_data); /* the paired Connect session */
    return (a == 0 && b == 0) ? 0 : -1;
}
