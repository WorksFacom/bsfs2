/**
 * @file allocate_bsfs.c
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#include "../../include/allocate_bsfs.h"

/**
 * @brief Allocates a sequence of blocks and fills a bspan queue.
 *
 * Uses a first-fit strategy to find free blocks in the bitmap. If a full span
 * cannot be allocated in one run, the function recursively continues allocation
 * until all requested blocks are covered.
 *
 * @param bitmap In-memory bitmap representing block allocation.
 * @param sb Pointer to the superblock containing total block count.
 * @param queue Queue to store the allocated block spans (bspan).
 * @param blocks_needed Number of blocks to allocate.
 * @param offset Starting point for the current scan (typically 0).
 * @return 0 on success, -1 if there are not enough free blocks.
 */
int allocate_bspan_queue(uint8_t *bitmap, superblock_t *sb,
                         bspan_queue_t *queue, uint32_t blocks_needed,
                         uint32_t offset) {
  if (blocks_needed == 0 || offset >= sb->total_blocks) {
    return (blocks_needed == 0) ? 0 : -1;
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
        return 0;
      }
    } else if (run_length > 0) {
      for (uint32_t j = run_start; j < run_start + run_length; j++) {
        set_bit(bitmap, j);
      }
      enqueue_bspan(queue, 0, run_start, run_length);
      return allocate_bspan_queue(bitmap, sb, queue, blocks_needed - run_length,
                                  i + 1);
    }
  }

  // Final run if we reach the end and still have a partial span
  if (run_length > 0) {
    for (uint32_t j = run_start; j < run_start + run_length; j++) {
      set_bit(bitmap, j);
    }
    enqueue_bspan(queue, 0, run_start, run_length);
    return allocate_bspan_queue(bitmap, sb, queue, blocks_needed - run_length,
                                sb->total_blocks);
  }

  return -1;
}

/**
 * @brief Allocates a single free block.
 *
 * Scans the bitmap and allocates the first free block found by setting its bit.
 *
 * @param bitmap Pointer to the bitmap array representing block allocation
 * status.
 * @param total_blocks Total number of blocks in the bitmap.
 * @return Index of the allocated block, or INVALID_BLOCK if none are available.
 */
uint32_t allocate_single_block(FILE *disk, superblock_t *sb) {
  uint32_t total_blocks = sb->total_blocks;
  uint8_t *bitmap = load_bitmap(disk, sb->block_bitmap_start);
  for (uint32_t i = 0; i < total_blocks; i++) {
    if (is_bit_free(bitmap, i)) {
      set_bit(bitmap, i);
      return i;
    }
  }

  fprintf(stderr, "Error: No blocks available.\n");
  return INVALID_BLOCK;
}
