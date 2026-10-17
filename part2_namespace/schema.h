/* schema.h - PART 2 (DBMS side): the files table, its rules, and the path view */
#ifndef SCHEMA_H
#define SCHEMA_H

#include "common.h"

/* Creates the files table, indexes, triggers, the root folder and the file_paths view. */
int files_schema_create(sqlite3 *db);

#endif
