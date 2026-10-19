/* test_part2.c - PART 2: directory tree (OS) and the files table rules (DBMS) */
#include "testkit.h"
#include "tree.h"

static int names_seen;
static char name_list[1024];

static void collect_name(const Entry *entry, void *context) {
    (void)context;
    names_seen++;
    strcat(name_list, entry->name);
    strcat(name_list, ",");
}

static void test_database_rules(void) {
    Volume v;
    CHECK(fresh_volume(&v, "p2.img", 2) == 0);

    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('a', 1, 16384)") == 0);
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('a', 1, 32768)") != 0);        /* duplicate name */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('x', 1, 12345)") != 0);        /* bad mode */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('', 1, 32768)") != 0);         /* empty name */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('p/q', 1, 32768)") != 0);      /* slash in name */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('..', 1, 16384)") != 0);
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('x', 999, 32768)") != 0);      /* no such parent */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('x', NULL, 32768)") != 0);     /* only root has none */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode, size) VALUES ('x', 1, 32768, 5)") != 0); /* must start empty */
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode, block3) VALUES ('x', 1, 32768, 9)") != 0);

    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('f', 1, 32768)") == 0);
    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('under_file', 3, 32768)") != 0); /* parent is a file */
    CHECK(db_run(v.db, "UPDATE files SET size = 20000 WHERE name = 'f'") != 0);                          /* too big */
    CHECK(db_run(v.db, "UPDATE files SET size = -1 WHERE name = 'f'") != 0);
    CHECK(db_run(v.db, "UPDATE files SET mode = 16384 WHERE name = 'f'") != 0);                          /* cannot change type */

    CHECK(db_run(v.db, "UPDATE files SET name = 'renamed' WHERE id = 1") != 0);                          /* root is fixed */
    CHECK(db_run(v.db, "DELETE FROM files WHERE id = 1") != 0);

    CHECK(db_run(v.db, "INSERT INTO files (name, parent_id, mode) VALUES ('inner', 2, 16384)") == 0);
    CHECK(db_run(v.db, "DELETE FROM files WHERE name = 'a'") != 0);                                      /* not empty */
    CHECK(db_run(v.db, "UPDATE files SET parent_id = 4 WHERE name = 'a'") != 0);                         /* into its own child */
    CHECK(db_run(v.db, "UPDATE files SET parent_id = 2 WHERE name = 'a'") != 0);                         /* into itself */
    CHECK(db_run(v.db, "UPDATE files SET parent_id = 2 WHERE name = 'f'") == 0);                         /* a normal move */
    CHECK(db_run(v.db, "UPDATE files SET parent_id = 3 WHERE name = 'inner'") != 0);                     /* target is a file */

    {
        int paths = 0;
        CHECK(db_get_int(v.db, "SELECT COUNT(*) FROM file_paths WHERE path = '/a/f'", &paths) == 0 && paths == 1);
        CHECK(db_get_int(v.db, "SELECT COUNT(*) FROM file_paths", &paths) == 0 && paths == 4);
    }

    /* modified_at moves forward by itself when a row changes */
    CHECK(db_run(v.db, "UPDATE files SET modified_at = 100 WHERE name = 'f'") == 0);
    CHECK(db_run(v.db, "UPDATE files SET owner_id = 7 WHERE name = 'f'") == 0);
    {
        int modified = 0;
        CHECK(db_get_int(v.db, "SELECT modified_at FROM files WHERE name = 'f'", &modified) == 0 && modified > 1000);
    }
    volume_close(&v);
    delete_volume_files("p2.img");
}

