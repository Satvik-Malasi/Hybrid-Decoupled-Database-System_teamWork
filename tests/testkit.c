/* testkit.c */
#include <direct.h>
#include "testkit.h"

static int checks_run = 0;
static int checks_failed = 0;
static char suite_name[64] = "";

void check_that(int ok, const char *text, const char *file, int line) {
    checks_run++;
    if (!ok) {
        checks_failed++;
        printf("  FAILED %s:%d: %s\n", file, line, text);
        if (get_error()[0]) printf("         last error: %s\n", get_error());
    }
}

void test_begin(const char *suite) {
    snprintf(suite_name, sizeof(suite_name), "%s", suite);
    _mkdir("tests");
    _mkdir("tests/work");
    if (_chdir("tests/work") != 0) {
        printf("cannot enter tests/work\n");
        exit(2);
    }
    printf("== %s ==\n", suite);
}

int test_end(void) {
    printf("%s: %d checks, %d failed\n", suite_name, checks_run, checks_failed);
    return checks_failed ? 1 : 0;
}

void delete_volume_files(const char *name) {
    char db_name[300];
    snprintf(db_name, sizeof(db_name), "%s.db", name);
    remove(name);
    remove(db_name);
}

int fresh_volume(Volume *v, const char *name, unsigned int size_mb) {
    delete_volume_files(name);
    return volume_create(v, name, size_mb, 0);
}

const char *run_shell(Shell *shell, const char *line, int *result) {
    static char captured[65536];
    FILE *saved = shell->out;
    FILE *capture = tmpfile();
    size_t got;
    int status;

    shell->out = capture;
    status = shell_run_line(shell, line);
    shell->out = saved;
    if (result) *result = status;

    fflush(capture);
    rewind(capture);
    got = fread(captured, 1, sizeof(captured) - 1, capture);
    captured[got] = '\0';
    fclose(capture);
    return captured;
}

int run_program(const char *command_line, const char *output_file) {
    char full[2048];
    snprintf(full, sizeof(full), "%s > %s 2>&1", command_line, output_file);
    return system(full);
}

const char *read_text_file(const char *path) {
    static char text[65536];
    FILE *file = fopen(path, "rb");
    size_t got = 0;
    if (file) {
        got = fread(text, 1, sizeof(text) - 1, file);
        fclose(file);
    }
    text[got] = '\0';
    {
        /* Windows programs end lines with \r\n; compare on plain \n */
        size_t from, to = 0;
        for (from = 0; from < got; from++) {
            if (text[from] != '\r') text[to++] = text[from];
        }
        text[to] = '\0';
    }
    return text;
}

int contains(const char *text, const char *part) {
    return strstr(text, part) != NULL;
}
