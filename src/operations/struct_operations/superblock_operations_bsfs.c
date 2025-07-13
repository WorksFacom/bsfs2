/**
 * @file superblock_operations_bsfs.c
 * @brief Implementation of superblock manipulation functions in the BSFS file
 * system.
 *
 * This file contains routines to read and write the superblock, which is
 * located at block 0 of the filesystem and stores essential metadata required
 * to mount and navigate the file system.
 */

#include "../../include/superblock_operations_bsfs.h"

/**
 * @brief Writes the superblock to block 0 of the filesystem.
 *
 * @param disk Pointer to the open filesystem image (disk).
 * @param sb Pointer to the superblock structure to write.
 * @return 0 on success, -1 on failure.
 */
int write_superblock(FILE *disk, const superblock_t *sb) {
  if (fseek(disk, 0, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek to superblock position.\n");
    return -1;
  }

  if (fwrite(sb, sizeof(superblock_t), 1, disk) != 1) {
    fprintf(stderr, "Failed to write superblock.\n");
    return -1;
  }

  fflush(disk);
  return 0;
}

/**
 * @brief Reads the superblock from block 0 of the filesystem.
 *
 * @param disk Pointer to the open filesystem image (disk).
 * @param sb Pointer to the superblock structure to populate.
 * @return 0 on success, -1 on failure.
 */
int read_superblock(FILE *disk, superblock_t *sb) {
  if (fseek(disk, 0, SEEK_SET) != 0) {
    fprintf(stderr, "Failed to seek to superblock position.\n");
    return -1;
  }

  if (fread(sb, sizeof(superblock_t), 1, disk) != 1) {
    fprintf(stderr, "Failed to read superblock.\n");
    return -1;
  }

  return 0;
}
