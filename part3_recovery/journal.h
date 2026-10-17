/* journal.h - PART 3 (DBMS side): the write-ahead log table and crash recovery
 *
 * Before a file's new data touches the image, a copy of every new block is stored in
 * wal_log with committed = 0. The metadata update and "committed = 1" happen together
 * in one database transaction, so a log row is only ever marked committed if the
 * file really points at the new blocks.
 */
#ifndef JOURNAL_H
#define JOURNAL_H

#include "volume.h"

int journal_schema_create(sqlite3 *db);

int journal_begin(Volume *v);
int journal_commit(Volume *v);
void journal_rollback(Volume *v);

/* Stores one new block image as an unfinished log row. */
int journal_log_block(Volume *v, int file_id, int block_no, const unsigned char *image, long *log_id);

/* Marks rows as committed. Call inside the same transaction that updates the file. */
int journal_mark_committed(Volume *v, const long *log_ids, int count);

/* Throws away log rows that never reached "committed". */
int journal_discard_unfinished(Volume *v, int file_id);

/* Removes older committed rows of a file; keep only rows with id >= first_kept (0 = delete them all). */
int journal_trim(Volume *v, int file_id, long first_kept);

/* Undo unfinished work, rebuild the bitmap, redo committed blocks that are missing on the image.
 * Fills v->undone_rows, v->redone_blocks and v->bitmap_fixes. */
int journal_recover(Volume *v);

#endif
