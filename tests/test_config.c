/* .env parsing edge cases and token persistence. Files go only to a
 * scratch folder next to the test binary (inside build/). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "platform.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

static const char *lookup(const char *text, const char *key)
{
    static char out[128];
    return config_env_lookup(text, key, out, sizeof out) == 0 ? out : NULL;
}

static void expect(const char *text, const char *want)
{
    const char *got = lookup(text, "SPOTIFY_CLIENT_ID");
    if (!want) {
        assert(got == NULL);
    } else {
        assert(got != NULL);
        assert(strcmp(got, want) == 0);
    }
}

static void test_env(void)
{
    expect("SPOTIFY_CLIENT_ID=abc123\n", "abc123");
    expect("SPOTIFY_CLIENT_ID=abc123", "abc123");                 /* no final newline */
    expect("  SPOTIFY_CLIENT_ID \t=  abc123  \n", "abc123");     /* whitespace */
    expect("SPOTIFY_CLIENT_ID=abc123\r\n", "abc123");             /* CRLF */
    expect("\xEF\xBB\xBF" "SPOTIFY_CLIENT_ID=abc\r\n", "abc");    /* UTF-8 BOM */
    expect("SPOTIFY_CLIENT_ID=\"abc 123\"\n", "abc 123");         /* double quotes */
    expect("SPOTIFY_CLIENT_ID='a # b'\n", "a # b");               /* single quotes keep '#' */
    expect("SPOTIFY_CLIENT_ID=abc # my app\n", "abc");            /* inline comment */
    expect("SPOTIFY_CLIENT_ID=abc#def\n", "abc#def");             /* '#' inside a value */
    expect("SPOTIFY_CLIENT_ID=\n", "");                           /* empty value */
    expect("SPOTIFY_CLIENT_ID= # nothing\n", "");
    expect("export SPOTIFY_CLIENT_ID=abc\n", "abc");
    expect("# SPOTIFY_CLIENT_ID=commented\n", NULL);
    expect("   # SPOTIFY_CLIENT_ID=commented\n", NULL);
    expect("", NULL);
    expect("\n\r\n\n", NULL);
    expect("SPOTIFY_CLIENT_ID\n", NULL);                          /* no '=' */
    expect("SPOTIFY_CLIENT_ID=\"unterminated\n", NULL);
    expect("SPOTIFY_CLIENT_IDX=nope\nXSPOTIFY_CLIENT_ID=nope\n", NULL); /* prefix/suffix keys */
    expect("SPOTIFY_CLIENT_ID=first\nSPOTIFY_CLIENT_ID=second\n", "second"); /* last wins */
    expect("A=1\r\nSPOTIFY_CLIENT_SECRET=hidden\r\nSPOTIFY_CLIENT_ID=id42\r\nB=2", "id42");
    assert(lookup("SPOTIFY_CLIENT_SECRET=hidden\n", "SPOTIFY_CLIENT_ID") == NULL);
    const char *both = "SPOTIFY_CLIENT_ID=id42\nSOLOIST_API_KEY=key7\n";
    assert(strcmp(lookup(both, "SOLOIST_API_KEY"), "key7") == 0);
    assert(strcmp(lookup(both, "SPOTIFY_CLIENT_ID"), "id42") == 0);

    /* Values that do not fit are rejected, not silently truncated. */
    char small[4];
    assert(config_env_lookup("K=abc\n", "K", small, sizeof small) == 0);
    assert(strcmp(small, "abc") == 0);
    assert(config_env_lookup("K=abcd\n", "K", small, sizeof small) == -1);
}

static void expect_set(const char *text, const char *want)
{
    char *got = config_env_set(text, "SOLOIST_API_KEY", "new_Key-9");
    assert(got != NULL);
    assert(strcmp(got, want) == 0);
    free(got);
}

