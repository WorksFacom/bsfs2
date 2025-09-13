/**
 * @file cd.c
 * @brief Implements the cd command for the BSFS filesystem.
 *
 * Changes the current working directory.
 */
#include "../../include/api_bsfs.h"
#include <stdio.h>
#include <errno.h>

/**
 * @brief Changes the current directory.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the directory path at args[1].
 * @return Returns 1 on success to continue browser loop.
 */
int cd(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: cd <directory>\n");
    return 1;
  }

  if (chdir_bsfs(disk, sb, args[1]) != 0) {
    if (errno == EACCES) printf("Permission denied\n");
    else printf("Directory not found: %s\n", args[1]);
    return 1;
  }
  return 1;
}
