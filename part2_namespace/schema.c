
/* schema.c - PART 2 (DBMS side)
 *
 * The rules of the namespace are enforced by the database itself, so no tool
 * (shell, SQL console, a future program) can leave the tree in a broken state:
 *   - CHECK constraints: mode is file or folder, size fits 12 blocks, sane names
 *   - UNIQUE index: two entries in one folder cannot share a name
 *   - triggers: parent must be a folder, no moving a folder into itself,
 *     the root cannot be changed or deleted, new rows start empty
 */
#include "schema.h"

int files_schema_create(sqlite3 *db) {
    char sql[4096];
    size_t used = 0;
    int i;

    used += snprintf(sql + used, sizeof(sql) - used,
        "CREATE TABLE files ("
        " id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name        TEXT    NOT NULL,"
        " parent_id   INTEGER REFERENCES files(id) ON DELETE RESTRICT,"
        " owner_id    INTEGER NOT NULL DEFAULT 0,"
        " mode        INTEGER NOT NULL CHECK (mode IN (16384, 32768)),"
        " size        INTEGER NOT NULL DEFAULT 0 CHECK (size BETWEEN 0 AND %d),"
        " created_at  INTEGER NOT NULL DEFAULT (strftime('%%s','now')),"
        " modified_at INTEGER NOT NULL DEFAULT (strftime('%%s','now')),",
        MAX_FILE_SIZE);
    for (i = 0; i < MAX_FILE_BLOCKS; i++) {
        used += snprintf(sql + used, sizeof(sql) - used,
                         " block%d INTEGER NOT NULL DEFAULT 0 CHECK (block%d >= 0),", i, i);
    }
    used += snprintf(sql + used, sizeof(sql) - used,
        " CHECK (id = 1 OR parent_id IS NOT NULL),"
        " CHECK (id = 1 OR (length(name) BETWEEN 1 AND %d AND instr(name, '/') = 0"
        "                   AND name NOT IN ('.', '..'))),"
        " CHECK (mode = 32768 OR size = 0)"
        ")", MAX_NAME);
    if (db_run(db, sql) != 0) return -1;

    if (db_run(db, "CREATE UNIQUE INDEX idx_files_parent_name ON files(parent_id, name)") != 0) return -1;

    /* the root folder goes in before the triggers exist */
    if (db_run(db, "INSERT INTO files (id, name, parent_id, mode) VALUES (1, '/', NULL, 16384)") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_new_row BEFORE INSERT ON files "
        "BEGIN "
        " SELECT RAISE(ABORT, 'parent must be an existing folder') "
        "  WHERE (SELECT mode FROM files WHERE id = NEW.parent_id) IS NOT 16384; "
        " SELECT RAISE(ABORT, 'a new entry must start empty (use the write command to add data)') "
        "  WHERE NEW.size <> 0"
        "     OR NEW.block0 <> 0 OR NEW.block1 <> 0 OR NEW.block2 <> 0 OR NEW.block3 <> 0"
        "     OR NEW.block4 <> 0 OR NEW.block5 <> 0 OR NEW.block6 <> 0 OR NEW.block7 <> 0"
        "     OR NEW.block8 <> 0 OR NEW.block9 <> 0 OR NEW.block10 <> 0 OR NEW.block11 <> 0; "
        "END") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_root_update BEFORE UPDATE ON files WHEN OLD.id = 1 "
        "BEGIN SELECT RAISE(ABORT, 'the root folder cannot be changed'); END") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_root_delete BEFORE DELETE ON files WHEN OLD.id = 1 "
        "BEGIN SELECT RAISE(ABORT, 'the root folder cannot be deleted'); END") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_keep_mode BEFORE UPDATE OF mode ON files WHEN NEW.mode <> OLD.mode "
        "BEGIN SELECT RAISE(ABORT, 'a file cannot be turned into a folder or the other way round'); END") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_move_check BEFORE UPDATE OF parent_id ON files "
        "WHEN NEW.parent_id IS NOT OLD.parent_id "
        "BEGIN "
        " SELECT RAISE(ABORT, 'parent must be an existing folder') "
        "  WHERE (SELECT mode FROM files WHERE id = NEW.parent_id) IS NOT 16384; "
        " SELECT RAISE(ABORT, 'a folder cannot be moved into itself or one of its own subfolders') "
        "  WHERE EXISTS (WITH RECURSIVE above(id) AS ("
        "      SELECT NEW.parent_id "
        "      UNION ALL SELECT f.parent_id FROM files f JOIN above ON f.id = above.id "
        "                WHERE f.parent_id IS NOT NULL) "
        "    SELECT 1 FROM above WHERE id = OLD.id); "
        "END") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_not_empty BEFORE DELETE ON files "
        "WHEN EXISTS (SELECT 1 FROM files WHERE parent_id = OLD.id) "
        "BEGIN SELECT RAISE(ABORT, 'folder is not empty'); END") != 0) return -1;

    if (db_run(db,
        "CREATE TRIGGER files_touch AFTER UPDATE ON files "
        "WHEN NEW.modified_at = OLD.modified_at "
        "BEGIN UPDATE files SET modified_at = strftime('%s','now') WHERE id = NEW.id; END") != 0) return -1;

    return db_run(db,
        "CREATE VIEW file_paths AS "
        "WITH RECURSIVE walk(id, path) AS ("
        "  SELECT id, '/' FROM files WHERE id = 1 "
        "  UNION ALL "
        "  SELECT f.id, CASE WHEN walk.path = '/' THEN '/' || f.name ELSE walk.path || '/' || f.name END "
        "  FROM files f JOIN walk ON f.parent_id = walk.id) "
        "SELECT id, path FROM walk");
}
