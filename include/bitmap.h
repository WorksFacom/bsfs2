/**
 * @file bitmap_operations_bsfs.h
 * @brief Bitmap management utilities for the BSFS file system.
 *
 * This header provides the core functions to manage the block allocation bitmap
 * in the Basic Solution File System (BSFS). The bitmap tracks which blocks on
 * the disk are allocated or free, and is essential for operations such as:
 *
 * - Block allocation and deallocation
 * - File creation and deletion
 * - Space management and consistency
 *
 * The bitmap is stored on disk as a linked list of fixed-size blocks and is
 * loaded into memory for manipulation during runtime. These functions abstract
 * the bitmap I/O and provide low-level access to set, clear, and query bit
 * status.
 *
 * ## Key functionalities:
 * - Initialization of the bitmap block list on disk
 * - Loading and saving the bitmap from/to disk
 * - Marking individual blocks as free or occupied
 * - Querying the state of specific blocks
 *
 * This interface is used internally by the allocator and file system logic
 * to ensure efficient space tracking and integrity of data block usage.
 */
#ifndef BITMAP_OPERATIONS
#define BITMAP_OPERATIONS

#include "../bsfs.h"
#include <linux/fs.h>
#include <linux/types.h>

/**
 * @brief Initializes a linked list of bitmap blocks on disk.
 *
 * This function creates and writes a series of bitmap blocks to the disk,
 * starting at a specified block number. Each bitmap block contains a byte array
 * representing the bitmap data (used to track free/allocated blocks) and a
 * pointer to the next bitmap block. The last block in the list will have its
 * `next_block` field set to `INVALID_BLOCK`, indicating the end of the list.
 *
 * @param disk Pointer to the file representing the disk (filesystem image).
 * @param start_block The starting block number where the bitmap list begins.
 * @param num_blocks The total number of blocks to be initialized in the list.
 *
 * @return 0 on success, or -1 if an error occurs during disk write operations.
 */
int init_bitmap_list(struct super_block *sb, __u32 start_block, __u32 num_blocks);

/**
 * @brief Loads a bitmap from the specified file.
 *
 * This function reads the bitmap from the disk, starting at the given position
 * and using the block size to determine the number of bytes to read.
 *
 * @param disk The file pointer to the filesystem image.
 * @param bitmap_start_block The block where the bitmap starts.
 * @return A pointer to the loaded bitmap in memory, or NULL if loading fails.
 */
void load_bitmap(struct super_block *sb);

/**
 * @brief Writes the bitmap to disk starting at the given block.
 *
 * This function traverses the linked list of bitmap blocks on disk and writes
 * the corresponding segments of the bitmap array to each block. It ensures the
 * updated bitmap is persisted for future allocations.
 *
 * @param disk File pointer to the filesystem image.
 * @param bitmap Pointer to the in-memory bitmap data.
 * @param bitmap_start_block Logical block number where the bitmap begins.
 * @return 0 on success, or -1 if an error occurs during reading or writing.
 */
int write_bitmap(struct super_block *sb, __u8 *bitmap, __u32 bitmap_start_block);

/**
 * @brief Checks if a given block is free in the bitmap.
 *
 * This function checks the status of a specific block in the bitmap to
 * determine if it is free (available for allocation) or occupied.
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to check.
 * @return 1 if the block is free, 0 if the block is occupied.
 */
__u32 bsfs_is_bit_free(__u8 *bitmap, __u32 index);

/**
 * @brief Marks a block as allocated in the bitmap.
 *
 * This function sets the bit corresponding to a given block to indicate that it
 * is now allocated (occupied).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as allocated.
 */
void bsfs_set_bit(__u8 *bitmap, __u32 index);

/**
 * @brief Marks a block as free in the bitmap.
 *
 * This function clears the bit corresponding to a given block to indicate that
 * it is now free (available for allocation).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as free.
 */
void bsfs_clear_bit(__u8 *bitmap, __u32 index);

#endif // BITMAP_OPERATIONS
