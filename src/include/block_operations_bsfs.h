/**
 * @file block_operations_bsfs.h
 * @brief Low-level block read/write operations for the BSFS file system.
 *
 * This header defines the interface for manipulating raw disk blocks
 * in the BSFS file system. It provides low-level functions for reading
 * from and writing to specific blocks on disk, as well as a utility function
 * for freeing blocks based on the bitmap.
 *
 * These operations are fundamental for interacting with file system
 * structures stored on disk, such as inodes, B-tree nodes, and user data.
 *
 * Functions declared here are intended to be used by higher-level components
 * such as allocation logic, metadata management, and file read/write APIs.
 *
 * The block operations are aligned with the block size defined in
 * the superblock and rely on bitmap status to maintain consistency.
 */
#ifndef BLOCK_OP_H
#define BLOCK_OP_H

#include "bitmap_operations_bsfs.h"
#include "filesystem_bsfs.h"

/**
 * @brief Writes data to a single block.
 *
 * This function writes a block of data to the specified block number on
 * the disk. It ensures the size of the data does not exceed the block size.
 * The data is written to the block with a full write of the block size.
 *
 * @param disk Pointer to the disk file where data will be written.
 * @param block_number The block number where the data will be written.
 * @param data Pointer to the data to be written.
 * @param size The size of the data to be written.
 * @return 0 on success, -1 if there is an error in the writing process.
 */
int write_block(FILE *disk, uint32_t block_number, const void *data,
                size_t size);

/**
 * @brief Reads data from a single block.
 *
 * This function reads a block of data from the specified block number on
 * the disk. It ensures the size of the data to be read does not exceed the
 * block size.
 *
 * @param disk Pointer to the disk file where data will be read from.
 * @param block_number The block number from which the data will be read.
 * @param data Pointer to the buffer where the read data will be stored.
 * @param size The size of the data to be read.
 * @return 0 on success, -1 if there is an error in the reading process.
 */
int read_block(FILE *disk, uint32_t block_number, void *data, size_t size);

/**
 * @brief Marks a block as free in the file system.
 *
 * Clears the bit corresponding to @p block_number in the allocation bitmap and
 * writes the updated bitmap to disk. The bitmap region location is obtained
 * from the superblock (sb->block_bitmap_start).
 *
 * @param disk Pointer to the disk file where the block is located.
 * @param sb   Pointer to the superblock providing bitmap location metadata.
 * @param block_number The block number to be marked as free.
 * @param bitmap Pointer to the in-memory bitmap buffer to update and write.
 * @param bitmap_size Unused parameter (reserved for future validation).
 * @return 0 on success, -1 on failure.
 */
int free_block(FILE *disk, superblock_t *sb, uint32_t block_number,
               uint8_t *bitmap, uint32_t bitmap_size);
#endif // !BLOCK_OP_H
