/* tree.h - PART 2 (OS side): the directory tree - paths, create, list, rename, delete */
#ifndef TREE_H
#define TREE_H

#include "volume.h"

typedef struct {
    int id;
    int parent_id;                      /* 0 for the root */
    int owner_id;
    int mode;                           /* MODE_FILE or MODE_DIR */
    int size;
    long created_at;
    long modified_at;
    int blocks[MAX_FILE_BLOCKS];
    char name[MAX_NAME + 1];
} Entry;

/* Column list matching tree_load_entry(). Handy for other parts that select rows from files. */
#define ENTRY_COLUMNS \
    "id, name, IFNULL(parent_id, 0), owner_id, mode, size, created_at, modified_at, " \
    "block0, block1, block2, block3, block4, block5, block6, block7, block8, block9, block10, block11"

void tree_load_entry(sqlite3_stmt *stmt, Entry *entry);

/* 1 when found. 0 when not (the reason is in get_error()). */
int tree_find(Volume *v, const char *path, Entry *entry);

/* Creates a file or folder. 0 on success, -1 with an error message. */
int tree_make(Volume *v, const char *path, int mode, int owner_id, Entry *created);

typedef void (*EntryVisitor)(const Entry *entry, void *context);

/* Lists a folder's children (or the single entry when path is a file), sorted by name. */
int tree_list(Volume *v, const char *path, EntryVisitor visit, void *context);

/* Removes a file, or a folder. A folder with contents needs recursive = 1.
 * removed (optional) receives how many entries were deleted. */
int tree_delete(Volume *v, const char *path, int recursive, int *removed);

/* Renames or moves. If the target is an existing folder the entry goes inside it. */
int tree_rename(Volume *v, const char *from, const char *to);

int tree_set_owner(Volume *v, const char *path, int owner_id);

/* Builds the full path of an entry id. */
int tree_path_of(Volume *v, int id, char *path, size_t capacity);

#endif
