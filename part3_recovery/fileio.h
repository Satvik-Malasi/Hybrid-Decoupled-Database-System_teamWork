/* fileio.h - PART 3 (OS side): reading and writing file contents safely

 */
#ifndef FILEIO_H
#define FILEIO_H

#include "volume.h"

#define CRASH_EXIT_CODE 86

int file_write(Volume *v, const char *path, const unsigned char *data, int length);

/* Reads the whole file into a new buffer (caller frees). */
int file_read(Volume *v, const char *path, unsigned char **data, int *length);

#endif
