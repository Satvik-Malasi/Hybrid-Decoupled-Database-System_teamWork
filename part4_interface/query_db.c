/* query_db.c - PART 4 (DBMS side): query_db.exe, a stand-alone SQL console for the metadata
 *
 *   query_db [-i image] ["sql statement"]
 *
 * With a statement it runs it and exits. Without one it reads statements from the
 * keyboard (one per line) until "exit". The same safety rules as the shell's sql command apply.
 */
#include "shell.h"
#include "sqlconsole.h"

#ifdef _WIN32
#include <io.h>
#define input_is_keyboard() _isatty(_fileno(stdin))
#else
#include <unistd.h>
#define input_is_keyboard() isatty(fileno(stdin))
#endif

int main(int argc, char **argv) {
    Volume volume;
    char image[MAX_PATH_LEN] = "";
    const char *statement = NULL;
    char line[4096];
    int i, exit_code = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            snprintf(image, sizeof(image), "%s", argv[++i]);
        } else if (!statement) {
            statement = argv[i];
        } else {
            fprintf(stderr, "usage: query_db [-i image] [\"sql statement\"]\n");
            return 2;
        }
    }
    if (image[0] == '\0') shell_remembered_volume(image, sizeof(image));
    if (image[0] == '\0') snprintf(image, sizeof(image), "hybridfs.img");

    if (volume_open(&volume, image, 0) != 0) {
        fprintf(stderr, "error: %s\n", get_error());
        return 1;
    }

    if (statement) {
        if (sql_console_run(&volume, statement, stdout) != 0) {
            fprintf(stderr, "error: %s\n", get_error());
            exit_code = 1;
        }
        volume_close(&volume);
        return exit_code;
    }

    if (input_is_keyboard()) printf("query_db on %s - type a SQL statement, or exit\n", image);
    for (;;) {
        size_t length;
        if (input_is_keyboard()) {
            fputs("sql> ", stdout);
            fflush(stdout);
        }
        if (!fgets(line, sizeof(line), stdin)) break;
        if (line[0] == '\xEF' && line[1] == '\xBB' && line[2] == '\xBF') memmove(line, line + 3, strlen(line) - 2);
        length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r' || line[length - 1] == ' ')) line[--length] = '\0';
        if (length == 0) continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;
        if (sql_console_run(&volume, line, stdout) != 0) {
            fprintf(stderr, "error: %s\n", get_error());
            exit_code = 1;
        }
        fflush(stdout);
    }
    volume_close(&volume);
    return exit_code;
}
