/* main.c - PART 4 (OS side): hybridfs.exe
 *
 *   hybridfs [image] [-c "command"]...
 *
 * With no -c it reads commands from the keyboard (or a pipe) until "exit".
 * With one or more -c options it runs those commands and stops; the exit code
 * is 1 if any of them failed.
 */
#include <ctype.h>
#include "shell.h"

#ifdef _WIN32
#include <io.h>
#define input_is_keyboard() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define input_is_keyboard() isatty(fileno(stdin))
#endif

#define MAX_COMMANDS 64

int main(int argc, char **argv) {
    Shell shell;
    const char *commands[MAX_COMMANDS];
    int command_count = 0, exit_code = 0, i;
    char image[MAX_PATH_LEN] = "";
    char line[LINE_LIMIT + 16];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc || command_count >= MAX_COMMANDS) {
                fprintf(stderr, "usage: hybridfs [image] [-c \"command\"]...\n");
                return 2;
            }
            commands[command_count++] = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: hybridfs [image] [-c \"command\"]...\n"
                   "Run without -c for an interactive session; type help inside.\n");
            return 0;
        } else if (image[0] == '\0') {
            snprintf(image, sizeof(image), "%s", argv[i]);
        } else {
            fprintf(stderr, "usage: hybridfs [image] [-c \"command\"]...\n");
            return 2;
        }
    }

    shell_start(&shell, stdout);

    if (image[0]) {
        if (shell_open_volume(&shell, image) != 0) {
            fprintf(stderr, "error: %s\n", get_error());
            return 1;
        }
    } else {
        char remembered[MAX_PATH_LEN];
        shell_remembered_volume(remembered, sizeof(remembered));
        if (remembered[0]) shell_open_volume(&shell, remembered);      /* quietly skip if it is gone */
    }

    if (command_count > 0) {
        for (i = 0; i < command_count; i++) {
            int result = shell_run_line(&shell, commands[i]);
            if (result == SHELL_QUIT) break;
            if (result == SHELL_FAIL) exit_code = 1;
        }
        shell_stop(&shell);
        return exit_code;
    }

    if (input_is_keyboard()) {
        puts("HybridFS - type help for commands, exit to leave");
        if (shell.is_open) printf("volume: %s\n", shell.volume.image_path);
    }
    for (;;) {
        int result;
        if (input_is_keyboard()) {
            fputs("hybridfs> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin)) break;
        if (line[0] == '\xEF' && line[1] == '\xBB' && line[2] == '\xBF') memmove(line, line + 3, strlen(line) - 2);
        result = shell_run_line(&shell, line);
        fflush(stdout);
        if (result == SHELL_QUIT) break;
        if (result == SHELL_FAIL) exit_code = 1;
    }
    shell_stop(&shell);
    return exit_code;
}
