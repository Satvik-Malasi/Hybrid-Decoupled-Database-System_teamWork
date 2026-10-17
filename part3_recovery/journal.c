/* journal.c - PART 3 (DBMS side): wal_log table, transactions and recovery */
#include "journal.h"
#include "space.h"

int journal_schema_create(sqlite3 *db) {
    if (db_run(db,
        "CREATE TABLE wal_log ("
        " id        INTEGER PRIMARY KEY AUTOINCREMENT,"
        " file_id   INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        " block_no  INTEGER NOT NULL CHECK (block_no > 0),"
        " data      BLOB    NOT NULL,"
        " committed INTEGER NOT NULL DEFAULT 0 CHECK (committed IN (0, 1)),"
        " ts        INTEGER NOT NULL DEFAULT (strftime('%s','now')))") != 0) return -1;
    return db_run(db, "CREATE INDEX idx_wal_file_block ON wal_log(file_id, block_no)");
}

int journal_begin(Volume *v) {
    return db_run(v->db, "BEGIN IMMEDIATE");
}

int journal_commit(Volume *v) {
    return db_run(v->db, "COMMIT");
}

void journal_rollback(Volume *v) {
    sqlite3_exec(v->db, "ROLLBACK", NULL, NULL, NULL);
}

int journal_log_block(Volume *v, int file_id, int block_no, const unsigned char *image, long *log_id) {
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(v->db, "INSERT INTO wal_log (file_id, block_no, data, committed) VALUES (?, ?, ?, 0)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, file_id);
    sqlite3_bind_int(stmt, 2, block_no);
    sqlite3_bind_blob(stmt, 3, image, BLOCK_SIZE, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    if (log_id) *log_id = (long)sqlite3_last_insert_rowid(v->db);
    return 0;
}

int journal_mark_committed(Volume *v, const long *log_ids, int count) {
    sqlite3_stmt *stmt;
    int i;

    if (sqlite3_prepare_v2(v->db, "UPDATE wal_log SET committed = 1 WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    for (i = 0; i < count; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, log_ids[i]);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            set_error("%s", sqlite3_errmsg(v->db));
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

int journal_discard_unfinished(Volume *v, int file_id) {
    sqlite3_stmt *stmt;
    int status;

    if (sqlite3_prepare_v2(v->db, "DELETE FROM wal_log WHERE committed = 0 AND (? = 0 OR file_id = ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, file_id);
    sqlite3_bind_int(stmt, 2, file_id);
    status = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    return 0;
}

int journal_trim(Volume *v, int file_id, long first_kept) {
    sqlite3_stmt *stmt;
    int status;

    if (sqlite3_prepare_v2(v->db,
            "DELETE FROM wal_log WHERE file_id = ? AND committed = 1 AND (? = 0 OR id < ?)",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, file_id);
    sqlite3_bind_int64(stmt, 2, first_kept);
    sqlite3_bind_int64(stmt, 3, first_kept);
    status = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    return 0;
}

/* Committed rows that describe blocks the file no longer owns are leftovers of an older write. */
static int purge_stale_rows(Volume *v) {
    return db_run(v->db,
        "DELETE FROM wal_log WHERE committed = 1 AND NOT EXISTS ("
        "  SELECT 1 FROM used_blocks u WHERE u.file_id = wal_log.file_id AND u.block_no = wal_log.block_no)");
}

/* Rewrites blocks whose newest committed log image differs from what is on the image. */
static int redo_blocks(Volume *v, int *redone) {
    sqlite3_stmt *stmt;
    unsigned char current[BLOCK_SIZE];
    int status;

    if (sqlite3_prepare_v2(v->db,
            "SELECT w.block_no, w.data FROM wal_log w "
            "JOIN used_blocks u ON u.file_id = w.file_id AND u.block_no = w.block_no "
            "WHERE w.committed = 1 AND w.id = ("
            "  SELECT MAX(x.id) FROM wal_log x WHERE x.file_id = w.file_id AND x.block_no = w.block_no AND x.committed = 1)",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    while ((status = sqlite3_step(stmt)) == SQLITE_ROW) {
        int block_no = sqlite3_column_int(stmt, 0);
        const void *logged = sqlite3_column_blob(stmt, 1);
        if (sqlite3_column_bytes(stmt, 1) != BLOCK_SIZE) continue;
        if (block_no < (int)v->disk.first_data_block || block_no >= (int)v->disk.total_blocks) continue;
        if (disk_read_block(&v->disk, (unsigned int)block_no, current) != 0 ||
            memcmp(current, logged, BLOCK_SIZE) != 0) {
            if (disk_write_block(&v->disk, (unsigned int)block_no, logged) != 0) {
                sqlite3_finalize(stmt);
                return -1;
            }
            (*redone)++;
        }
    }
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    return *redone > 0 ? disk_flush(&v->disk) : 0;
}

int journal_recover(Volume *v) {
    int unfinished = 0, leaked = 0, dangling = 0, redone = 0;

    v->undone_rows = v->redone_blocks = v->bitmap_fixes = 0;
    if (!v->disk_ready) {
        set_error("the disk image is not open");
        return -1;
    }

    /* 1. undo: a row that never got committed belongs to a write that did not finish */
    if (db_get_int(v->db, "SELECT COUNT(*) FROM wal_log WHERE committed = 0", &unfinished) != 0) return -1;
    if (journal_discard_unfinished(v, 0) != 0) return -1;
    if (purge_stale_rows(v) != 0) return -1;

    /* 2. the metadata is the truth about who owns which block: rebuild the bitmap from it */
    if (space_reconcile(v, &leaked, &dangling) != 0) return -1;

    /* 3. redo: make sure every committed block really is on the image */
    if (redo_blocks(v, &redone) != 0) return -1;

    v->undone_rows = unfinished;
    v->redone_blocks = redone;
    v->bitmap_fixes = leaked + dangling;
    return 0;
}
