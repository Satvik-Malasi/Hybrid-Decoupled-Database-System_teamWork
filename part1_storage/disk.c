/* disk.c - PART 1 (OS side): block device, superblock and allocation bitmap */
#include <stdint.h>
#include "disk.h"

#ifdef _WIN32
#include <io.h>
#define sync_file(fp) _commit(_fileno(fp))
#else
#include <unistd.h>
#define sync_file(fp) fsync(fileno(fp))
#endif

#define MAGIC "HYBRIDFS"
#define FORMAT_VERSION 2

static uint32_t crc32_of(const unsigned char *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_u32(unsigned char *at, uint32_t value) {
    at[0] = (unsigned char)(value);
    at[1] = (unsigned char)(value >> 8);
    at[2] = (unsigned char)(value >> 16);
    at[3] = (unsigned char)(value >> 24);
}

static uint32_t get_u32(const unsigned char *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static unsigned int bitmap_blocks_for(unsigned int total_blocks) {
    unsigned int bytes = (total_blocks + 7) / 8;
    return (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

static int bit_is_set(const unsigned char *bitmap, unsigned int n) {
    return (bitmap[n / 8] >> (n % 8)) & 1;
}

static void bit_set(unsigned char *bitmap, unsigned int n) {
    bitmap[n / 8] |= (unsigned char)(1u << (n % 8));
}

static void bit_clear(unsigned char *bitmap, unsigned int n) {
    bitmap[n / 8] &= (unsigned char)~(1u << (n % 8));
}

static int seek_to_block(Disk *disk, unsigned int block_no) {
    return fseek(disk->image, (long)block_no * BLOCK_SIZE, SEEK_SET);
}

static int write_superblock(Disk *disk) {
    unsigned char block[BLOCK_SIZE];
    memset(block, 0, sizeof(block));
    memcpy(block, MAGIC, 8);
    put_u32(block + 8, FORMAT_VERSION);
    put_u32(block + 12, BLOCK_SIZE);
    put_u32(block + 16, disk->total_blocks);
    put_u32(block + 20, disk->bitmap_blocks);
    put_u32(block + 24, disk->first_data_block);
    put_u32(block + 28, crc32_of(block, 28));
    if (seek_to_block(disk, 0) != 0 || fwrite(block, 1, BLOCK_SIZE, disk->image) != BLOCK_SIZE) {
        set_error("cannot write the superblock");
        return -1;
    }
    return 0;
}

/* Marks the superblock, the bitmap itself, and the unused tail bits as taken. */
static void mark_reserved_bits(Disk *disk) {
    unsigned int n;
    for (n = 0; n < disk->first_data_block; n++) bit_set(disk->bitmap, n);
    for (n = disk->total_blocks; n < disk->bitmap_blocks * BLOCK_SIZE * 8; n++) bit_set(disk->bitmap, n);
}

static int save_bitmap_block(Disk *disk, unsigned int index) {
    if (seek_to_block(disk, 1 + index) != 0 ||
        fwrite(disk->bitmap + (size_t)index * BLOCK_SIZE, 1, BLOCK_SIZE, disk->image) != BLOCK_SIZE) {
        set_error("cannot write the allocation bitmap");
        return -1;
    }
    return 0;
}

int disk_create(Disk *disk, const char *path, unsigned int size_mb) {
    memset(disk, 0, sizeof(*disk));
    if (size_mb < MIN_SIZE_MB || size_mb > MAX_SIZE_MB) {
        set_error("size must be between %d and %d MB", MIN_SIZE_MB, MAX_SIZE_MB);
        return -1;
    }
    disk->total_blocks = size_mb * 1024u;
    disk->bitmap_blocks = bitmap_blocks_for(disk->total_blocks);
    disk->first_data_block = 1 + disk->bitmap_blocks;
    disk->next_search = disk->first_data_block;

    disk->image = fopen(path, "wb+");
    if (!disk->image) {
        set_error("cannot create image '%s'", path);
        return -1;
    }
    disk->bitmap = calloc(disk->bitmap_blocks, BLOCK_SIZE);
    if (!disk->bitmap) {
        set_error("out of memory");
        goto failed;
    }
    /* stretch the file to its full size, then write the metadata blocks */
    if (fseek(disk->image, (long)disk->total_blocks * BLOCK_SIZE - 1, SEEK_SET) != 0 || fputc(0, disk->image) == EOF) {
        set_error("cannot size the image to %u MB (is there enough free space?)", size_mb);
        goto failed;
    }
    mark_reserved_bits(disk);
    if (write_superblock(disk) != 0 || disk_bitmap_save(disk) != 0) goto failed;
    return 0;

failed:
    disk_close(disk);
    remove(path);
    return -1;
}

int disk_open(Disk *disk, const char *path) {
    unsigned char block[BLOCK_SIZE];
    long file_size;
    unsigned int index;

    memset(disk, 0, sizeof(*disk));
    disk->image = fopen(path, "rb+");
    if (!disk->image) {
        set_error("cannot open image '%s'", path);
        return -1;
    }
    if (fread(block, 1, BLOCK_SIZE, disk->image) != BLOCK_SIZE || memcmp(block, MAGIC, 8) != 0) {
        set_error("'%s' is not a HybridFS image", path);
        goto failed;
    }
    if (get_u32(block + 28) != crc32_of(block, 28)) {
        set_error("superblock checksum mismatch - the image is damaged");
        goto failed;
    }
    if (get_u32(block + 8) != FORMAT_VERSION || get_u32(block + 12) != BLOCK_SIZE) {
        set_error("image format is not supported by this version");
        goto failed;
    }
    disk->total_blocks = get_u32(block + 16);
    disk->bitmap_blocks = get_u32(block + 20);
    disk->first_data_block = get_u32(block + 24);
    if (disk->total_blocks < 1024 || disk->bitmap_blocks != bitmap_blocks_for(disk->total_blocks) ||
        disk->first_data_block != 1 + disk->bitmap_blocks) {
        set_error("superblock values are inconsistent");
        goto failed;
    }
    fseek(disk->image, 0, SEEK_END);
    file_size = ftell(disk->image);
    if (file_size < (long)disk->total_blocks * BLOCK_SIZE) {
        set_error("image file is shorter than its superblock says");
        goto failed;
    }
    disk->bitmap = malloc((size_t)disk->bitmap_blocks * BLOCK_SIZE);
    if (!disk->bitmap) {
        set_error("out of memory");
        goto failed;
    }
    for (index = 0; index < disk->bitmap_blocks; index++) {
        if (seek_to_block(disk, 1 + index) != 0 ||
            fread(disk->bitmap + (size_t)index * BLOCK_SIZE, 1, BLOCK_SIZE, disk->image) != BLOCK_SIZE) {
            set_error("cannot read the allocation bitmap");
            goto failed;
        }
    }
    mark_reserved_bits(disk);
    disk->next_search = disk->first_data_block;
    return 0;

failed:
    disk_close(disk);
    return -1;
}

void disk_close(Disk *disk) {
    if (disk->image) {
        fflush(disk->image);
        fclose(disk->image);
    }
    free(disk->bitmap);
    disk->image = NULL;
    disk->bitmap = NULL;
}

int disk_read_block(Disk *disk, unsigned int block_no, void *buffer) {
    if (block_no >= disk->total_blocks) {
        set_error("block %u is outside the disk", block_no);
        return -1;
    }
    if (seek_to_block(disk, block_no) != 0 || fread(buffer, 1, BLOCK_SIZE, disk->image) != BLOCK_SIZE) {
        set_error("cannot read block %u", block_no);
        return -1;
    }
    return 0;
}

int disk_write_block(Disk *disk, unsigned int block_no, const void *buffer) {
    if (block_no < disk->first_data_block || block_no >= disk->total_blocks) {
        set_error("block %u is not a data block", block_no);
        return -1;
    }
    if (seek_to_block(disk, block_no) != 0 || fwrite(buffer, 1, BLOCK_SIZE, disk->image) != BLOCK_SIZE) {
        set_error("cannot write block %u", block_no);
        return -1;
    }
    return 0;
}

int disk_flush(Disk *disk) {
    if (fflush(disk->image) != 0 || sync_file(disk->image) != 0) {
        set_error("cannot flush the image to disk");
        return -1;
    }
    return 0;
}

int disk_alloc_block(Disk *disk) {
    unsigned int data_blocks = disk->total_blocks - disk->first_data_block;
    unsigned int tried, n;

    for (tried = 0; tried < data_blocks; tried++) {
        n = disk->next_search + tried;
        if (n >= disk->total_blocks) n = disk->first_data_block + (n - disk->total_blocks);
        if (!bit_is_set(disk->bitmap, n)) {
            bit_set(disk->bitmap, n);
            disk->next_search = n + 1;
            if (save_bitmap_block(disk, n / 8 / BLOCK_SIZE) != 0) {
                bit_clear(disk->bitmap, n);
                return -1;
            }
            return (int)n;
        }
    }
    set_error("disk is full");
    return -1;
}

int disk_free_block(Disk *disk, unsigned int block_no) {
    if (block_no < disk->first_data_block || block_no >= disk->total_blocks) {
        set_error("cannot free block %u: not a data block", block_no);
        return -1;
    }
    if (!bit_is_set(disk->bitmap, block_no)) return 0;
    bit_clear(disk->bitmap, block_no);
    if (block_no < disk->next_search) disk->next_search = block_no;
    return save_bitmap_block(disk, block_no / 8 / BLOCK_SIZE);
}

int disk_is_used(const Disk *disk, unsigned int block_no) {
    if (block_no >= disk->total_blocks) return 1;
    return bit_is_set(disk->bitmap, block_no);
}

unsigned int disk_free_count(const Disk *disk) {
    unsigned int n, count = 0;
    for (n = disk->first_data_block; n < disk->total_blocks; n++) {
        if (!bit_is_set(disk->bitmap, n)) count++;
    }
    return count;
}

void disk_bitmap_reset(Disk *disk) {
    memset(disk->bitmap, 0, (size_t)disk->bitmap_blocks * BLOCK_SIZE);
    mark_reserved_bits(disk);
    disk->next_search = disk->first_data_block;
}

void disk_mark_used(Disk *disk, unsigned int block_no) {
    if (block_no >= disk->first_data_block && block_no < disk->total_blocks) bit_set(disk->bitmap, block_no);
}

int disk_bitmap_save(Disk *disk) {
    unsigned int index;
    for (index = 0; index < disk->bitmap_blocks; index++) {
        if (save_bitmap_block(disk, index) != 0) return -1;
    }
    return disk_flush(disk);
}
