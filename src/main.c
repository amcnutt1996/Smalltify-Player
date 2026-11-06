/* Startup: no args -> GUI, otherwise CLI command. */
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "config.h"
#include "http.h"
#include "ui.h"

int main(int argc, char **argv)
{
    Config cfg;
    if (config_load(&cfg) != 0) {
        fprintf(stderr, "error: cannot resolve application folder\n");
        return 1;
    }
    if (http_global_init() != 0) {
        fprintf(stderr, "error: libcurl init failed\n");
        return 1;
    }

    int rc;
    if (argc < 2)
        rc = ui_run(&cfg);
    else if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        cli_usage(argv[0]);
        rc = 0;
    } else
        rc = cli_dispatch(&cfg, argc - 1, argv + 1);

    http_global_cleanup();
    return rc;
}
