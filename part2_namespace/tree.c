/* tree.c - PART 2 (OS side): path walking and the namespace operations */
#include "tree.h"

#define MAX_DEPTH 128

typedef struct {
    char name[MAX_NAME + 1];
} Piece;

void tree_load_entry(sqlite3_stmt *stmt, Entry *entry) {
    const unsigned char *name;
    int i;

    memset(entry, 0, sizeof(*entry));
    entry->id = sqlite3_column_int(stmt, 0);
    name = sqlite3_column_text(stmt, 1);
    snprintf(entry->name, sizeof(entry->name), "%s", name ? (const char *)name : "");
    entry->parent_id = sqlite3_column_int(stmt, 2);
    entry->owner_id = sqlite3_column_int(stmt, 3);
    entry->mode = sqlite3_column_int(stmt, 4);
    entry->size = sqlite3_column_int(stmt, 5);
    entry->created_at = (long)sqlite3_column_int64(stmt, 6);
    entry->modified_at = (long)sqlite3_column_int64(stmt, 7);
    for (i = 0; i < MAX_FILE_BLOCKS; i++) entry->blocks[i] = sqlite3_column_int(stmt, 8 + i);
}

/* Splits "/a/./b/../c" into pieces. ".." steps back one level. Returns the piece count or -1. */
static int split_path(const char *path, Piece *pieces, int max_pieces) {
    int count = 0;
    const char *p = path;

    while (*p) {
        const char *start;
        size_t length;

        while (*p == '/') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != '/') p++;
        length = (size_t)(p - start);

        if (length == 1 && start[0] == '.') continue;
        if (length == 2 && start[0] == '.' && start[1] == '.') {
            if (count > 0) count--;
            continue;
        }
        if (length > MAX_NAME) {
            set_error("name too long (limit is %d characters)", MAX_NAME);
            return -1;
        }
        if (count >= max_pieces) {
            set_error("path is nested too deeply");
            return -1;
        }
        memcpy(pieces[count].name, start, length);
        pieces[count].name[length] = '\0';
        count++;
    }
    return count;
}

