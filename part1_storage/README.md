# Part 1 - Storage

Owner: Manoj. Files: `disk.h/.c` (OS), `space.h/.c` (DBMS). Test: `mingw32-make part1`.

## OS half - `disk.c`

The image is an array of 1024-byte blocks.

```
block 0            superblock: "HYBRIDFS", format 2, block size, total blocks, bitmap blocks, first data block, CRC-32
blocks 1 .. B      allocation bitmap, 1 bit per block
blocks B+1 .. end  data
```

- `disk_create` makes the image, validates the size (1-2000 MB; above that a byte offset no longer fits in a
  32-bit `fseek`), writes the superblock and marks the metadata blocks as used.
- `disk_open` checks the magic, the checksum, the format and that the file is as long as it claims.
- `disk_alloc_block` scans the bitmap from where the last allocation ended and wraps around. `disk_free_block`
  lowers the next search position so freed space is reused.
- Writes to blocks 0..B are rejected, so a bug elsewhere cannot overwrite the superblock or bitmap.
- Bitmap size grows with the disk (100 MB needs 13 bitmap blocks). The earlier version kept the bitmap in one block and
  could address only 8 MB.

## DBMS half - `space.c`

- View `used_blocks(file_id, block_no)` lists every block pointer in `files` (a UNION over `block0..block11`).
- View `block_usage(block_no, owners)` counts owners per block; anything above 1 is corruption.
- `space_compare` / `space_reconcile`: the metadata is the truth about who owns which block. Reconcile rebuilds the
  in-memory bitmap from `used_blocks` and writes it back if it differed. It reports *leaked* blocks (marked used,
  owned by nobody) and *dangling* ones (owned, but marked free).
- `space_report` prints sizes and free space (the `info` command).

## Why the bitmap is rebuildable

The bitmap is written on every allocation, but it is only a cache. After a crash it may be wrong in either
direction, so recovery (part 3) never trusts it; it rebuilds it from the database, which is transactional.

## Things worth being able to explain

- Why next-fit allocation, and what fragmentation does here (nothing visible: every block is the same size).
- Why a superblock checksum matters (detecting a damaged image instead of misreading it).
- What happens if the bitmap says "free" for a block a file still owns (two files could get the same block) and how
  `space_reconcile` prevents it.
