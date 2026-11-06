/* PKCE S256 challenge = base64url(SHA-256(verifier)), checked against
 * published vectors. */
#include <assert.h>
#include <string.h>

#include "auth.h"

static void check(const char *verifier, const char *expected)
{
    char out[64];
    assert(auth_pkce_challenge(verifier, out, sizeof out) == 0);
    assert(strcmp(out, expected) == 0);
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
    return 0;
}
