/* PKCE S256 challenge = base64url(SHA-256(verifier)), checked against
 * published vectors; and the granted-scope check that asks sign-ins from
 * before a scope was added for a new consent. Files go only to a scratch
 * folder next to the test binary (inside build/). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "config.h"
#include "platform.h"

/* What builds before the library features asked for. */
#define OLD_SCOPES "user-read-playback-state user-modify-playback-state " \
                   "user-read-currently-playing playlist-read-private " \
                   "playlist-read-collaborative user-library-read"

static void check(const char *verifier, const char *expected)
{
    char out[64];
    assert(auth_pkce_challenge(verifier, out, sizeof out) == 0);
    assert(strcmp(out, expected) == 0);
}

static void test_scopes_cover(void)
{
    assert(auth_scopes_cover("a b c", "a c") == 1);
    assert(auth_scopes_cover("c b a", "a b c") == 1); /* any order */
    assert(auth_scopes_cover("a b", "a c") == 0);
    assert(auth_scopes_cover("", "a") == 0);
    assert(auth_scopes_cover(NULL, "a") == 0);
    assert(auth_scopes_cover("a", "") == 1 && auth_scopes_cover("a", NULL) == 1);
    assert(auth_scopes_cover("  a   b ", " b  a ") == 1);
    /* Whole words only. */
    assert(auth_scopes_cover("user-library-read", "user-library") == 0);
    assert(auth_scopes_cover("user-library-readx user-library", "user-library-read") == 0);
    /* The new features' scopes are asked for; an older grant lacks them. */
    assert(auth_scopes_cover(AUTH_SCOPES, "user-library-modify user-follow-read "
                                          "user-follow-modify user-read-recently-played") == 1);
    assert(auth_scopes_cover(AUTH_SCOPES, OLD_SCOPES) == 1);
    assert(auth_scopes_cover(OLD_SCOPES, AUTH_SCOPES) == 0);
}

static void save_token(const Config *c, const char *scope)
{
    Token t;
    memset(&t, 0, sizeof t);
    snprintf(t.access, sizeof t.access, "BQD-access");
    snprintf(t.refresh, sizeof t.refresh, "AQB-refresh");
    t.expires_at = 1893456000; /* 2030-01-01: no refresh needed */
    snprintf(t.scope, sizeof t.scope, "%s", scope);
    assert(config_save_token(c, &t) == 0);
}

/* 1 if a store opened on the saved token wants a new consent. */
static int reconsent(const Config *c, int *logged_in)
{
    AuthStore *s = auth_store_new(c);
    assert(s);
    *logged_in = auth_store_logged_in(s);
    int need = auth_store_needs_reconsent(s);
    auth_store_free(s);
    return need;
}

static void test_reconsent(void)
{
    const char *exe = plat_exe_dir();
    char root[CFG_PATH_LEN];
    Config c;
    Token t;
    int in;

    assert(exe != NULL);
    assert(plat_path_join(root, sizeof root, exe, "test_auth_scratch") == 0);
    assert(plat_remove_tree(root) == 0);
    assert(plat_mkdir(root) == 0);
    memset(&c, 0, sizeof c);
    assert(plat_path_join(c.data_dir, sizeof c.data_dir, root, "data") == 0);
    assert(plat_path_join(c.token_path, sizeof c.token_path, c.data_dir, "token.json") == 0);
    assert(plat_path_join(c.soloist_data, sizeof c.soloist_data, c.data_dir, "soloist") == 0);
    assert(plat_path_join(c.paired_path, sizeof c.paired_path, c.data_dir, "soloist-paired") == 0);
    assert(plat_path_join(c.name_path, sizeof c.name_path, c.data_dir, "device-name") == 0);

    /* Signed out: nothing to ask for again. */
    assert(reconsent(&c, &in) == 0 && !in);

    /* A token saved before the granted scopes were recorded (no "scope"
     * key) counts as lacking the new ones, but stays signed in. */
    static const char legacy[] =
        "{\"access_token\":\"BQD\",\"refresh_token\":\"AQB\",\"expires_at\":1893456000}";
    assert(plat_mkdir(c.data_dir) == 0);
    assert(plat_write_file_atomic(c.token_path, legacy, strlen(legacy)) == 0);
    assert(config_load_token(&c, &t) == 0 && t.scope[0] == '\0');
    assert(reconsent(&c, &in) == 1 && in);

    /* What the previous build was granted: still one new consent. */
    save_token(&c, OLD_SCOPES);
    assert(config_load_token(&c, &t) == 0 && !strcmp(t.scope, OLD_SCOPES));
    assert(reconsent(&c, &in) == 1 && in);

    /* After the new sign-in: complete (extra scopes are fine), also after a
     * reload from disk. */
    save_token(&c, AUTH_SCOPES " user-read-email");
    assert(config_load_token(&c, &t) == 0 && strstr(t.scope, "user-follow-read"));
    assert(reconsent(&c, &in) == 0 && in);

    /* Logout forgets the grant with the token. */
    AuthStore *s = auth_store_new(&c);
    assert(s);
    auth_store_logout(s);
    assert(!auth_store_logged_in(s) && !auth_store_needs_reconsent(s));
    auth_store_free(s);
    assert(config_load_token(&c, &t) == -1);
    assert(plat_remove_tree(root) == 0);
}

int main(void)
{
    /* RFC 7636 appendix B (the verifier has '_' after "p1r": it is the
     * base64url of the octets listed there). */
    check("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk",
          "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    /* FIPS 180-2 "abc": ba7816bf...f20015ad, base64url-encoded. */
    check("abc", "ungWv48Bz-pBQUDeXa4iI7ADYaOWF3qctBD_YfIAFa0");
    /* Empty input: e3b0c442...7852b855. */
    check("", "47DEQpj8HBSa-_TImW-5JCeuQeRkm5NMpJWZG3hSuFU");
    /* 56 bytes: the length no longer fits, padding spills into a 2nd block. */
    check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "JI1qYdIGOLjlwCaTDD5gOaM85Flk_yFn9uzt1BnbBsE");
    /* 64 chars, the length of the verifiers we generate (one full block). */
    check("0123456789012345678901234567890123456789012345678901234567890123",
          "lnTZ4HhTW3zsQyhDh6buOZVhiOc1qFRSsAULVTQc2lY");

    /* 43 chars + NUL needed; a smaller buffer must fail, not truncate. */
    char small[43];
    assert(auth_pkce_challenge("abc", small, sizeof small) == -1);
    char exact[44];
    assert(auth_pkce_challenge("abc", exact, sizeof exact) == 0);

    test_scopes_cover();
    test_reconsent();
    return 0;
}
