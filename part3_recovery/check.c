/* check.c - PART 3 (OS side): consistency checker */
#include <stdarg.h>
#include "check.h"
#include "journal.h"
#include "space.h"
#include "tree.h"

/* Prints one problem line and adds it to the running total. */
static void count_problem(FILE *out, int *total, const char *format, ...) {
    va_list args;
    fprintf(out, "  problem: ");
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);
    fputc('\n', out);
    (*total)++;
}

static int run_checks(Volume *v, FILE *out, int *problems) {
    sqlite3_stmt *stmt;
    int total = 0, count = 0, leaked = 0, dangling = 0;

    /* unfinished log rows */
    if (db_get_int(v->db, "SELECT COUNT(*) FROM wal_log WHERE committed = 0", &count) != 0) return -1;
    if (count > 0) count_problem(out, &total, "%d unfinished log row(s) from an interrupted write", count);

    /* block pointers that leave the data area */
    if (sqlite3_prepare_v2(v->db, "SELECT file_id, block_no FROM used_blocks WHERE block_no < ? OR block_no >= ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, (int)v->disk.first_data_block);
    sqlite3_bind_int(stmt, 2, (int)v->disk.total_blocks);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        count_problem(out, &total, "entry %d points at block %d, which is outside the data area",
                      sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1));
    }
    sqlite3_finalize(stmt);

    /* a block owned twice */
    if (sqlite3_prepare_v2(v->db, "SELECT block_no, owners FROM block_usage WHERE owners > 1", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        count_problem(out, &total, "block %d is owned by %d entries",
                      sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1));
    }
    sqlite3_finalize(stmt);

    /* size has to match the number of blocks; folders own no blocks */
    if (sqlite3_prepare_v2(v->db, "SELECT " ENTRY_COLUMNS " FROM files", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Entry entry;
        int wanted, i, bad = 0;
        tree_load_entry(stmt, &entry);
        wanted = entry.mode == MODE_FILE ? (entry.size + BLOCK_SIZE - 1) / BLOCK_SIZE : 0;
        for (i = 0; i < MAX_FILE_BLOCKS; i++) {
            if ((i < wanted) != (entry.blocks[i] > 0)) bad = 1;
        }
        if (bad) {
            count_problem(out, &total, "entry %d ('%s'): size %d does not match its block pointers",
                          entry.id, entry.name, entry.size);
        }
    }
    sqlite3_finalize(stmt);

    /* entries that cannot be reached from the root */
    if (db_get_int(v->db, "SELECT COUNT(*) FROM files WHERE id NOT IN (SELECT id FROM file_paths)", &count) != 0) return -1;
    if (count > 0) count_problem(out, &total, "%d entry(ies) cannot be reached from the root folder", count);

    /* bitmap against metadata */
    if (space_compare(v, &leaked, &dangling) != 0) return -1;
    if (leaked > 0) count_problem(out, &total, "%d block(s) marked used but owned by nobody (leaked)", leaked);
    if (dangling > 0) count_problem(out, &total, "%d block(s) owned by a file but marked free", dangling);

    *problems = total;
    return 0;
}

int volume_check(Volume *v, int fix, FILE *out, int *remaining) {
    int problems = 0;

    *remaining = 0;
    if (!v->disk_ready) {
        set_error("the disk image is not open");
        return -1;
    }
    fprintf(out, "checking %s ...\n", v->image_path);
    if (run_checks(v, out, &problems) != 0) return -1;

    if (problems > 0 && fix) {
        fprintf(out, "repairing log rows and the bitmap ...\n");
        if (journal_recover(v) != 0) return -1;
        fprintf(out, "  discarded %d log row(s), rewrote %d block(s), corrected %d bitmap entries\n",
                v->undone_rows, v->redone_blocks, v->bitmap_fixes);
        fprintf(out, "checking again ...\n");
        if (run_checks(v, out, &problems) != 0) return -1;
    }

    *remaining = problems;
    if (problems == 0) {
        fprintf(out, "ok: no problems found\n");
    } else if (!fix) {
        fprintf(out, "%d problem(s) found - run 'check --fix' to repair what can be repaired\n", problems);
    } else {
        fprintf(out, "%d problem(s) could not be repaired automatically\n", problems);
    }
    return 0;
}
