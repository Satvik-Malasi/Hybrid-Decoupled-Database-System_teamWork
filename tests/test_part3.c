/* test_part3.c - PART 3: file contents + crash safety (OS), write-ahead log + recovery (DBMS) */
#include "testkit.h"
#include "tree.h"
#include "fileio.h"
#include "journal.h"
#include "check.h"
#include "space.h"

static int read_equals(Volume *v, const char *path, const char *expected) {
    unsigned char *data = NULL;
    int length = 0, same;
    if (file_read(v, path, &data, &length) != 0) return 0;
    same = length == (int)strlen(expected) && memcmp(data, expected, (size_t)length) == 0;
    free(data);
    return same;
}

static int check_is_clean(Volume *v) {
    FILE *sink = tmpfile();
    int remaining = -1;
    int ok = volume_check(v, 0, sink, &remaining) == 0 && remaining == 0;
    fclose(sink);
    return ok;
}

static void test_read_write(void) {
    Volume v;
    unsigned char big[MAX_FILE_SIZE + 1];
    unsigned char *back;
    int length, i;
    unsigned int free_start;
    int sizes[] = {0, 1, 1023, 1024, 1025, 5000, MAX_FILE_SIZE};

    CHECK(fresh_volume(&v, "p3.img", 2) == 0);
    CHECK(tree_make(&v, "/f.bin", MODE_FILE, 0, NULL) == 0);
    CHECK(tree_make(&v, "/dir", MODE_DIR, 0, NULL) == 0);
    free_start = disk_free_count(&v.disk);

    for (i = 0; i < (int)sizeof(big); i++) big[i] = (unsigned char)(i * 7 + 3);
    for (i = 0; i < (int)(sizeof(sizes) / sizeof(sizes[0])); i++) {
        int size = sizes[i];
        CHECK(file_write(&v, "/f.bin", big, size) == 0);
        CHECK(file_read(&v, "/f.bin", &back, &length) == 0);
        CHECK(length == size && (size == 0 || memcmp(back, big, (size_t)size) == 0));
        free(back);
        CHECK(disk_free_count(&v.disk) == free_start - (unsigned int)((size + BLOCK_SIZE - 1) / BLOCK_SIZE));
    }
    CHECK(check_is_clean(&v));

    CHECK(file_write(&v, "/f.bin", big, MAX_FILE_SIZE + 1) != 0);                   /* one byte too many */
    CHECK(contains(get_error(), "at most"));
    CHECK(file_write(&v, "/dir", big, 5) != 0);                                     /* folders hold no data */
    CHECK(file_write(&v, "/nothing", big, 5) != 0);
    CHECK(file_read(&v, "/dir", &back, &length) != 0);
    CHECK(file_read(&v, "/nothing", &back, &length) != 0);
    CHECK(file_write(&v, "/f.bin", big, -1) != 0);

    /* text with an embedded zero byte survives, because length is stored, not guessed */
    CHECK(file_write(&v, "/f.bin", (const unsigned char *)"ab\0cd", 5) == 0);
    CHECK(file_read(&v, "/f.bin", &back, &length) == 0 && length == 5 && back[2] == 0 && back[4] == 'd');
    free(back);

    /* the log keeps only the newest write of a file */
    CHECK(file_write(&v, "/f.bin", big, 3000) == 0);
    CHECK(file_write(&v, "/f.bin", big, 100) == 0);
    {
        int rows = 0;
        CHECK(db_get_int(v.db, "SELECT COUNT(*) FROM wal_log", &rows) == 0 && rows == 1);
        CHECK(db_get_int(v.db, "SELECT COUNT(*) FROM wal_log WHERE committed = 0", &rows) == 0 && rows == 0);
    }
    volume_close(&v);
    delete_volume_files("p3.img");
}