static int load_by_id(Volume *v, int id, Entry *entry) {
    sqlite3_stmt *stmt;
    int found = 0;

    if (sqlite3_prepare_v2(v->db, "SELECT " ENTRY_COLUMNS " FROM files WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        tree_load_entry(stmt, entry);
        found = 1;
    }
    sqlite3_finalize(stmt);
    if (!found) set_error("entry %d does not exist", id);
    return found ? 0 : -1;
}

static int find_child(Volume *v, int parent_id, const char *name, Entry *entry) {
    sqlite3_stmt *stmt;
    int found = 0;

    if (sqlite3_prepare_v2(v->db, "SELECT " ENTRY_COLUMNS " FROM files WHERE parent_id = ? AND name = ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, parent_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        tree_load_entry(stmt, entry);
        found = 1;
    }
    sqlite3_finalize(stmt);
    return found;
}

/* Walks the first `count` pieces down from the root. 0 on success, -1 with a message otherwise. */
static int walk(Volume *v, const Piece *pieces, int count, const char *shown_path, Entry *entry) {
    int i;

    if (load_by_id(v, ROOT_ID, entry) != 0) return -1;
    for (i = 0; i < count; i++) {
        int status;
        if (entry->mode != MODE_DIR) {
            set_error("'%s' is not a folder", entry->name);
            return -1;
        }
        status = find_child(v, entry->id, pieces[i].name, entry);
        if (status < 0) return -1;
        if (status == 0) {
            set_error("no such file or folder: %s", shown_path);
            return -1;
        }
    }
    return 0;
}

int tree_find(Volume *v, const char *path, Entry *entry) {
    Piece pieces[MAX_DEPTH];
    int count = split_path(path, pieces, MAX_DEPTH);
    if (count < 0) return 0;
    return walk(v, pieces, count, path, entry) == 0 ? 1 : 0;
}

int tree_make(Volume *v, const char *path, int mode, int owner_id, Entry *created) {
    Piece pieces[MAX_DEPTH];
    Entry parent, existing;
    sqlite3_stmt *stmt;
    int count = split_path(path, pieces, MAX_DEPTH);
    int status;

    if (count < 0) return -1;
    if (count == 0) {
        set_error("'/' already exists");
        return -1;
    }
    if (walk(v, pieces, count - 1, path, &parent) != 0) return -1;
    if (parent.mode != MODE_DIR) {
        set_error("cannot create inside '%s': it is a file", parent.name);
        return -1;
    }
    status = find_child(v, parent.id, pieces[count - 1].name, &existing);
    if (status < 0) return -1;
    if (status == 1) {
        set_error("'%s' already exists", path);
        return -1;
    }

    if (sqlite3_prepare_v2(v->db, "INSERT INTO files (name, parent_id, owner_id, mode) VALUES (?, ?, ?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, pieces[count - 1].name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, parent.id);
    sqlite3_bind_int(stmt, 3, owner_id);
    sqlite3_bind_int(stmt, 4, mode);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    if (created) return load_by_id(v, (int)sqlite3_last_insert_rowid(v->db), created);
    return 0;
}

int tree_list(Volume *v, const char *path, EntryVisitor visit, void *context) {
    Entry target, child;
    sqlite3_stmt *stmt;

    if (!tree_find(v, path, &target)) return -1;
    if (target.mode != MODE_DIR) {
        visit(&target, context);
        return 0;
    }
    if (sqlite3_prepare_v2(v->db, "SELECT " ENTRY_COLUMNS " FROM files WHERE parent_id = ? ORDER BY name",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, target.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        tree_load_entry(stmt, &child);
        visit(&child, context);
    }
    sqlite3_finalize(stmt);
    return 0;
}

/* Gives back every block the entries owned, once the delete has been committed. */
static void release_blocks(Volume *v, const int *blocks, int block_count) {
    int i;
    if (!v->disk_ready) return;
    for (i = 0; i < block_count; i++) disk_free_block(&v->disk, (unsigned int)blocks[i]);
}

/* Grows a plain int array by one slot when it is full. Returns 0, or -1 when out of memory. */
static int append_int(int **list, int *count, int *capacity, int value) {
    if (*count == *capacity) {
        int new_capacity = *capacity ? *capacity * 2 : 64;
        int *bigger = realloc(*list, (size_t)new_capacity * sizeof(int));
        if (!bigger) {
            set_error("out of memory");
            return -1;
        }
        *list = bigger;
        *capacity = new_capacity;
    }
    (*list)[(*count)++] = value;
    return 0;
}

int tree_delete(Volume *v, const char *path, int recursive, int *removed) {
    Entry target, item;
    sqlite3_stmt *stmt;
    int *ids = NULL, *freed = NULL;
    int id_count = 0, id_capacity = 0, freed_count = 0, freed_capacity = 0;
    int i, b, has_children = 0, result = -1;

    if (removed) *removed = 0;
    if (!tree_find(v, path, &target)) return -1;
    if (target.id == ROOT_ID) {
        set_error("the root folder cannot be deleted");
        return -1;
    }

    if (target.mode == MODE_DIR) {
        if (sqlite3_prepare_v2(v->db, "SELECT 1 FROM files WHERE parent_id = ? LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) {
            set_error("%s", sqlite3_errmsg(v->db));
            return -1;
        }
        sqlite3_bind_int(stmt, 1, target.id);
        has_children = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
        if (has_children && !recursive) {
            set_error("'%s' is not empty - use rm -r to delete it with everything inside", path);
            return -1;
        }
    }

    /* collect the entry and everything under it, deepest first so folders are emptied before they go */
    if (sqlite3_prepare_v2(v->db,
            "WITH RECURSIVE sub(id, depth) AS ("
            "  SELECT ?, 0 UNION ALL SELECT f.id, sub.depth + 1 FROM files f JOIN sub ON f.parent_id = sub.id) "
            "SELECT id FROM sub ORDER BY depth DESC", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, target.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (append_int(&ids, &id_count, &id_capacity, sqlite3_column_int(stmt, 0)) != 0) {
            sqlite3_finalize(stmt);
            goto done;
        }
    }
    sqlite3_finalize(stmt);

    if (db_run(v->db, "BEGIN IMMEDIATE") != 0) goto done;
    for (i = 0; i < id_count; i++) {
        if (load_by_id(v, ids[i], &item) != 0) goto rollback;
        if (item.mode == MODE_FILE) {
            for (b = 0; b < MAX_FILE_BLOCKS; b++) {
                if (item.blocks[b] > 0 && append_int(&freed, &freed_count, &freed_capacity, item.blocks[b]) != 0) {
                    goto rollback;
                }
            }
        }
        if (sqlite3_prepare_v2(v->db, "DELETE FROM files WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) {
            set_error("%s", sqlite3_errmsg(v->db));
            goto rollback;
        }
        sqlite3_bind_int(stmt, 1, ids[i]);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            set_error("%s", sqlite3_errmsg(v->db));
            sqlite3_finalize(stmt);
            goto rollback;
        }
        sqlite3_finalize(stmt);
    }
    if (db_run(v->db, "COMMIT") != 0) goto rollback;

    /* only after the commit: if we stop here the bitmap is rebuilt from metadata at the next open */
    release_blocks(v, freed, freed_count);
    if (removed) *removed = id_count;
    result = 0;
    goto done;

rollback:
    {
        char saved[512];
        snprintf(saved, sizeof(saved), "%s", get_error());
        sqlite3_exec(v->db, "ROLLBACK", NULL, NULL, NULL);
        set_error("%s", saved);
    }
done:
    free(ids);
    free(freed);
    return result;
}

int tree_rename(Volume *v, const char *from, const char *to) {
    Piece pieces[MAX_DEPTH];
    Entry source, target_parent, existing;
    sqlite3_stmt *stmt;
    char new_name[MAX_NAME + 1];
    int count, status;

    if (!tree_find(v, from, &source)) return -1;
    if (source.id == ROOT_ID) {
        set_error("the root folder cannot be moved");
        return -1;
    }

    count = split_path(to, pieces, MAX_DEPTH);
    if (count < 0) return -1;
    if (count == 0) {
        set_error("'/' already exists");
        return -1;
    }
    if (walk(v, pieces, count - 1, to, &target_parent) != 0) return -1;
    snprintf(new_name, sizeof(new_name), "%s", pieces[count - 1].name);

    status = find_child(v, target_parent.id, new_name, &existing);
    if (status < 0) return -1;
    if (status == 1) {
        if (existing.id == source.id) return 0;             /* moving onto itself changes nothing */
        if (existing.mode != MODE_DIR) {
            set_error("'%s' already exists", to);
            return -1;
        }
        /* the target is a folder, so the entry moves inside it and keeps its name */
        target_parent = existing;
        snprintf(new_name, sizeof(new_name), "%s", source.name);
        status = find_child(v, target_parent.id, new_name, &existing);
        if (status < 0) return -1;
        if (status == 1) {
            set_error("'%s' already has an entry called '%s'", to, new_name);
            return -1;
        }
    }
    if (target_parent.mode != MODE_DIR) {
        set_error("'%s' is a file, not a folder", target_parent.name);
        return -1;
    }

    if (sqlite3_prepare_v2(v->db, "UPDATE files SET name = ?, parent_id = ? WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, target_parent.id);
    sqlite3_bind_int(stmt, 3, source.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int tree_set_owner(Volume *v, const char *path, int owner_id) {
    Entry target;
    sqlite3_stmt *stmt;

    if (!tree_find(v, path, &target)) return -1;
    if (target.id == ROOT_ID) {
        set_error("the owner of the root folder cannot be changed");
        return -1;
    }
    if (sqlite3_prepare_v2(v->db, "UPDATE files SET owner_id = ? WHERE id = ?", -1, &stmt, NULL) != SQLITE_OK) {
        set_error("%s", sqlite3_errmsg(v->db));
        return -1;
    }
    sqlite3_bind_int(stmt, 1, owner_id);
    sqlite3_bind_int(stmt, 2, target.id);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        set_error("%s", sqlite3_errmsg(v->db));
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

int tree_path_of(Volume *v, int id, char *path, size_t capacity) {
    Entry node;
    char names[MAX_DEPTH][MAX_NAME + 1];
    int depth = 0, i;
    size_t used = 0;

    if (id == ROOT_ID) {
        snprintf(path, capacity, "/");
        return 0;
    }
    while (id != ROOT_ID) {
        if (depth >= MAX_DEPTH || load_by_id(v, id, &node) != 0) {
            set_error("cannot build the path of entry %d", id);
            return -1;
        }
        snprintf(names[depth++], sizeof(names[0]), "%s", node.name);
        id = node.parent_id;
        if (id == 0) break;
    }
    path[0] = '\0';
    for (i = depth - 1; i >= 0; i--) {
        int written = snprintf(path + used, capacity - used, "/%s", names[i]);
        if (written < 0 || (size_t)written >= capacity - used) {
            set_error("path is too long");
            return -1;
        }
        used += (size_t)written;
    }
    return 0;
}
