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

#include "../bsfs.h"
#include "btree.h"

/**
 * @brief Purge Recovery entries until at least a target number of blocks are freed.
 *
 * Attempts to reclaim space by first removing expired entries (those with
 * retention_until <= now). If that is insufficient, continues by removing the
 * oldest remaining entries (ascending deleted_at) until at least
 * @p needed_blocks have been freed or there are no more entries.
 *
 * For each purged entry, this routine deallocates blocks described by its 16
 * direct spans and, if present, traverses the per-file bspan B-tree to
 * deallocate the remaining spans. Finally, it removes the metadata entry from
 * the Recovery B-tree.
 *
 * @param disk          Open filesystem image.
 * @param sb            Loaded superblock (used for bitmap updates and recovery root).
 * @param needed_blocks Minimum number of blocks to free.
 * @param now           Current timestamp used for retention checks.
 * @return Number of blocks actually freed (can be less than requested).
 */
size_t rec_gc_purge_until(struct super_block *sb, uint32_t needed_blocks, uint64_t now);

#endif /* REC_GC_BSFS_H */