static void test_paths_and_listing(void) {
    Volume v;
    Entry e;

    CHECK(fresh_volume(&v, "p2.img", 2) == 0);
    CHECK(tree_make(&v, "/projects", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_make(&v, "/projects/alpha", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_make(&v, "/projects/alpha/notes.txt", MODE_FILE, 5, &e) == 0);
    CHECK(e.mode == MODE_FILE && e.owner_id == 5 && e.size == 0);

    CHECK(tree_make(&v, "/projects", MODE_DIR, 0, NULL) != 0);                      /* duplicates */
    CHECK(contains(get_error(), "already exists"));
    CHECK(tree_make(&v, "/projects/alpha/notes.txt", MODE_FILE, 0, NULL) != 0);
    CHECK(tree_make(&v, "/missing/child", MODE_FILE, 0, NULL) != 0);                /* parent missing */
    CHECK(tree_make(&v, "/projects/alpha/notes.txt/x", MODE_FILE, 0, NULL) != 0);   /* parent is a file */
    CHECK(tree_make(&v, "/", MODE_DIR, 0, NULL) != 0);

    CHECK(tree_find(&v, "/projects/alpha/notes.txt", &e) == 1);
    CHECK(tree_find(&v, "projects//alpha/./notes.txt", &e) == 1);                   /* messy but valid */
    CHECK(tree_find(&v, "/projects/alpha/../alpha/notes.txt", &e) == 1);
    CHECK(tree_find(&v, "/projects/../../..", &e) == 1 && e.id == ROOT_ID);         /* cannot climb above root */
    CHECK(tree_find(&v, "/projects/nothing", &e) == 0);
    CHECK(contains(get_error(), "no such file or folder"));
    CHECK(tree_find(&v, "/projects/alpha/notes.txt/deeper", &e) == 0);

    {
        char long_name[400];
        char path[450];
        memset(long_name, 'n', 256);
        long_name[256] = '\0';
        snprintf(path, sizeof(path), "/%s", long_name);
        CHECK(tree_make(&v, path, MODE_FILE, 0, NULL) != 0);
        memset(long_name, 'n', 255);
        long_name[255] = '\0';
        snprintf(path, sizeof(path), "/%s", long_name);
        CHECK(tree_make(&v, path, MODE_FILE, 0, NULL) == 0);
    }

    CHECK(tree_make(&v, "/projects/zeta", MODE_FILE, 0, NULL) == 0);
    CHECK(tree_make(&v, "/projects/Beta", MODE_FILE, 0, NULL) == 0);
    names_seen = 0;
    name_list[0] = '\0';
    CHECK(tree_list(&v, "/projects", collect_name, NULL) == 0);
    CHECK(names_seen == 3);
    CHECK(strcmp(name_list, "Beta,alpha,zeta,") == 0);                              /* sorted by name */

    names_seen = 0;
    CHECK(tree_list(&v, "/projects/zeta", collect_name, NULL) == 0 && names_seen == 1);   /* a file lists itself */
    CHECK(tree_list(&v, "/nope", collect_name, NULL) != 0);

    {
        char path[MAX_PATH_LEN];
        CHECK(tree_find(&v, "/projects/alpha/notes.txt", &e) == 1);
        CHECK(tree_path_of(&v, e.id, path, sizeof(path)) == 0 && strcmp(path, "/projects/alpha/notes.txt") == 0);
        CHECK(tree_path_of(&v, ROOT_ID, path, sizeof(path)) == 0 && strcmp(path, "/") == 0);
    }

    CHECK(tree_set_owner(&v, "/projects/alpha/notes.txt", 42) == 0);
    CHECK(tree_find(&v, "/projects/alpha/notes.txt", &e) == 1 && e.owner_id == 42);
    CHECK(tree_set_owner(&v, "/", 3) != 0);
    CHECK(tree_set_owner(&v, "/ghost", 3) != 0);

    volume_close(&v);
    delete_volume_files("p2.img");
}

static void test_rename(void) {
    Volume v;
    Entry e;

    CHECK(fresh_volume(&v, "p2.img", 2) == 0);
    CHECK(tree_make(&v, "/a", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_make(&v, "/b", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_make(&v, "/a/one.txt", MODE_FILE, 0, NULL) == 0);
    CHECK(tree_make(&v, "/b/one.txt", MODE_FILE, 0, NULL) == 0);
    CHECK(tree_make(&v, "/two.txt", MODE_FILE, 0, NULL) == 0);

    CHECK(tree_rename(&v, "/two.txt", "/three.txt") == 0);                           /* plain rename */
    CHECK(tree_find(&v, "/two.txt", &e) == 0 && tree_find(&v, "/three.txt", &e) == 1);
    CHECK(tree_rename(&v, "/three.txt", "/a") == 0);                                 /* into an existing folder */
    CHECK(tree_find(&v, "/a/three.txt", &e) == 1);
    CHECK(tree_rename(&v, "/a/one.txt", "/b") != 0);                                 /* name already taken there */
    CHECK(tree_rename(&v, "/a/one.txt", "/b/one.txt") != 0);                         /* file in the way */
    CHECK(tree_rename(&v, "/a/one.txt", "/a/one.txt") == 0);                         /* onto itself: nothing to do */
    CHECK(tree_rename(&v, "/a", "/a/inside") != 0);                                  /* folder into itself */
    CHECK(tree_rename(&v, "/", "/z") != 0);
    CHECK(tree_rename(&v, "/ghost", "/z") != 0);
    CHECK(tree_rename(&v, "/a/one.txt", "/ghost/z") != 0);
    CHECK(tree_rename(&v, "/a/one.txt", "/b/one.txt/z") != 0);
    CHECK(tree_rename(&v, "/a", "/b/a2") == 0);                                      /* a whole folder moves with its contents */
    CHECK(tree_find(&v, "/b/a2/three.txt", &e) == 1);

    volume_close(&v);
    delete_volume_files("p2.img");
}

static void test_delete(void) {
    Volume v;
    Entry e;
    int removed = 0, file_id, block_a, block_b, block_c;
    unsigned int free_before;
    char sql[200];

    CHECK(fresh_volume(&v, "p2.img", 2) == 0);
    CHECK(tree_make(&v, "/top", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_make(&v, "/top/mid", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_make(&v, "/top/mid/deep.txt", MODE_FILE, 0, &e) == 0);
    file_id = e.id;
    CHECK(tree_make(&v, "/top/side.txt", MODE_FILE, 0, NULL) == 0);
    CHECK(tree_make(&v, "/keep.txt", MODE_FILE, 0, NULL) == 0);

    /* give deep.txt two blocks so we can see them come back */
    free_before = disk_free_count(&v.disk);
    block_a = disk_alloc_block(&v.disk);
    block_b = disk_alloc_block(&v.disk);
    block_c = disk_alloc_block(&v.disk);
    snprintf(sql, sizeof(sql), "UPDATE files SET size = 1500, block0 = %d, block1 = %d WHERE id = %d", block_a, block_b, file_id);
    CHECK(db_run(v.db, sql) == 0);
    CHECK(disk_free_count(&v.disk) == free_before - 3);
    CHECK(disk_free_block(&v.disk, (unsigned int)block_c) == 0);
    free_before = disk_free_count(&v.disk);

    CHECK(tree_delete(&v, "/top", 0, &removed) != 0);                                /* not empty, no -r */
    CHECK(contains(get_error(), "rm -r"));
    CHECK(tree_find(&v, "/top/mid/deep.txt", &e) == 1);                              /* nothing was removed */

    CHECK(tree_delete(&v, "/top", 1, &removed) == 0);
    CHECK(removed == 4);                                                             /* top, mid, deep.txt, side.txt */
    CHECK(tree_find(&v, "/top", &e) == 0);
    CHECK(tree_find(&v, "/keep.txt", &e) == 1);                                      /* neighbours untouched */
    CHECK(!disk_is_used(&v.disk, (unsigned int)block_a) && !disk_is_used(&v.disk, (unsigned int)block_b));
    CHECK(disk_free_count(&v.disk) == free_before + 2);

    CHECK(tree_delete(&v, "/keep.txt", 0, &removed) == 0 && removed == 1);           /* a plain file */
    CHECK(tree_delete(&v, "/keep.txt", 0, &removed) != 0);                           /* already gone */
    CHECK(tree_delete(&v, "/", 1, &removed) != 0);                                   /* never the root */
    CHECK(tree_delete(&v, "/..", 1, &removed) != 0);

    CHECK(tree_make(&v, "/empty", MODE_DIR, 0, NULL) == 0);
    CHECK(tree_delete(&v, "/empty", 0, &removed) == 0 && removed == 1);              /* empty folder, no -r needed */
    CHECK(tree_make(&v, "/empty", MODE_DIR, 0, NULL) == 0);                          /* and the name is free again */

    volume_close(&v);
    delete_volume_files("p2.img");
}

int main(void) {
    test_begin("part 2 - namespace (tree + files table)");
    test_database_rules();
    test_paths_and_listing();
    test_rename();
    test_delete();
    return test_end();
}
