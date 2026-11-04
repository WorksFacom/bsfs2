/**
 * @file alloc.h
 * @brief Block allocation, the metadata reserve and deferred frees.
 *
 * The bitmap decides what is allocated. Which free block gets handed out next
 * is up to the strategy compiled in (see alloc_strategy.h). This layer keeps
 * sbi->free_blocks and the metadata reserve honest, and holds back blocks that
 * copy-on-write released until the new tree is on disk.
 *
 * Everything here takes sbi->bitmap_lock or sbi->pending_free_lock itself.
 */
#ifndef ALLOCATE_H
#define ALLOCATE_H

#include "../bsfs.h"
#include "bitmap.h"

/// Upper bound on the metadata reserve, in blocks. Sized for a handful of full
/// root-to-leaf copy-on-write paths, which is all a truncate or an unlink needs
/// to make progress on a filesystem that is otherwise full.
#define BSFS_META_RESERVE 64

/// How many recovery snapshots one reclaim pass may drop. Bounded so a single
/// write does not pay for purging the whole trash.
#define BSFS_PURGE_BATCH 32

/**
 * @brief Allocates one block for file data.
 *
 * Stops short of the metadata reserve, so data can fill the disk without
 * leaving truncate and unlink stuck. Never reclaims on its own. Callers that get
 * INVALID_BLOCK back return -ENOSPC, and the VFS entry point retries after
 * bsfs_reclaim_space().
 *
 * @return the block index, or INVALID_BLOCK if nothing is free above the reserve.
 */
__u32 allocate_single_block(struct super_block *sb);

/**
 * @brief Allocates a block for tree metadata, spending the reserve if needed.
 *
 * Copy-on-write has to allocate before it can release, so the paths that shrink
 * a tree must not be the ones that hit ENOSPC first. Every clone and node split
 * allocates through here. File data goes through allocate_single_block().
 *
 * @param prio a bsfs_alloc_prio_t, usually the handle's. BSFS_ALLOC_GROW leaves
 *             the whole reserve alone, BSFS_ALLOC_WRITEBACK may take half of it,
 *             BSFS_ALLOC_SHRINK may take all of it.
 * @return the block index, or INVALID_BLOCK.
 */
__u32 allocate_meta_block(struct super_block *sb, __u8 prio);

/**
 * @brief Counts free blocks and sizes the metadata reserve.
 *
 * Call once at mount, after load_bitmap(). Counts from data_block_start, and
 * sets the reserve to 1/64 of the data area, capped at BSFS_META_RESERVE.
 */
void bsfs_init_block_counts(struct super_block *sb);

/**
 * @brief Frees a block right now, skipping the deferred list.
 *
 * Only safe for blocks no durable tree can reach, like a clone that never got
 * published. Anything a copy-on-write mutation replaced goes through
 * bsfs_defer_free_block() instead. Freeing a block that is already free is a
 * no-op, which the rollback paths rely on.
 *
 * @return 0, or -1 if @p block_index is past the end of the filesystem.
 */
int deallocate_single_block(struct super_block *sb, __u32 block_index);

/**
 * @brief Queues a block released by copy-on-write instead of freeing it now.
 *
 * A block replaced by a copy-on-write mutation must not be reused while the old
 * tree can still be reached from disk. Until the new root is durable, a crash
 * leaves the on-disk tree pointing at it. Deferred blocks become free again at
 * the next commit point. Blocks 0 and INVALID_BLOCK are ignored.
 *
 * @return 0 if the block was queued, -ENOMEM if it had to be leaked.
 */
int bsfs_defer_free_block(struct super_block *sb, __u32 block);

/**
 * @brief Queues every block of a span, as bsfs_defer_free_block() does.
 *
 * Data blocks need the same barrier as metadata. Until the inode carrying the
 * new extent root is on disk, the old tree still reaches them.
 *
 * @return 0, or the first error. Blocks that could not be queued are leaked.
 */
int bsfs_defer_free_bspan(struct super_block *sb, const bspan_t *bspan);

/**
 * @brief Flushes metadata and releases every deferred block.
 *
 * Syncs the block device and the superblock, issues a cache flush, and checks
 * sbi->writeback_error. Only then do the queued blocks go back to the bitmap.
 * Runs the barrier even when nothing is queued, so callers can use it to
 * publish the roots. Called from ->sync_fs and ->put_super.
 * bsfs_reclaim_space() runs the same barrier after writing back inodes.
 *
 * @return 0, or a negative error if the barrier failed. The blocks stay queued
 *         in that case.
 */
int bsfs_commit_deferred_frees(struct super_block *sb);

/**
 * @brief Writes back dirty inodes, then commits the deferred frees.
 *
 * The one way to get blocks back after an ENOSPC. If space is still short after
 * the commit, purges up to BSFS_PURGE_BATCH recovery snapshots and commits
 * again. Must be called with no filesystem locks held, from a VFS entry point
 * that can retry.
 *
 * @return 0, -EBUSY if s_umount was busy, as during an unmount, or the
 *         commit's error.
 */
int bsfs_reclaim_space(struct super_block *sb);

/**
 * @brief True when a mutation should reclaim before starting.
 *
 * Fires once free blocks drop to twice the reserve and there is something to
 * get back, either deferred blocks or a recovery tree. Cheap enough for every
 * entry point. Copy-on-write allocates before it releases, so an entry point
 * that waits for ENOSPC to happen has already lost.
 */
bool bsfs_should_reclaim(struct super_block *sb);

#endif
