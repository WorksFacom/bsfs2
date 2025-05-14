/**
 * @file allocate_bsfs.h
 * @brief Functions for allocating blocks in the BSFS file system.
 */
#ifndef ALLOCATE_H
#define ALLOCATE_H

#include "bitmap_operations_bsfs.h"
#include "bspan_queue_bsfs.h"
#include "filesystem_bsfs.h"

/**
 * @brief Allocates a sequence of blocks and fills a bspan queue.
 *
 * This function uses the first-fit algorithm to find a sequence of free blocks
 * in the bitmap. It fills the provided bspan queue with one or more spans
 * covering the allocated blocks. If the exact number of blocks cannot be found
 * in a contiguous run, it recursively continues to allocate what remains.
 *
 * @param bitmap Pointer to the bitmap array representing block allocation
 * status.
 * @param queue Pointer to the bspan queue to fill with allocated spans.
 * @param blocks_needed Number of blocks to allocate.
 * @param total_blocks Total number of blocks in the bitmap.
 * @return 0 on success, -1 if insufficient free blocks are available.
 */
int allocate_bspan_queue(uint8_t *bitmap, superblock_t *sb,
                         bspan_queue_t *queue, uint32_t blocks_needed,
                         uint32_t offset);

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
uint32_t allocate_single_block(FILE *disk, superblock_t *sb);

#endif
