/**
 * @file allocate_bsfs.c
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#include "../../include/allocate_bsfs.h"

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
int allocate_bspan_queue(FILE *disk, superblock_t *sb, bspan_queue_t *queue,
                         uint32_t blocks_needed, uint32_t offset) {
  if (blocks_needed == 0 || offset >= sb->total_blocks) {
    return (blocks_needed == 0) ? 0 : -1;
  }

  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap) {
    fprintf(stderr, "Failed to load bitmap from disk.\n");
    return -1;
  }

  uint32_t run_start = INVALID_BLOCK;
  uint32_t run_length = 0;

  for (uint32_t i = offset; i < sb->total_blocks; i++) {
    if (is_bit_free(bitmap, i)) {
      if (run_start == INVALID_BLOCK) {
        run_start = i;
        run_length = 1;
      } else {
        run_length++;
      }

      if (run_length == blocks_needed) {
        for (uint32_t j = run_start; j < run_start + run_length; j++) {
          set_bit(bitmap, j);
        }
        enqueue_bspan(queue, 0, run_start, run_length);
        write_bitmap(disk, bitmap, sb->block_bitmap_start);
        free(bitmap);
        return 0;
      }
    } else if (run_length > 0) {
      for (uint32_t j = run_start; j < run_start + run_length; j++) {
        set_bit(bitmap, j);
      }
      enqueue_bspan(queue, 0, run_start, run_length);
      int result = allocate_bspan_queue(disk, sb, queue,
                                        blocks_needed - run_length, i + 1);
      write_bitmap(disk, bitmap, sb->block_bitmap_start);
      free(bitmap);
      return result;
    }
  }

  // Final run if reached end of bitmap with a partial span
  if (run_length > 0) {
    for (uint32_t j = run_start; j < run_start + run_length; j++) {
      set_bit(bitmap, j);
    }
    enqueue_bspan(queue, 0, run_start, run_length);
    int result = allocate_bspan_queue(
        disk, sb, queue, blocks_needed - run_length, sb->total_blocks);
    write_bitmap(disk, bitmap, sb->block_bitmap_start);
    free(bitmap);
    return result;
  }

  free(bitmap);
  return -1;
}

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
uint32_t allocate_single_block(FILE *disk, superblock_t *sb) {
  uint32_t total_blocks = sb->total_blocks;
  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap)
    return INVALID_BLOCK;

  for (uint32_t i = 0; i < total_blocks; i++) {
    if (is_bit_free(bitmap, i)) {
      set_bit(bitmap, i);

      // ✅ Persist the updated bitmap to disk
      if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
        fprintf(stderr, "Error: Failed to save updated bitmap.\n");
        free(bitmap);
        return INVALID_BLOCK;
      }

      free(bitmap);
      return i;
    }
  }

  fprintf(stderr, "Error: No blocks available.\n");
  free(bitmap);
  return INVALID_BLOCK;
}

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
int deallocate_single_block(FILE *disk, superblock_t *sb,
                            uint32_t block_index) {
  if (block_index >= sb->total_blocks) {
    fprintf(stderr, "Invalid block index for deallocation: %u\n", block_index);
    return -1;
  }

  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap) {
    fprintf(stderr, "Failed to load bitmap from disk.\n");
    return -1;
  }

  clear_bit(bitmap, block_index);

  if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
    fprintf(stderr, "Failed to write updated bitmap to disk.\n");
    free(bitmap);
    return -1;
  }

  free(bitmap);
  return 0;
}

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
int deallocate_bspan(FILE *disk, superblock_t *sb, const bspan_t *bspan) {
  // Load bitmap from disk
  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  if (!bitmap) {
    fprintf(stderr, "Error: failed to load block bitmap.\n");
    return -1;
  }

  // Clear bits corresponding to the span
  for (uint32_t i = 0; i < bspan->length; i++) {
    clear_bit(bitmap, bspan->disk_blk + i);
  }

  // Persist updated bitmap to disk
  if (write_bitmap(disk, bitmap, sb->block_bitmap_start) != 0) {
    fprintf(stderr, "Error: failed to write block bitmap to disk.\n");
    free(bitmap);
    return -1;
  }

  free(bitmap);
  return 0;
}
