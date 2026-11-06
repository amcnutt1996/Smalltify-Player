/* Soloist build-date parsing and expiry arithmetic, plus the child exit
 * code the supervisor relies on to spot an expired build. */
#include <assert.h>
#include <stdio.h>

#include "platform.h"
#include "soloist.h"

#define DAY 86400

static void test_build_day(void)
{
    /* Real `soloist --version` output; 2026-11-06 is day 20763. */
    const char *v = "soloist 1.3.10.50 build 1793966524 (20261106) (g19412df6ea) (macos/aarch64)\n";
    assert(soloist_build_day(v) == 20763);
    assert(soloist_build_day("x (20000101)") == 10957);
    assert(soloist_build_day("x (20000301)") == 11017);  /* after a century leap day */
    assert(soloist_build_day("x (20240229)") == 19782);  /* leap day */
    assert(soloist_build_day("x (20261231)") == 20818);

    /* Only an 8-digit group in parentheses counts. */
    assert(soloist_build_day("") == -1);
    assert(soloist_build_day("soloist build 1793966524") == -1);
    assert(soloist_build_day("(2026110)") == -1);        /* 7 digits */
    assert(soloist_build_day("(202611060)") == -1);      /* 9 digits */
    assert(soloist_build_day("(20261106") == -1);        /* unclosed */
    assert(soloist_build_day("(20261306)") == -1);       /* month 13 */
    assert(soloist_build_day("(20261100)") == -1);       /* day 0 */
    assert(soloist_build_day("(19991231)") == -1);       /* before any build */
    assert(soloist_build_day("(g1234567) (20261106)") == 20763); /* skips non-dates */
}

static void test_days_left(void)
{
    long day = 20763;
    int64_t build_noon = (int64_t)day * DAY + DAY / 2;
    assert(soloist_days_left(day, build_noon) == SOLOIST_EXPIRY_DAYS);
    /* Counted per UTC day: the last second of a day is still that day. */
    assert(soloist_days_left(day, (int64_t)(day + 1) * DAY - 1) == SOLOIST_EXPIRY_DAYS);
    assert(soloist_days_left(day, (int64_t)(day + 1) * DAY) == SOLOIST_EXPIRY_DAYS - 1);
    /* Renewal boundary and expiry. */
    int64_t renew = (int64_t)(day + SOLOIST_EXPIRY_DAYS - SOLOIST_RENEW_DAYS) * DAY;
    assert(soloist_days_left(day, renew) == SOLOIST_RENEW_DAYS);
    assert(soloist_days_left(day, renew - 1) == SOLOIST_RENEW_DAYS + 1);
    assert(soloist_days_left(day, (int64_t)(day + SOLOIST_EXPIRY_DAYS) * DAY) == 0);
    assert(soloist_days_left(day, (int64_t)(day + 200) * DAY) < 0);
}

static void test_exit_code(void)
{
#ifndef _WIN32
    const char *expired[] = { "/bin/sh", "-c", "echo bye; exit 10", NULL };
    plat_proc *p = plat_proc_spawn(expired);
    assert(p != NULL);
    char buf[64];
    while (plat_proc_read(p, buf, sizeof buf) > 0) {
    }
    assert(plat_proc_wait(p, 5000) == 10);
    assert(plat_proc_wait(p, 0) == 10); /* stays known once reaped */
    plat_proc_kill(p);

    const char *sleeper[] = { "/bin/sh", "-c", "sleep 30", NULL };
    p = plat_proc_spawn(sleeper);
    assert(p != NULL);
    assert(plat_proc_wait(p, 10) == -1); /* still running */
    plat_proc_terminate(p);
    assert(plat_proc_wait(p, 0) == -1);  /* killed by a signal */
    plat_proc_kill(p);
#endif
}

/* Same build, at most one check a day: no re-download on every launch. */
static void test_need_download(void)
{
    long day = 20763;
    int64_t fresh = (int64_t)day * DAY + DAY / 2;           /* 90 days left */
    int64_t renew = fresh + (int64_t)(SOLOIST_EXPIRY_DAYS - SOLOIST_RENEW_DAYS) * DAY; /* 7 left */
    int64_t dead = fresh + (int64_t)SOLOIST_EXPIRY_DAYS * DAY;
    assert(soloist_need_download(-1, 0, fresh) == 1);       /* missing or broken */
    assert(soloist_need_download(day, 0, fresh) == 0);      /* plenty of days left */
    assert(soloist_need_download(day, 0, renew) == 1);      /* renewal window, never checked */
    assert(soloist_need_download(day, renew - 3600, renew) == 0);    /* checked an hour ago */
    assert(soloist_need_download(day, renew - DAY, renew) == 1);     /* a day ago */
    assert(soloist_need_download(day, renew + 3600, renew) == 1);    /* clock set back */
    assert(soloist_need_download(day, dead - 60, dead) == 1);        /* expired: always */
}

int main(void)
{
    test_need_download();
    test_build_day();
    test_days_left();
    test_exit_code();
    printf("test_soloist: ok\n");
    return 0;
}
