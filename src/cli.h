/* Command-line front end: `smalltify-player <command> [args]`.
 * Each command returns a process exit code. */
#ifndef CLI_H
#define CLI_H

#include "config.h"

#define CLI_PROG "smalltify-player"

typedef struct {
    const char *name;
    const char *usage;   /* e.g. "seek <seconds>"; one form per line */
    int (*run)(const Config *cfg, int argc, char **argv); /* argv[0] = command */
} CliCommand;

/* NULL-terminated (name == NULL) table defined in cli.c. */
extern const CliCommand cli_commands[];

int  cli_dispatch(const Config *cfg, int argc, char **argv); /* argv[0] = command */
void cli_usage(void); /* always names the program CLI_PROG */

#endif
