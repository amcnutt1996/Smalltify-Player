/* Startup: no args -> GUI, otherwise CLI command. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

#include "cli.h"
#include "config.h"
#include "http.h"
#include "platform.h"
#include "ui.h"

#ifdef __APPLE__
/* Raven opens the Vulkan loader by bare name, then from Homebrew paths, and
 * the hardened runtime refuses a Homebrew loader. The bundle ships its own
 * loader and MoltenVK in Contents/Frameworks. Opening the bundled loader
 * first makes its install name "libvulkan.1.dylib" already loaded, so
 * Raven's own dlopen of that name gets this copy. Outside a bundle the
 * file is absent and Raven's probes run as before. */
static void preload_vulkan(void)
{
    char lib[4096];
    char icd[4096];
    int n1 = snprintf(lib, sizeof lib, "%s/../Frameworks/libvulkan.1.dylib", plat_exe_dir());
    int n2 = snprintf(icd, sizeof icd, "%s/../Resources/MoltenVK_icd.json", plat_exe_dir());
    if (n1 < 0 || (size_t)n1 >= sizeof lib || n2 < 0 || (size_t)n2 >= sizeof icd)
        return;
    if (!dlopen(lib, RTLD_NOW | RTLD_GLOBAL))
        return;
    setenv("VK_ICD_FILENAMES", icd, 1);
}
#endif

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
    if (gui) {
#ifdef __APPLE__
        preload_vulkan();
#endif
        rc = ui_run(&cfg);
    } else if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        cli_usage();
        rc = 0;
    } else
        rc = cli_dispatch(&cfg, argc - 1, argv + 1);

    http_global_cleanup();
    return rc;
}
