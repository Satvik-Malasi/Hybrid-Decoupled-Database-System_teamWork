/* space.c - PART 1 (DBMS side): ownership views, bitmap rebuild and the space report */
#include "space.h"

int space_schema_create(sqlite3 *db) {
    char sql[2048];
    size_t used = 0;
    int i;

    used += snprintf(sql + used, sizeof(sql) - used, "CREATE VIEW used_blocks AS ");
    for (i = 0; i < MAX_FILE_BLOCKS; i++) {
        used += snprintf(sql + used, sizeof(sql) - used,
                         "%sSELECT id AS file_id, block%d AS block_no FROM files WHERE block%d > 0",
                         i ? " UNION ALL " : "", i, i);
    }
    if (db_run(db, sql) != 0) return -1;

    return db_run(db,
        "CREATE VIEW block_usage AS "
        "SELECT block_no, COUNT(*) AS owners FROM used_blocks GROUP BY block_no");
}

/* Works out which blocks the metadata says are in use, and compares with the bitmap on the image. */
static int rebuild(Volume *v, int apply, int *leaked, int *dangling) {
    Disk *disk = &v->disk;
    size_t bytes = (size_t)disk->bitmap_blocks * BLOCK_SIZE;
    unsigned char *before;
    sqlite3_stmt *stmt;
    unsigned int n;
    int lost = 0, missing = 0;

    if (!v->disk_ready) {
        set_error("the disk image is not open");
        return -1;
    }
    before = malloc(bytes);
    if (!before) {
        set_error("out of memory");
        return -1;
    }
    memcpy(before, disk->bitmap, bytes);

    disk_bitmap_reset(disk);
    if (sqlite3_prepare_v2(v->db, "SELECT block_no FROM used_blocks", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        memcpy(disk->bitmap, before, bytes);
        free(before);
        return -1;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        disk_mark_used(disk, (unsigned int)sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);

    for (n = disk->first_data_block; n < disk->total_blocks; n++) {
        int was_used = (before[n / 8] >> (n % 8)) & 1;
        int is_used = disk_is_used(disk, n);
        if (was_used && !is_used) lost++;
        if (!was_used && is_used) missing++;
    }
    if (leaked) *leaked = lost;
    if (dangling) *dangling = missing;

    if (!apply) {
        memcpy(disk->bitmap, before, bytes);
    } else if (lost || missing) {
        if (disk_bitmap_save(disk) != 0) {
            free(before);
            return -1;
        }
    }
    free(before);
    return 0;
}

int space_compare(Volume *v, int *leaked, int *dangling) {
    return rebuild(v, 0, leaked, dangling);
}

int space_reconcile(Volume *v, int *leaked, int *dangling) {
    return rebuild(v, 1, leaked, dangling);
}

int space_report(Volume *v, FILE *out) {
    int file_count = 0, folder_count = 0;
    unsigned int free_blocks, data_blocks;

    if (db_get_int(v->db, "SELECT COUNT(*) FROM files WHERE mode = 32768", &file_count) != 0) return -1;
    if (db_get_int(v->db, "SELECT COUNT(*) FROM files WHERE mode = 16384", &folder_count) != 0) return -1;

    fprintf(out, "image:           %s\n", v->image_path);
    fprintf(out, "metadata:        %s\n", v->db_path);
    fprintf(out, "files / folders: %d / %d (root included)\n", file_count, folder_count);
    if (!v->disk_ready) {
        fprintf(out, "disk image:      not found\n");
        return 0;
    }
    data_blocks = v->disk.total_blocks - v->disk.first_data_block;
    free_blocks = disk_free_count(&v->disk);
    fprintf(out, "block size:      %d bytes\n", BLOCK_SIZE);
    fprintf(out, "total blocks:    %u (%u MB)\n", v->disk.total_blocks, v->disk.total_blocks / 1024);
    fprintf(out, "reserved blocks: %u (superblock + bitmap)\n", v->disk.first_data_block);
    fprintf(out, "data blocks:     %u used, %u free of %u\n", data_blocks - free_blocks, free_blocks, data_blocks);
    return 0;
}
