/* fileio.c - PART 3 (OS side): copy-on-write file contents */
#include "fileio.h"
#include "journal.h"
#include "tree.h"

static void crash_point(const char *step) {
    const char *wanted = getenv("HYBRIDFS_CRASH_AT");
    if (wanted && strcmp(wanted, step) == 0) {
        fprintf(stderr, "simulated crash at '%s'\n", step);
        _exit(CRASH_EXIT_CODE);          /* no cleanup, like a power cut */
    }
}

static void release_new_blocks(Volume *v, const int *blocks, int count) {
    int i;
    for (i = 0; i < count; i++) disk_free_block(&v->disk, (unsigned int)blocks[i]);
}

static int update_metadata(Volume *v, int file_id, int length, const int *blocks, const long *log_ids, int log_count) {
    char sql[512];
    size_t used = 0;
    sqlite3_stmt *stmt;
    int i;

    used += snprintf(sql + used, sizeof(sql) - used, "UPDATE files SET size = ?");
    for (i = 0; i < MAX_FILE_BLOCKS; i++) used += snprintf(sql + used, sizeof(sql) - used, ", block%d = ?", i);
    snprintf(sql + used, sizeof(sql) - used, " WHERE id = ?");

    if (journal_begin(v) != 0) return -1;
    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        journal_rollback(v);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, length);
    for (i = 0; i < MAX_FILE_BLOCKS; i++) sqlite3_bind_int(stmt, 2 + i, blocks[i]);
    sqlite3_bind_int(stmt, 2 + MAX_FILE_BLOCKS, file_id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        journal_rollback(v);
        return -1;
    }
    sqlite3_finalize(stmt);
    if (journal_mark_committed(v, log_ids, log_count) != 0 || journal_commit(v) != 0) {
        journal_rollback(v);
        return -1;
    }
    return 0;
}

int file_write(Volume *v, const char *path, const unsigned char *data, int length) {
    Entry file;
    int fresh[MAX_FILE_BLOCKS] = {0};
    long log_ids[MAX_FILE_BLOCKS] = {0};
    unsigned char images[MAX_FILE_BLOCKS][BLOCK_SIZE];
    int needed, i;

    if (!v->disk_ready) {
        set_error("the disk image is not open");
        return -1;
    }
    if (!tree_find(v, path, &file)) return -1;
    if (file.mode != MODE_FILE) {
        set_error("'%s' is a folder, not a file", path);
        return -1;
    }
    if (length < 0 || length > MAX_FILE_SIZE) {
        set_error("too much data: a file can hold at most %d bytes (%d blocks)", MAX_FILE_SIZE, MAX_FILE_BLOCKS);
        return -1;
    }
    needed = (length + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* 1. reserve fresh blocks - the current ones stay untouched */
    for (i = 0; i < needed; i++) {
        int block = disk_alloc_block(&v->disk);
        if (block < 0) {
            release_new_blocks(v, fresh, i);
            return -1;
        }
        fresh[i] = block;
        memset(images[i], 0, BLOCK_SIZE);
        memcpy(images[i], data + (size_t)i * BLOCK_SIZE,
               (size_t)(length - i * BLOCK_SIZE < BLOCK_SIZE ? length - i * BLOCK_SIZE : BLOCK_SIZE));
    }

    /* 2. write the log first (its own transaction, so it is on disk before the data is) */
    if (journal_begin(v) != 0) {
        release_new_blocks(v, fresh, needed);
        return -1;
    }
    for (i = 0; i < needed; i++) {
        if (journal_log_block(v, file.id, fresh[i], images[i], &log_ids[i]) != 0) {
            journal_rollback(v);
            release_new_blocks(v, fresh, needed);
            return -1;
        }
    }
    if (journal_commit(v) != 0) {
        journal_rollback(v);
        release_new_blocks(v, fresh, needed);
        return -1;
    }
    crash_point("after_log");

    /* 3. write the data blocks */
    for (i = 0; i < needed; i++) {
        if (disk_write_block(&v->disk, (unsigned int)fresh[i], images[i]) != 0) goto undo;
    }
    if (disk_flush(&v->disk) != 0) goto undo;
    crash_point("after_data");

    /* 4. switch the file over to the new blocks and commit the log rows in one step */
    if (update_metadata(v, file.id, length, fresh, log_ids, needed) != 0) goto undo;
    crash_point("after_commit");

    /* 5. the old blocks and old log rows are garbage now */
    for (i = 0; i < MAX_FILE_BLOCKS; i++) {
        if (file.blocks[i] > 0) disk_free_block(&v->disk, (unsigned int)file.blocks[i]);
    }
    journal_trim(v, file.id, needed > 0 ? log_ids[0] : 0);
    return 0;

undo:
    {
        char saved[512];
        snprintf(saved, sizeof(saved), "%s", get_error());
        journal_discard_unfinished(v, file.id);
        release_new_blocks(v, fresh, needed);
        set_error("%s", saved);
    }
    return -1;
}

int file_read(Volume *v, const char *path, unsigned char **data, int *length) {
    Entry file;
    unsigned char block[BLOCK_SIZE];
    unsigned char *buffer;
    int i, blocks_used;

    *data = NULL;
    *length = 0;
    if (!v->disk_ready) {
        set_error("the disk image is not open");
        return -1;
    }
    if (!tree_find(v, path, &file)) return -1;
    if (file.mode != MODE_FILE) {
        set_error("'%s' is a folder, not a file", path);
        return -1;
    }
    buffer = malloc((size_t)file.size + 1);
    if (!buffer) {
        set_error("out of memory");
        return -1;
    }
    blocks_used = (file.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (i = 0; i < blocks_used; i++) {
        int chunk = file.size - i * BLOCK_SIZE;
        if (chunk > BLOCK_SIZE) chunk = BLOCK_SIZE;
        if (file.blocks[i] <= 0 || disk_read_block(&v->disk, (unsigned int)file.blocks[i], block) != 0) {
            set_error("'%s' is damaged: block %d cannot be read (run: check)", path, i);
            free(buffer);
            return -1;
        }
        memcpy(buffer + (size_t)i * BLOCK_SIZE, block, (size_t)chunk);
    }
    buffer[file.size] = '\0';
    *data = buffer;
    *length = file.size;
    return 0;
}
