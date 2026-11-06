/* Startup: no args -> GUI, otherwise CLI command. */
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "config.h"
#include "http.h"
#include "platform.h"
#include "ui.h"

int main(int argc, char **argv)
{
    if (plat_startup(&argc, &argv) != 0) {
        fprintf(stderr, "error: cannot initialize the process\n");
        return 1;
    }
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
#ifdef __APPLE__
    /* Older LaunchServices pass "-psn_<n>_<n>" on a Finder launch: that is
     * still a GUI launch, not a CLI command. */
    int gui = argc < 2 || !strncmp(argv[1], "-psn_", 5);
#else
    int gui = argc < 2;
#endif
    if (gui)
        rc = ui_run(&cfg);
    else if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        cli_usage();
        rc = 0;
    } else
        rc = cli_dispatch(&cfg, argc - 1, argv + 1);

    http_global_cleanup();
    return rc;
}
