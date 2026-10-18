/* check.h - PART 3 (OS side): the consistency checker (like fsck) */
#ifndef CHECK_H
#define CHECK_H

#include "volume.h"

/* Looks for leaked or missing bitmap entries, shared blocks, bad pointers, size mismatches,
 * orphaned entries and unfinished log rows. With fix = 1 it repairs what can be repaired safely
 * (log rows and bitmap) and checks again. remaining receives the number of problems left. */
int volume_check(Volume *v, int fix, FILE *out, int *remaining);

#endif
