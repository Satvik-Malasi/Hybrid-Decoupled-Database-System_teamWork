/* shell.h - PART 4 (OS side): the command interpreter behind hybridfs.exe */
#ifndef SHELL_H
#define SHELL_H

#include "volume.h"

#define REMEMBER_FILE ".hybridfs_volume"      /* holds the path of the volume used last */

typedef struct {
    Volume volume;
    int is_open;
    FILE *out;                                /* where normal output goes (stdout, or a buffer in tests) */
} Shell;

#define LINE_LIMIT  20000                     /* longest command line accepted */

#define SHELL_OK    0
#define SHELL_FAIL  1
#define SHELL_QUIT  2

void shell_start(Shell *shell, FILE *out);
void shell_stop(Shell *shell);

/* Opens a volume (recovering it if needed). 0 on success, -1 with an error message. */
int shell_open_volume(Shell *shell, const char *image_path);

/* Runs one command line. Returns SHELL_OK, SHELL_FAIL (message already printed) or SHELL_QUIT. */
int shell_run_line(Shell *shell, const char *line);

/* The volume remembered from last time, or an empty string. */
void shell_remembered_volume(char *path, size_t capacity);

#endif
