/* common.h - constants and small helpers used by every part of HybridFS */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

#define BLOCK_SIZE       1024                         /* bytes in one disk block */
#define MAX_NAME         255                          /* longest file or folder name */
#define MAX_FILE_BLOCKS  12                           /* a file points at 12 blocks directly */
#define MAX_FILE_SIZE    (BLOCK_SIZE * MAX_FILE_BLOCKS)
#define MAX_PATH_LEN     1024

#define MODE_DIR         0x4000                       /* 16384: the row is a folder */
#define MODE_FILE        0x8000                       /* 32768: the row is a file */
#define ROOT_ID          1                            /* the "/" folder is always row 1 */
#define SCHEMA_VERSION   2                            /* stored in the database as user_version */

/* Every function that fails returns -1 (or 0 for "not found") and leaves a message here. */
void set_error(const char *format, ...);
const char *get_error(void);

/* Run SQL that returns no rows; -1 and an error message on failure. */
int db_run(sqlite3 *db, const char *sql);

/* Read one number (first column of first row). Gives 0 when the query returns nothing. */
int db_get_int(sqlite3 *db, const char *sql, int *result);

#endif
