/**
 * @file superblock_operations_bsfs.h
 * @brief Functions for reading and writing the superblock in the BSFS file
 * system.
 *
 * The superblock is always located at block 0 and contains metadata about
 * the layout and configuration of the BSFS instance.
 */
#ifndef SUPERBLOCK_OPERATIONS_BSFS_H
#define SUPERBLOCK_OPERATIONS_BSFS_H

#include "filesystem_bsfs.h"

/**
 * @brief Writes the superblock to block 0 of the filesystem.
 *
 * @param disk Pointer to the open filesystem image (disk).
 * @param sb Pointer to the superblock structure to write.
 * @return 0 on success, -1 on failure.
 */
int write_superblock(FILE *disk, const superblock_t *sb);

/**
 * @brief Reads the superblock from block 0 of the filesystem.
 *
 * @param disk Pointer to the open filesystem image (disk).
 * @param sb Pointer to the superblock structure to populate.
 * @return 0 on success, -1 on failure.
 */
int read_superblock(FILE *disk, superblock_t *sb);

#endif // SUPERBLOCK_OPERATIONS_BSFS_H
