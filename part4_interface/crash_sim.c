/* crash_sim.c - PART 4 (OS side): crash_sim.exe, a helper for demonstrating crash recovery
 *

 */
#include "volume.h"
#include "fileio.h"

#ifdef _WIN32
#define set_env(name, value) _putenv_s(name, value)
#else
#define set_env(name, value) setenv(name, value, 1)
#endif

int main(int argc, char **argv) {
    Volume volume;
    const char *new_text = "THIS IS THE NEW CONTENT WRITTEN BY crash_sim";
    int status;

    if (argc != 4) {
        fprintf(stderr, "usage: crash_sim <image> <file> <after_log|after_data|after_commit>\n");
        return 2;
    }
    if (strcmp(argv[3], "after_log") != 0 && strcmp(argv[3], "after_data") != 0 && strcmp(argv[3], "after_commit") != 0) {
        fprintf(stderr, "error: unknown step '%s'\n", argv[3]);
        return 2;
    }
    if (volume_open(&volume, argv[1], 1) != 0) {
        fprintf(stderr, "error: %s\n", get_error());
        return 1;
    }
    set_env("HYBRIDFS_CRASH_AT", argv[3]);
    status = file_write(&volume, argv[2], (const unsigned char *)new_text, (int)strlen(new_text));
    if (status != 0) {
        fprintf(stderr, "error: %s\n", get_error());
        volume_close(&volume);
        return 1;
    }
    puts("the write finished without hitting the crash point");
    volume_close(&volume);
    return 0;
}
