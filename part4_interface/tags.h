/* tags.h - PART 4 (DBMS side): searchable key/value tags on files and folders
 *
 * Tags live in their own table, one row per (entry, key), with an index on
 * (key, value) so a search does not have to read every file.
 */
#ifndef TAGS_H
#define TAGS_H

#include "volume.h"

#define MAX_TAG_KEY   64
#define MAX_TAG_VALUE 255
#define MAX_SEARCH_TERMS 8

int tags_schema_create(sqlite3 *db);

/* Adds the tag, or replaces the value if the key is already there. */
int tags_set(Volume *v, const char *path, const char *key, const char *value);

/* Removes one tag. An error if the entry does not have it. */
int tags_remove(Volume *v, const char *path, const char *key);

typedef void (*TagVisitor)(const char *key, const char *value, void *context);
int tags_list(Volume *v, const char *path, TagVisitor visit, void *context);

/* Each term is "key" (has the tag) or "key=value" (has the tag with exactly that value).
 * An entry must match every term. */
typedef void (*MatchVisitor)(const char *path, void *context);
int tags_search(Volume *v, int term_count, char **terms, MatchVisitor visit, void *context);

#endif
