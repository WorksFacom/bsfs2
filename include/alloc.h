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

#include "bitmap.h"
#include "bspan_queue.h"
#include "bsfs.h"

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

int allocate_bspan_queue(FILE *disk, bsfs_super_block *sb, bspan_queue_t *queue,
                         uint32_t blocks_needed, uint32_t offset);

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

uint32_t allocate_single_block(FILE *disk, bsfs_super_block *sb);

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

int deallocate_single_block(FILE *disk, bsfs_super_block *sb, uint32_t block_index);

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
int deallocate_bspan(FILE *disk, bsfs_super_block *sb, const bspan_t *bspan);

#endif
