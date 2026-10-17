/* space.h - PART 1 (DBMS side): views over block ownership, and keeping the bitmap in step with them.
 *
 * The files table is the source of truth for which blocks belong to whom.
 * The bitmap on the image is only a cache of that, so it can always be rebuilt.
 */
#ifndef SPACE_H
#define SPACE_H

#include "volume.h"

int space_schema_create(sqlite3 *db);       /* views used_blocks and block_usage */

/* Compare the bitmap with the metadata without changing anything. */
int space_compare(Volume *v, int *leaked, int *dangling);

/* Rebuild the bitmap from the metadata. Counts how many entries had to change. */
int space_reconcile(Volume *v, int *leaked, int *dangling);

int space_report(Volume *v, FILE *out);

#endif