static void test_disk_full(void) {
    Volume v;
    unsigned char chunk[MAX_FILE_SIZE];
    char path[64];
    int i, last_ok = -1;
    unsigned int free_after_fill;

    memset(chunk, 'Z', sizeof(chunk));
    CHECK(fresh_volume(&v, "p3.img", 1) == 0);                                      /* 1022 data blocks */
    CHECK(tree_make(&v, "/victim", MODE_FILE, 0, NULL) == 0);
    CHECK(file_write(&v, "/victim", (const unsigned char *)"precious", 8) == 0);

    for (i = 0; i < 200; i++) {
        snprintf(path, sizeof(path), "/fill%d", i);
        if (tree_make(&v, path, MODE_FILE, 0, NULL) != 0) break;
        if (file_write(&v, path, chunk, sizeof(chunk)) != 0) break;
        last_ok = i;
    }
    CHECK(last_ok > 50 && last_ok < 200);
    CHECK(contains(get_error(), "full"));
    free_after_fill = disk_free_count(&v.disk);

    /* a failed write leaves the old contents and the old space accounting alone */
    CHECK(file_write(&v, "/victim", chunk, sizeof(chunk)) != 0);
    CHECK(read_equals(&v, "/victim", "precious"));
    CHECK(disk_free_count(&v.disk) == free_after_fill);
    CHECK(check_is_clean(&v));

    /* deleting makes room again */
    CHECK(tree_delete(&v, "/fill0", 0, NULL) == 0);
    CHECK(file_write(&v, "/victim", chunk, 2000) == 0);
    CHECK(check_is_clean(&v));
    volume_close(&v);
    delete_volume_files("p3.img");
}

static int crash_at(const char *step) {
    char command[256];
    snprintf(command, sizeof(command), "..\\..\\crash_sim.exe p3c.img /doc.txt %s", step);
    return run_program(command, "crash_out.txt");
}

static void test_crash(const char *step, int expect_new, int expect_undone, int expect_bitmap_fixes) {
    Volume v;

    CHECK(fresh_volume(&v, "p3c.img", 2) == 0);
    CHECK(tree_make(&v, "/doc.txt", MODE_FILE, 0, NULL) == 0);
    CHECK(file_write(&v, "/doc.txt", (const unsigned char *)"ORIGINAL CONTENT", 16) == 0);
    volume_close(&v);

    CHECK(crash_at(step) == CRASH_EXIT_CODE);                                       /* it really died at that step */

    CHECK(volume_open(&v, "p3c.img", 1) == 0);                                      /* open = recover */
    CHECK(v.undone_rows == expect_undone);
    CHECK(expect_bitmap_fixes ? v.bitmap_fixes > 0 : 1);
    if (expect_new) {
        CHECK(read_equals(&v, "/doc.txt", "THIS IS THE NEW CONTENT WRITTEN BY crash_sim"));
    } else {
        CHECK(read_equals(&v, "/doc.txt", "ORIGINAL CONTENT"));
    }
    CHECK(check_is_clean(&v));
    {
        int rows = 0;
        CHECK(db_get_int(v.db, "SELECT COUNT(*) FROM wal_log WHERE committed = 0", &rows) == 0 && rows == 0);
    }
    CHECK(journal_recover(&v) == 0 && v.undone_rows == 0 && v.redone_blocks == 0 && v.bitmap_fixes == 0);   /* idempotent */
    volume_close(&v);
    delete_volume_files("p3c.img");
}

static void test_crash_points(void) {
    test_crash("after_log", 0, 1, 1);          /* new block was reserved, nothing else happened */
    test_crash("after_data", 0, 1, 1);         /* data is on disk but the file does not point at it */
    test_crash("after_commit", 1, 0, 1);       /* the file already points at the new block; old block leaked */
}

