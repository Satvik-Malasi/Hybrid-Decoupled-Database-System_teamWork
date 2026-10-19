/* volume.h - one HybridFS volume = a raw disk image + its metadata database */
#ifndef VOLUME_H
#define VOLUME_H

#include "common.h"
#include "disk.h"

typedef struct {
    Disk disk;                          /* raw blocks (part 1) */
    int disk_ready;                     /* 1 when the image is open; the SQL tools can run without it */
    sqlite3 *db;                        /* metadata (parts 2, 3 and 4) */
    char image_path[MAX_PATH_LEN];
    char db_path[MAX_PATH_LEN + 8];     /* image path + ".db" */
    /* what the last recovery pass did (filled by journal_recover) */
    int undone_rows;                    /* unfinished log rows thrown away */
    int redone_blocks;                  /* blocks rewritten from the log */
    int bitmap_fixes;                   /* bitmap entries corrected */
} Volume;

/* Make a new volume. Refuses to touch existing files unless force is 1. */
int volume_create(Volume *v, const char *image_path, unsigned int size_mb, int force);

/* Open an existing volume. With need_image = 0 a missing image is not an error. */
int volume_open(Volume *v, const char *image_path, int need_image);

void volume_close(Volume *v);
int volume_is_open(const Volume *v);

#endif