static void test_env_set(void)
{
    /* Replace in place; every other line (comments, secrets) verbatim. */
    expect_set("# c\nSPOTIFY_CLIENT_ID=id\nSPOTIFY_CLIENT_SECRET=s\nSOLOIST_API_KEY=\n# end\n",
               "# c\nSPOTIFY_CLIENT_ID=id\nSPOTIFY_CLIENT_SECRET=s\nSOLOIST_API_KEY=new_Key-9\n# end\n");
    expect_set("SOLOIST_API_KEY=old\r\nA=1\r\n", "SOLOIST_API_KEY=new_Key-9\r\nA=1\r\n");
    expect_set("  export SOLOIST_API_KEY = \"old\" # note\n", "SOLOIST_API_KEY=new_Key-9\n");
    expect_set("SOLOIST_API_KEY=a\nSOLOIST_API_KEY=b", "SOLOIST_API_KEY=new_Key-9\nSOLOIST_API_KEY=new_Key-9");
    expect_set("\xEF\xBB\xBF" "SOLOIST_API_KEY=old\n", "\xEF\xBB\xBF" "SOLOIST_API_KEY=new_Key-9\n");
    /* Commented-out lines and look-alike keys are left alone: append. */
    expect_set("# SOLOIST_API_KEY=x\nSOLOIST_API_KEYS=y\n",
               "# SOLOIST_API_KEY=x\nSOLOIST_API_KEYS=y\nSOLOIST_API_KEY=new_Key-9\n");
    expect_set("A=1", "A=1\nSOLOIST_API_KEY=new_Key-9\n");          /* no final newline */
    expect_set("A=1\r\nB=2", "A=1\r\nB=2\r\nSOLOIST_API_KEY=new_Key-9\r\n"); /* CRLF style kept */
    expect_set("", "SOLOIST_API_KEY=new_Key-9\n");

    char *set = config_env_set("A=1\n", "SOLOIST_API_KEY", "k");
    assert(set && strcmp(lookup(set, "SOLOIST_API_KEY"), "k") == 0);
    free(set);
}

static void make_config(Config *c, const char *root)
{
    memset(c, 0, sizeof *c);
    assert(plat_path_join(c->data_dir, sizeof c->data_dir, root, "data") == 0);
    assert(plat_path_join(c->token_path, sizeof c->token_path, c->data_dir, "token.json") == 0);
    assert(plat_path_join(c->soloist_data, sizeof c->soloist_data, c->data_dir, "soloist") == 0);
    assert(plat_path_join(c->soloist_cache, sizeof c->soloist_cache, c->data_dir, "soloist-cache") == 0);
}