static void test_redo(void) {
    Volume v;
    Entry e;
    unsigned char garbage[BLOCK_SIZE];

    CHECK(fresh_volume(&v, "p3.img", 2) == 0);
    CHECK(tree_make(&v, "/doc.txt", MODE_FILE, 0, NULL) == 0);
    CHECK(file_write(&v, "/doc.txt", (const unsigned char *)"committed data", 14) == 0);
    CHECK(tree_find(&v, "/doc.txt", &e) == 1);

    /* simulate a block that never reached the platter although the commit did */
    memset(garbage, 0x11, sizeof(garbage));
    CHECK(disk_write_block(&v.disk, (unsigned int)e.blocks[0], garbage) == 0);
    CHECK(!read_equals(&v, "/doc.txt", "committed data"));

    CHECK(journal_recover(&v) == 0);
    CHECK(v.redone_blocks == 1);
    CHECK(read_equals(&v, "/doc.txt", "committed data"));
    volume_close(&v);
    delete_volume_files("p3.img");
}

static void test_checker(void) {
    Volume v;
    Entry e;
    FILE *sink;
    char report[4096];
    size_t got;
    int remaining = -1, leaked_block;

    CHECK(fresh_volume(&v, "p3.img", 2) == 0);
    CHECK(tree_make(&v, "/one", MODE_FILE, 0, NULL) == 0);
    CHECK(tree_make(&v, "/two", MODE_FILE, 0, NULL) == 0);
    CHECK(file_write(&v, "/one", (const unsigned char *)"first", 5) == 0);
    CHECK(file_write(&v, "/two", (const unsigned char *)"second", 6) == 0);
    CHECK(check_is_clean(&v));

    /* a leaked block, an unfinished log row: both repairable */
    leaked_block = disk_alloc_block(&v.disk);
    CHECK(leaked_block > 0);
    CHECK(tree_find(&v, "/one", &e) == 1);
    CHECK(db_run(v.db, "INSERT INTO wal_log (file_id, block_no, data, committed) VALUES (2, 500, zeroblob(1024), 0)") == 0);

    sink = tmpfile();
    CHECK(volume_check(&v, 0, sink, &remaining) == 0 && remaining == 2);
    rewind(sink);
    got = fread(report, 1, sizeof(report) - 1, sink);
    report[got] = '\0';
    fclose(sink);
    CHECK(contains(report, "leaked") && contains(report, "unfinished log"));

    sink = tmpfile();
    CHECK(volume_check(&v, 1, sink, &remaining) == 0 && remaining == 0);
    fclose(sink);
    CHECK(check_is_clean(&v));

    /* damage the checker must report but must not pretend to fix */
    {
        char sql[200];
        snprintf(sql, sizeof(sql), "UPDATE files SET block0 = %d WHERE name = 'two'", e.blocks[0]);
        CHECK(db_run(v.db, sql) == 0);                                              /* two blocks now share one block */
    }
    sink = tmpfile();
    CHECK(volume_check(&v, 1, sink, &remaining) == 0 && remaining >= 1);
    rewind(sink);
    got = fread(report, 1, sizeof(report) - 1, sink);
    report[got] = '\0';
    fclose(sink);
    CHECK(contains(report, "owned by 2 entries"));

    CHECK(db_run(v.db, "UPDATE files SET block0 = 99999 WHERE name = 'two'") == 0); /* outside the disk */
    sink = tmpfile();
    CHECK(volume_check(&v, 0, sink, &remaining) == 0 && remaining >= 1);
    rewind(sink);
    got = fread(report, 1, sizeof(report) - 1, sink);
    report[got] = '\0';
    fclose(sink);
    CHECK(contains(report, "outside the data area"));

    CHECK(db_run(v.db, "UPDATE files SET block0 = 0 WHERE name = 'two'") == 0);     /* size 6 but no block */
    sink = tmpfile();
    CHECK(volume_check(&v, 0, sink, &remaining) == 0 && remaining >= 1);
    rewind(sink);
    got = fread(report, 1, sizeof(report) - 1, sink);
    report[got] = '\0';
    fclose(sink);
    CHECK(contains(report, "does not match its block pointers"));

    volume_close(&v);
    delete_volume_files("p3.img");
}

int main(void) {
    test_begin("part 3 - recovery (journal + fileio + check)");
    test_read_write();
    test_disk_full();
    test_crash_points();
    test_redo();
    test_checker();
    return test_end();
}
