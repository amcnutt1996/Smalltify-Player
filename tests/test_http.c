#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "http.h"

static void expect(const char *src, size_t cap, int rc, const char *want)
{
    char out[256];
    assert(cap <= sizeof out);
    memset(out, 'x', sizeof out);
    assert(http_urlencode(src, out, cap) == rc);
    assert(strcmp(out, want) == 0);
}

/* The body cap, exercised offline: this source file served via file://. */
static void test_max_body(void)
{
    FILE *f = fopen(__FILE__, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size > 0);
    fclose(f);

    char url[1024];
    int n = snprintf(url, sizeof url, "file://%s", __FILE__);
    assert(n > 0 && (size_t)n < sizeof url && __FILE__[0] == '/');

    HttpClient *c = http_client_new(NULL);
    assert(c);
    HttpResponse r;
    const size_t caps[] = { 0, (size_t)size, (size_t)size - 1, 16 };
    const int rcs[] = { 0, 0, -1, -1 };
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        http_client_set_max_body(c, caps[i]);
        assert(http_request(c, "GET", url, NULL, NULL, &r) == rcs[i]);
        assert(rcs[i] != 0 || (r.len == (size_t)size && r.body));
        assert(rcs[i] == 0 || (!r.body && r.len == 0));
        http_response_free(&r);
    }
    http_client_free(c);
}

int main(void)
{
    /* RFC 3986 unreserved characters pass through untouched. */
    expect("AZaz09-._~", 64, 0, "AZaz09-._~");
    expect("", 1, 0, "");

    /* Space is %20, never '+' (Spotify query strings and form bodies). */
    expect("daft punk", 64, 0, "daft%20punk");

    /* Every reserved / sub-delim character is escaped, uppercase hex. */
    expect(":/?#[]@!$&'()*+,;=%", 128, 0,
           "%3A%2F%3F%23%5B%5D%40%21%24%26%27%28%29%2A%2B%2C%3B%3D%25");

    /* UTF-8 is encoded byte by byte; high and control bytes too. */
    expect("Beyonc\xC3\xA9", 64, 0, "Beyonc%C3%A9");
    expect("\xE2\x9C\x93", 64, 0, "%E2%9C%93");
    expect("\x01\x7F\xFF", 64, 0, "%01%7F%FF");

    /* Exact fit: output plus NUL fills the buffer. */
    expect("abc", 4, 0, "abc");
    expect("a b", 6, 0, "a%20b");

    /* Truncation reports -1, stays NUL-terminated, never splits a %XX. */
    expect("abc", 3, -1, "ab");
    expect("a b", 4, -1, "a");
    expect("a b", 5, -1, "a%20");
    expect("\xC3\xA9", 6, -1, "%C3");
    expect("abc", 1, -1, "");

    /* Degenerate arguments. */
    {
        char out[4] = "zzz";
        assert(http_urlencode("a", out, 0) == -1);
        assert(strcmp(out, "zzz") == 0);
        assert(http_urlencode(NULL, out, sizeof out) == -1);
        assert(out[0] == '\0');
        assert(http_urlencode("a", NULL, 8) == -1);
    }

    assert(http_global_init() == 0);
    test_max_body();
    http_global_cleanup();
    return 0;
}