static void test_token(void)
{
    const char *exe = plat_exe_dir();
    assert(exe != NULL);
    char root[CFG_PATH_LEN];
    assert(plat_path_join(root, sizeof root, exe, "test_config_scratch") == 0);
    assert(plat_remove_tree(root) == 0);
    assert(plat_mkdir(root) == 0);

    Config c;
    make_config(&c, root);
    Token t, u;
    assert(config_load_token(&c, &u) == -1); /* missing */

    memset(&t, 0, sizeof t);
    snprintf(t.access, sizeof t.access, "BQD-access\"quote\\slash");
    snprintf(t.refresh, sizeof t.refresh, "AQB-refresh");
    t.expires_at = 1893456000; /* 2030-01-01 */
    assert(config_save_token(&c, &t) == 0); /* also creates data/ */
    assert(config_load_token(&c, &u) == 0);
    assert(strcmp(u.access, t.access) == 0);
    assert(strcmp(u.refresh, t.refresh) == 0);
    assert(u.expires_at == t.expires_at);
#ifndef _WIN32
    struct stat st;
    assert(stat(c.token_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);
#endif

    /* Overwrite (rotated refresh token) replaces the old file atomically. */
    snprintf(t.refresh, sizeof t.refresh, "AQB-rotated");
    assert(config_save_token(&c, &t) == 0);
    assert(config_load_token(&c, &u) == 0);
    assert(strcmp(u.refresh, "AQB-rotated") == 0);

    /* Corrupt or refresh-less files count as "not logged in". */
    assert(plat_write_file_atomic(c.token_path, "{not json", 9) == 0);
    assert(config_load_token(&c, &u) == -1);
    const char *no_refresh = "{\"access_token\":\"x\",\"expires_at\":1}";
    assert(plat_write_file_atomic(c.token_path, no_refresh, strlen(no_refresh)) == 0);
    assert(config_load_token(&c, &u) == -1);
    assert(u.access[0] == '\0');

    /* Logout removes the token and Soloist's whole data tree (its paired
     * session), but keeps the playback cache. */
    char nested[CFG_PATH_LEN], file[CFG_PATH_LEN], cached[CFG_PATH_LEN];
    assert(plat_mkdir(c.soloist_data) == 0);
    assert(plat_path_join(nested, sizeof nested, c.soloist_data, "sub") == 0);
    assert(plat_mkdir(nested) == 0);
    assert(plat_path_join(file, sizeof file, nested, "session") == 0);
    assert(plat_write_file_atomic(file, "{}", 2) == 0);
    assert(plat_mkdir(c.soloist_cache) == 0);
    assert(plat_path_join(cached, sizeof cached, c.soloist_cache, "blob") == 0);
    assert(plat_write_file_atomic(cached, "x", 1) == 0);
    assert(config_save_token(&c, &t) == 0);
    assert(config_clear_login(&c) == 0);
    assert(config_load_token(&c, &u) == -1);
    assert(plat_read_file(file, 1024, NULL) == NULL);
    char *kept = plat_read_file(cached, 1024, NULL);
    assert(kept && kept[0] == 'x');
    free(kept);
    assert(config_clear_login(&c) == 0); /* already gone is fine */

    assert(plat_remove_tree(root) == 0);
}

/* config_set_env_value on a real file in the scratch folder. */
static void test_set_env_value(void)
{
    const char *exe = plat_exe_dir();
    assert(exe != NULL);
    char root[CFG_PATH_LEN];
    assert(plat_path_join(root, sizeof root, exe, "test_config_env") == 0);
    assert(plat_remove_tree(root) == 0);
    assert(plat_mkdir(root) == 0);
    Config c;
    memset(&c, 0, sizeof c);
    assert(plat_path_join(c.env_path, sizeof c.env_path, root, ".env") == 0);

    /* Missing file: created with just the line. */
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "first") == 0);
    char *text = plat_read_file(c.env_path, 4096, NULL);
    assert(text && strcmp(text, "SOLOIST_API_KEY=first\n") == 0);
    free(text);

    const char *orig = "# keep\nSPOTIFY_CLIENT_ID=id\nSOLOIST_API_KEY=first\nZ=9";
    assert(plat_write_file_atomic(c.env_path, orig, strlen(orig)) == 0);
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "second") == 0);
    text = plat_read_file(c.env_path, 4096, NULL);
    assert(text && strcmp(text, "# keep\nSPOTIFY_CLIENT_ID=id\nSOLOIST_API_KEY=second\nZ=9") == 0);
    free(text);
#ifndef _WIN32
    struct stat st;
    assert(stat(c.env_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);
#endif

    /* Values that would not read back unchanged are refused, file untouched. */
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "") == -1);
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "a b") == -1);
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "x\ny") == -1);
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "#x") == -1);
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "\"x\"") == -1);
    text = plat_read_file(c.env_path, 4096, NULL);
    assert(text && strstr(text, "SOLOIST_API_KEY=second\n"));
    free(text);

    /* A folder in the way (unreadable as a file) is never clobbered. */
    assert(plat_remove_tree(c.env_path) == 0);
    assert(plat_mkdir(c.env_path) == 0);
    assert(config_set_env_value(&c, "SOLOIST_API_KEY", "third") == -1);

    assert(plat_remove_tree(root) == 0);
}

int main(void)
{
    test_env();
    test_env_set();
    test_token();
    test_set_env_value();
    return 0;
}
