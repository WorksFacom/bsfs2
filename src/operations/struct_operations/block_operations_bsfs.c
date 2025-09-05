/**
 * @file block_operations_bsfs.c
 * @brief Implementation of low-level block I/O operations for BSFS.
 *
 * This module provides basic functions for reading and writing fixed-size
 * blocks on disk in the BSFS file system. It also includes a utility function
 * to free blocks and mark them as available in the block bitmap.
 *
 * - `write_block()` writes a full block of data to a given block number.
 * - `read_block()` reads a full block of data from a given block number.
 * - `free_block()` clears the content of a block and marks it as free in the
 * bitmap.
 *
 * These functions form the foundation for all I/O operations involving
 * disk-resident structures, including metadata trees, inodes, and data files.
 *
 * All access respects the `BLOCK_SIZE` defined in the superblock
 * and integrates with the bitmap for block availability tracking.
 */
#include "../../include/block_operations_bsfs.h"

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
                size_t size) {
  if (size > BLOCK_SIZE) {
    fprintf(stderr,
            "Error (write block): write size greater than block size.\n");
    return -1;
  }

  if (!disk) {
    fprintf(stderr, "Error (write block): null disk pointer .\n");
    return -1;
  }

  // Calculate the offset
  uint64_t offset = (uint64_t)block_number * BLOCK_SIZE;

  if (fseek(disk, offset, SEEK_SET) != 0) {
    perror("Error (write block): fseek.\n");
    return -1;
  }

  // Prepare the buffer
  uint8_t buffer[BLOCK_SIZE];
  memset(buffer, 0, BLOCK_SIZE);
  memcpy(buffer, data, size);

  // Write the full block
  if (fwrite(buffer, 1, BLOCK_SIZE, disk) != BLOCK_SIZE) {
    perror("Error (write block): fwrite\n");
    return -1;
  }

  return 0; // Success
}

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
int read_block(FILE *disk, uint32_t block_number, void *data, size_t size) {
  if (size > BLOCK_SIZE) {
    fprintf(stderr,
            "Error (read block): reading size greater than block size.\n");
    return -1;
  }

  if (!disk) {
    fprintf(stderr, "Error (read block): null disk pointer.\n");
    return -1;
  }

  // Calculate the offset
  uint64_t offset = (uint64_t)block_number * BLOCK_SIZE;

  if (fseek(disk, offset, SEEK_SET) != 0) {
    perror("Error (read block): fseek.\n");
    return -1;
  }

  // Read the full block
  if (fread(data, 1, size, disk) != size) {
    perror("Error (read block): fread.\n");
    return -1;
  }

  return 0; // Success
}

/**
 * @brief Marks a block as free in the file system.
 *
 * Clears the bit corresponding to @p block_number in the allocation bitmap and
 * writes the updated bitmap to disk. The bitmap region location is obtained
 * from the superblock (sb->block_bitmap_start).
 *
 * @param disk Pointer to the disk file where the block is located.
 * @param sb Pointer to the superblock providing bitmap location metadata.
 * @param block_number The block number to be marked as free.
 * @param bitmap Pointer to the in-memory bitmap buffer to update and write.
 * @param bitmap_size Unused parameter (reserved for future validation).
 * @return 0 on success, -1 on failure.
 */
int free_block(FILE *disk, superblock_t *sb, uint32_t block_number,
               uint8_t *bitmap, uint32_t bitmap_size) {
  if (!disk || !bitmap) {
    fprintf(stderr, "Error (free block): null disk pointer or bitmap.\n");
    return -1;
  }

  // Mark the block as free in the bitmap
  clear_bit(bitmap, block_number);

  // Save the updated bitmap back to the disk
  if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
    return -1;
  }

  return 0; // Success
}
