/*
 * @file bitmap_operations_bsfs.c
 * @brief C file containing functions for bitmap manipulation in the BSFS
 * file system.
 *
 * This file provides functions for loading, saving, and manipulating the bitmap
 * which is used for managing free and allocated blocks in the filesystem.
 */
#include "../../include/bitmap_operations_bsfs.h"

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
int init_bitmap_list(FILE *disk, uint32_t start_block, uint32_t num_blocks) {
  for (uint32_t i = 0; i < num_blocks; i++) {
    bitmap_block_t block;
    memset(block.bits, 0, BITMAP_BLOCK_BYTES);
    if (i < num_blocks - 1) {
      block.next_block = start_block + i + 1;
    } else {
      block.next_block = INVALID_BLOCK;
    }

    if (fseek(disk, (start_block + i) * BLOCK_SIZE, SEEK_SET) != 0) {
      fprintf(stderr, "Failed to seek while initializing bitmap list.\n");
      return -1;
    }
    if (fwrite(&block, sizeof(bitmap_block_t), 1, disk) != 1) {
      fprintf(stderr, "Failed to write bitmap block.\n");
      return -1;
    }
  }
  fflush(disk);
  return 0;
}

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
uint8_t *load_bitmap(FILE *disk, uint32_t bitmap_start_block) {
  uint8_t *bitmap = NULL;
  size_t total_bytes = 0;

  uint32_t current_block = bitmap_start_block;

  while (current_block != INVALID_BLOCK) {
    bitmap_block_t block;

    if (fseek(disk, current_block * BLOCK_SIZE, SEEK_SET) != 0 ||
        fread(&block, sizeof(bitmap_block_t), 1, disk) != 1) {
      fprintf(stderr, "Failed to read bitmap block.\n");
      free(bitmap);
      return NULL;
    }

    // Grow the in-memory buffer to accommodate the next bitmap chunk
    bitmap = realloc(bitmap, total_bytes + BITMAP_BLOCK_BYTES);
    if (!bitmap) {
      fprintf(stderr, "Failed to realloc bitmap buffer.\n");
      return NULL;
    }

    memcpy(bitmap + total_bytes, block.bits, BITMAP_BLOCK_BYTES);
    total_bytes += BITMAP_BLOCK_BYTES;
    current_block = block.next_block;
  }

  return bitmap;
}

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
int write_bitmap(FILE *disk, uint8_t *bitmap, uint32_t bitmap_start_block) {
  uint32_t current_block = bitmap_start_block;
  size_t offset = 0;

  while (current_block != INVALID_BLOCK) {
    bitmap_block_t block;
    memset(&block, 0, sizeof(block));

    // Preserve next_block pointer from on-disk structure
    if (fseek(disk, current_block * BLOCK_SIZE + BITMAP_BLOCK_BYTES, SEEK_SET) !=
            0 ||
        fread(&block.next_block, sizeof(uint32_t), 1, disk) != 1) {
      fprintf(stderr, "Failed to read next block info.\n");
      return -1;
    }

    // Copy a slice of the bitmap into this block's bits
    size_t to_copy = BITMAP_BLOCK_BYTES;
    memcpy(block.bits, bitmap + offset, to_copy);
    offset += to_copy;

    if (fseek(disk, current_block * BLOCK_SIZE, SEEK_SET) != 0 ||
        fwrite(&block, sizeof(bitmap_block_t), 1, disk) != 1) {
      fprintf(stderr, "Failed to write bitmap block.\n");
      return -1;
    }

    current_block = block.next_block;
  }

  fflush(disk);
  return 0;
}

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
uint32_t is_bit_free(uint8_t *bitmap, uint32_t index) {
  uint32_t byte_index = index / 8;
  uint8_t bit_index = index % 8;

  return !(bitmap[byte_index] & (1 << bit_index));
}

/**
 * @brief Marks a block as allocated in the bitmap.
 *
 * This function sets the bit corresponding to a given block to indicate that it
 * is now allocated (occupied).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as allocated.
 */
void set_bit(uint8_t *bitmap, uint32_t index) {
  uint32_t byte_index = index / 8;
  uint8_t bit_index = index % 8;

  bitmap[byte_index] |= (1 << bit_index);
}

/**
 * @brief Marks a block as free in the bitmap.
 *
 * This function clears the bit corresponding to a given block to indicate that
 * it is now free (available for allocation).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as free.
 */
void clear_bit(uint8_t *bitmap, uint32_t index) {
  uint32_t byte_index = index / 8;
  uint8_t bit_index = index % 8;

  bitmap[byte_index] &= ~(1 << bit_index);
}
