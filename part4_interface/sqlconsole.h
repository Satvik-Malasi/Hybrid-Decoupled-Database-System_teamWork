/* sqlconsole.h - PART 4 (DBMS side): run SQL against the metadata, with guard rails
 *
 * Allowed:  SELECT anything; INSERT / UPDATE / DELETE on files and file_tags.
 * Refused:  changing the structure (CREATE, DROP, ALTER, PRAGMA, ATTACH ...),
 *           touching wal_log, and editing the columns the file system itself owns
 *           (id, mode, size, block0..block11). Several statements in one go are refused.
 * After a change the block bitmap is brought back in line with the metadata, so
 * deleting rows really frees their space.
 */
#ifndef SQLCONSOLE_H
#define SQLCONSOLE_H

#include "volume.h"

int sql_console_run(Volume *v, const char *sql, FILE *out);

#endif
