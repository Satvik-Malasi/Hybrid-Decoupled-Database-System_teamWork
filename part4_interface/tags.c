/* tags.c - PART 4 (DBMS side): the file_tags table and its queries */
#include "tags.h"
#include "tree.h"

int tags_schema_create(sqlite3 *db) {
    if (db_run(db,
        "CREATE TABLE file_tags ("
        " file_id   INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        " tag_key   TEXT    NOT NULL CHECK (length(tag_key) BETWEEN 1 AND 64 AND tag_key NOT GLOB '*[^A-Za-z0-9_.-]*'),"
        " tag_value TEXT    NOT NULL DEFAULT '' CHECK (length(tag_value) <= 255),"
        " PRIMARY KEY (file_id, tag_key))") != 0) return -1;
    return db_run(db, "CREATE INDEX idx_file_tags_lookup ON file_tags(tag_key, tag_value)");
}

static int key_is_valid(const char *key) {
    size_t i, length = strlen(key);
    if (length < 1 || length > MAX_TAG_KEY) return 0;
    for (i = 0; i < length; i++) {
        char c = key[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '.' || c == '-';
        if (!ok) return 0;
    }
    return 1;
}

static int check_key(const char *key) {
    if (!key_is_valid(key)) {
        set_error("a tag name is 1-%d characters: letters, digits, '_', '.' or '-'", MAX_TAG_KEY);
        return -1;
    }
    return 0;
}

int tags_set(Volume *v, const char *path, const char *key, const char *value) {
    Entry target;
    sqlite3_stmt *stmt;
    int status;

    if (check_key(key) != 0) return -1;
    if (strlen(value) > MAX_TAG_VALUE) {
        set_error("a tag value can have at most %d characters", MAX_TAG_VALUE);
        return -1;
    }
    if (!tree_find(v, path, &target)) return -1;

    if (sqlite3_prepare_v2(v->db,
            "INSERT INTO file_tags (file_id, tag_key, tag_value) VALUES (?, ?, ?) "
            "ON CONFLICT(file_id, tag_key) DO UPDATE SET tag_value = excluded.tag_value",
            -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, target.id);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
    status = sqlite3_step(stmt);
    if (status != SQLITE_DONE) set_error("%s", sqlite3_errmsg(v->db));
    sqlite3_finalize(stmt);
    return status == SQLITE_DONE ? 0 : -1;
}

int tags_remove(Volume *v, const char *path, const char *key) {
    Entry target;
    sqlite3_stmt *stmt;
    int status;

    if (!tree_find(v, path, &target)) return -1;
    if (sqlite3_prepare_v2(v->db, "DELETE FROM file_tags WHERE file_id = ? AND tag_key = ?", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, target.id);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    status = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (status != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    if (sqlite3_changes(v->db) == 0) {
        set_error("'%s' has no tag called '%s'", path, key);
        return -1;
    }
    return 0;
}

int tags_list(Volume *v, const char *path, TagVisitor visit, void *context) {
    Entry target;
    sqlite3_stmt *stmt;

    if (!tree_find(v, path, &target)) return -1;
    if (sqlite3_prepare_v2(v->db, "SELECT tag_key, tag_value FROM file_tags WHERE file_id = ? ORDER BY tag_key",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, target.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        visit((const char *)sqlite3_column_text(stmt, 0), (const char *)sqlite3_column_text(stmt, 1), context);
    }
    sqlite3_finalize(stmt);
    return 0;
}

int tags_search(Volume *v, int term_count, char **terms, MatchVisitor visit, void *context) {
    char sql[2048];
    char keys[MAX_SEARCH_TERMS][MAX_TAG_KEY + 1];
    const char *values[MAX_SEARCH_TERMS];
    sqlite3_stmt *stmt;
    size_t used = 0;
    int i, bind_at = 1;
    int *ids = NULL, id_count = 0, id_capacity = 0;

    if (term_count < 1) {
        set_error("give at least one tag to look for, like: search type or search type=report");
        return -1;
    }
    if (term_count > MAX_SEARCH_TERMS) {
        set_error("at most %d search terms are allowed", MAX_SEARCH_TERMS);
        return -1;
    }

    for (i = 0; i < term_count; i++) {
        char *equals = strchr(terms[i], '=');
        size_t key_length = equals ? (size_t)(equals - terms[i]) : strlen(terms[i]);
        if (key_length > MAX_TAG_KEY) key_length = MAX_TAG_KEY + 1;
        if (key_length > MAX_TAG_KEY) {
            set_error("a tag name is at most %d characters", MAX_TAG_KEY);
            return -1;
        }
        memcpy(keys[i], terms[i], key_length);
        keys[i][key_length] = '\0';
        if (check_key(keys[i]) != 0) return -1;
        values[i] = equals ? equals + 1 : NULL;
    }

    used += snprintf(sql + used, sizeof(sql) - used, "SELECT t0.file_id FROM file_tags t0");
    for (i = 1; i < term_count; i++) {
        used += snprintf(sql + used, sizeof(sql) - used, " JOIN file_tags t%d ON t%d.file_id = t0.file_id", i, i);
    }
    used += snprintf(sql + used, sizeof(sql) - used, " WHERE 1 = 1");
    for (i = 0; i < term_count; i++) {
        used += snprintf(sql + used, sizeof(sql) - used, " AND t%d.tag_key = ?", i);
        if (values[i]) used += snprintf(sql + used, sizeof(sql) - used, " AND t%d.tag_value = ?", i);
    }
    snprintf(sql + used, sizeof(sql) - used, " ORDER BY t0.file_id");

    if (sqlite3_prepare_v2(v->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    for (i = 0; i < term_count; i++) {
        sqlite3_bind_text(stmt, bind_at++, keys[i], -1, SQLITE_STATIC);
        if (values[i]) sqlite3_bind_text(stmt, bind_at++, values[i], -1, SQLITE_STATIC);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (id_count == id_capacity) {
            int new_capacity = id_capacity ? id_capacity * 2 : 32;
            int *bigger = realloc(ids, (size_t)new_capacity * sizeof(int));
            if (!bigger) {
                set_error("out of memory");
                sqlite3_finalize(stmt);
                free(ids);
                return -1;
            }
            ids = bigger;
            id_capacity = new_capacity;
        }
        ids[id_count++] = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    for (i = 0; i < id_count; i++) {
        char path[MAX_PATH_LEN];
        if (tree_path_of(v, ids[i], path, sizeof(path)) == 0) visit(path, context);
    }
    free(ids);
    return 0;
}
