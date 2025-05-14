/**
 * @file bitmap_operations_bsfs.h
 * @brief Header file containing functions for bitmap manipulation in the BSFS
 * file system.
 *
 * This file provides functions for loading, saving, and manipulating the bitmap
 * which is used for managing free and allocated blocks in the filesystem.
 */
#ifndef BITMAP_OPERATIONS
#define BITMAP_OPERATIONS
#include "filesystem_bsfs.h"
#include <stdio.h>
#include <stdlib.h>

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
int init_bitmap_list(FILE *disk, uint32_t start_block, uint32_t num_blocks);

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
uint8_t *load_bitmap(FILE *disk, uint32_t bitmap_start_block);

/**
 * @brief Saves the bitmap to the specified disk.
 *
 * This function writes the bitmap to the disk at the specified position,
 * ensuring that the bitmap is correctly stored in the filesystem.
 *
 * @param disk The file pointer to the filesystem image.
 * @param bitmap The pointer to the bitmap to be saved.
 * @param bitmap_start_block The block where the bitmap starts.
 * @param bitmap_total_blocks The total number of blocks in the bitmap.
 * @return 0 on success, or -1 if saving the bitmap fails.
 */
int write_bitmap(FILE *file, uint8_t *bitmap, uint32_t bitmap_start_block);

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
uint32_t is_bit_free(uint8_t *bitmap, uint32_t index);

/**
 * @brief Marks a block as allocated in the bitmap.
 *
 * This function sets the bit corresponding to a given block to indicate that it
 * is now allocated (occupied).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as allocated.
 */
void set_bit(uint8_t *bitmap, uint32_t index);

/**
 * @brief Marks a block as free in the bitmap.
 *
 * This function clears the bit corresponding to a given block to indicate that
 * it is now free (available for allocation).
 *
 * @param bitmap The pointer to the bitmap.
 * @param index The index of the block to mark as free.
 */
void clear_bit(uint8_t *bitmap, uint32_t index);

#endif // BITMAP_OPERATIONS
