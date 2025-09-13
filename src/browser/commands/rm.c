/**
 * @file rm.c
 * @brief Implements the rm command for the BSFS filesystem.
 *
 * Removes files from the filesystem.
 */
#include "../../include/api_bsfs.h"
#include <errno.h>

/**
 * @brief Removes a file from the BSFS filesystem.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the filename at args[1].
 * @return Returns 1 to continue browser loop on success or failure.
 */
int rm(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: rm <filename>\n");
    return 1;
  }

    if (remove_file_bsfs(disk, sb, args[1]) != 0) {
        if (errno == EACCES) printf("Permission denied\n");
        else printf("Error removing file: %s\n", args[1]);
        return 1;
    }

  printf("File removed: %s\n", args[1]);
  return 1;
}
