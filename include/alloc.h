/**
 * @file allocate_bsfs.h
 * @brief Block allocation interface for the BSFS file system.
 *
 * This header defines the interface for block allocation and deallocation
 * operations used in the BSFS file system. It provides functions to allocate
 * individual blocks and block spans (`bspan_t`), as well as their
 * corresponding deallocation routines.
 *
 * All functions ensure synchronization with the block bitmap on disk,
 * preserving the consistency of allocation metadata across the file system. The
 * allocation logic supports both sequential and sparse data layouts, which can
 * be used in conjunction with B-tree indexing or file write operations.
 *
 * Functions declared here operate on structures defined in:
 * - bitmap_operations_bsfs.h (bitmap handling)
 * - filesystem_bsfs.h (superblock and constants)
 * - bspan_queue_bsfs.h (queue of allocation spans)
 */
#ifndef ALLOCATE_H
#define ALLOCATE_H

#include "../bsfs.h"
#include "bitmap.h"
#include "bspan_queue.h"

// Upper bound on the metadata reserve, in blocks. Sized for a handful of full
// root-to-leaf copy-on-write paths, which is all a truncate or an unlink needs
// to make progress on a filesystem that is otherwise full.
#define BSFS_META_RESERVE 64

// How many recovery snapshots one reclaim pass may drop. Bounded so a single
// write does not pay for purging the whole trash.
#define BSFS_PURGE_BATCH 32

/**
 * @brief Allocates a sequence of free blocks and fills a bspan queue.
 *
 * This function scans the block bitmap on disk to find available (free) blocks
 * and groups them into spans (bspan_t), which are enqueued into a queue.
 * It uses a first-fit strategy and continues recursively if the entire
 * allocation cannot be fulfilled in a single span.
 *
 * The function internally loads the bitmap, updates it, and writes the updated
 * bitmap back to disk after allocation.
 *
 * @param disk Pointer to the FILE representing the filesystem image.
 * @param sb Pointer to the superblock containing block and bitmap information.
 * @param queue Pointer to the bspan queue where allocated spans will be
 * enqueued.
 * @param blocks_needed Total number of blocks required for allocation.
 * @param offset Starting block index for the allocation scan (typically 0).
 * @return 0 on success, or -1 if there are not enough free blocks.
 */

int allocate_bspan_queue(struct super_block *sb, bspan_queue_t *queue, __u32 blocks_needed, __u32 offset);

/**
 * @brief Allocates a single free block and updates the block bitmap.
 *
 * This function scans the block bitmap to find the first available (free)
 * block. Once found, it marks the block as occupied in the in-memory bitmap and
 * persists the updated bitmap back to disk to ensure consistency.
 *
 * The bitmap must be previously formatted as a linked list of blocks starting
 * from the block number indicated by the superblock. This function assumes
 * that each block in the bitmap contains a pointer to the next block.
 *
 * @param disk Pointer to the FILE representing the filesystem image.
 * @param sb Pointer to the superblock structure containing metadata about the
 * filesystem.
 * @return The index of the allocated block on success, or INVALID_BLOCK if no
 * free blocks are available or if an error occurs during bitmap saving.
 */

__u32 allocate_single_block(struct super_block *sb);

/**
 * @brief Allocates a block for tree metadata, spending the reserve if needed.
 *
 * Copy-on-write has to allocate before it can release, so the paths that shrink
 * a tree must not be the ones that hit ENOSPC first. Every clone and node split
 * allocates through here; file data goes through allocate_single_block().
 */
__u32 allocate_meta_block(struct super_block *sb, __u8 prio);

/**
 * @brief Counts free blocks and sizes the metadata reserve. Call once at mount,
 *        after the bitmap is loaded.
 */
void bsfs_init_block_counts(struct super_block *sb);

/**
 * @brief Deallocates a single block in the filesystem.
 *
 * This function marks the specified block as free in the block allocation
 * bitmap and updates the bitmap on disk. It should be used whenever a block is
 * no longer needed (e.g., during file truncation or deletion).
 *
 * @param disk Pointer to the FILE representing the filesystem image.
 * @param sb Pointer to the superblock with bitmap location information.
 * @param block_index Index of the block to be deallocated.
 * @return 0 on success, or -1 if the block index is invalid or bitmap write
 * fails.
 */

int deallocate_single_block(struct super_block *sb, __u32 block_index);

/**
 * @brief Frees a contiguous range of disk blocks described by a bspan and
 * updates the bitmap on disk.
 *
 * This function loads the block allocation bitmap from disk, marks all blocks
 * covered by the specified `bspan` as free, and writes the updated bitmap back
 * to the disk. It is used to reclaim storage space when a file or segment is
 * removed from the filesystem.
 *
 * The bitmap is read and written based on the starting block specified in the
 * superblock. Any failure to read or write the bitmap will be reported as an
 * error.
 *
 * @param disk Pointer to the open FILE representing the filesystem image.
 * @param sb Pointer to the superblock structure containing bitmap metadata.
 * @param bspan Pointer to the bspan structure indicating which disk blocks to
 * free.
 * @return 0 on success, -1 on failure to read or write the bitmap.
 */
int deallocate_bspan(struct super_block *sb, const bspan_t *bspan);

/**
 * @brief Queues a block released by copy-on-write instead of freeing it now.
 *
 * A block replaced by a copy-on-write mutation must not be reused while the old
 * tree can still be reached from disk: until the new root is durable, a crash
 * leaves the on-disk tree pointing at it. Deferred blocks become free again at
 * the next commit point.
 *
 * @return 0 if the block was queued, -ENOMEM if it had to be leaked.
 */
int bsfs_defer_free_block(struct super_block *sb, __u32 block);

/**
 * @brief Queues every block of a span, as bsfs_defer_free_block() does.
 *
 * Data blocks need the same barrier as metadata: until the inode carrying the
 * new extent root is on disk, the old tree still reaches them.
 */
int bsfs_defer_free_bspan(struct super_block *sb, const bspan_t *bspan);

/**
 * @brief Flushes metadata and releases every deferred block.
 *
 * Runs the barrier even when nothing is queued, so callers can use it to
 * publish the roots. Called from ->sync_fs, ->put_super and
 * bsfs_reclaim_space(). Returns 0, or a negative error if the barrier failed -
 * in which case the blocks stay queued.
 */
int bsfs_commit_deferred_frees(struct super_block *sb);

/**
 * @brief Writes back dirty inodes, then commits the deferred frees.
 *
 * The one way to get blocks back after an ENOSPC. Must be called with no
 * filesystem locks held, from a VFS entry point that can retry.
 */
int bsfs_reclaim_space(struct super_block *sb);

/**
 * @brief True when a mutation should commit the deferred list before starting.
 *
 * Two counter reads. Copy-on-write allocates before it releases, so an entry
 * point that waits for ENOSPC to happen has already lost.
 */
bool bsfs_should_reclaim(struct super_block *sb);

#endif
