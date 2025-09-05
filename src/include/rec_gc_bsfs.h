/**
 * @file rec_gc_bsfs.h
 * @brief Recovery garbage collection (GC) routines for BSFS.
 *
 * Provides on-demand purge functions that reclaim disk space occupied by
 * recovery entries (snapshots of deleted files). These are intended to be
 * invoked from allocation paths when the filesystem runs out of free blocks.
 */
#ifndef REC_GC_BSFS_H
#define REC_GC_BSFS_H

#include "btree_bsfs.h"
#include "filesystem_bsfs.h"
#include <stdint.h>
#include <stdio.h>

/**
 * @brief Purge recovery entries until at least @p needed_blocks are freed.
 *
 * Attempts to reclaim space by removing expired recovery entries first
 * (retention_until <= @p now). If insufficient, continues purging the oldest
 * entries (ascending deleted_at) until @p needed_blocks have been freed or no
 * entries remain.
 *
 * For each purged entry, data blocks referenced in direct spans and in the
 * bspan B-tree (if any) are deallocated, and the recovery entry is removed
 * from the Recovery B-tree.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (used for bitmap updates and recovery root).
 * @param needed_blocks Minimum number of blocks to free.
 * @param now  Current timestamp for retention checks.
 * @return Number of blocks actually freed (can be less than requested).
 */
size_t rec_gc_purge_until(FILE *disk, superblock_t *sb, uint32_t needed_blocks,
                          uint64_t now);

#endif /* REC_GC_BSFS_H */

