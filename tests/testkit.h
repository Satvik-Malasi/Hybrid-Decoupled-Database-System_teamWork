/* testkit.h - tiny helpers shared by the test programs */
#ifndef TESTKIT_H
#define TESTKIT_H

#include "volume.h"
#include "shell.h"

#define CHECK(condition) check_that((condition) ? 1 : 0, #condition, __FILE__, __LINE__)

void check_that(int ok, const char *text, const char *file, int line);

/* Moves into tests/work (created if needed) so test files never mix with the project. */
void test_begin(const char *suite);

/* Prints the result and returns the exit code for main(). */
int test_end(void);

/* Makes a fresh volume called name (any old one is deleted first). */
int fresh_volume(Volume *v, const char *name, unsigned int size_mb);
void delete_volume_files(const char *name);

/* Runs a shell command line and returns everything it printed to stdout in a buffer
 * (static, valid until the next call). result receives SHELL_OK / SHELL_FAIL / SHELL_QUIT. */
const char *run_shell(Shell *shell, const char *line, int *result);

/* Runs a program with its output sent to a file, returns its exit code. */
int run_program(const char *command_line, const char *output_file);

/* Reads a whole text file into a static buffer. */
const char *read_text_file(const char *path);

int contains(const char *text, const char *part);

#endif
