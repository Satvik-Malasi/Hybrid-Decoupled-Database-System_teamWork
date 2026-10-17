/* disk.h - PART 1 (OS side): the raw block device and its free-space bitmap.
 *
 * Image layout (1 KiB blocks):
 *   block 0                 superblock (magic, sizes, checksum)
 *   blocks 1 .. B           allocation bitmap, one bit per block
 *   blocks B+1 .. end       data blocks
 */
#ifndef DISK_H
#define DISK_H

#include "common.h"

#define MIN_SIZE_MB 1
#define MAX_SIZE_MB 2000        /* keeps every byte offset inside a 32-bit fseek */

typedef struct {
    FILE *image;
    unsigned int total_blocks;
    unsigned int bitmap_blocks;
    unsigned int first_data_block;
    unsigned int next_search;           /* where the allocator starts looking */
    unsigned char *bitmap;              /* in-memory copy, bitmap_blocks * BLOCK_SIZE bytes */
} Disk;

int disk_create(Disk *disk, const char *path, unsigned int size_mb);
int disk_open(Disk *disk, const char *path);
void disk_close(Disk *disk);

int disk_read_block(Disk *disk, unsigned int block_no, void *buffer);
int disk_write_block(Disk *disk, unsigned int block_no, const void *buffer);
int disk_flush(Disk *disk);

/* Take the next free data block (marks it used). Returns its number, or -1 when the disk is full. */
int disk_alloc_block(Disk *disk);
int disk_free_block(Disk *disk, unsigned int block_no);
int disk_is_used(const Disk *disk, unsigned int block_no);
unsigned int disk_free_count(const Disk *disk);

/* Used when the bitmap is rebuilt from the metadata (see space.c). */
void disk_bitmap_reset(Disk *disk);                  /* only metadata blocks stay marked */
void disk_mark_used(Disk *disk, unsigned int block_no);
int disk_bitmap_save(Disk *disk);

#endif
