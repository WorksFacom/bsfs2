/**
 * @file mkdir.c
 * @brief Implements the mkdir command for the BSFS filesystem.
 *
 * Provides functionality to create directories within the BSFS filesystem.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Create a new directory.
 *
 * Usage: mkdir &lt;directory&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Directory name in args[1].
 * @return 1 to continue the REPL.
 */
int mkdir(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: mkdir <directory>\n");
    return 1;
  }
  if (mkdir_bsfs(disk, sb, args[1]) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error creating directory: %s\n", args[1]);
    return 1;
  }
  printf("Directory created: %s\n", args[1]);
  return 1;
}
