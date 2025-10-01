/**
 * @file rmdir.c
 * @brief Implements the rmdir command for the BSFS filesystem.
 *
 * Provides functionality to remove directories within the BSFS filesystem.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Remove an empty directory.
 *
 * Usage: rmdir &lt;directory&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Directory name in args[1].
 * @return 1 to continue the REPL.
 */
int rmdir(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: rmdir <directory>\n");
    return 1;
  }
  if (rmdir_bsfs(disk, sb, args[1]) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error removing directory (must be empty): %s\n", args[1]);
    return 1;
  }
  printf("Directory removed: %s\n", args[1]);
  return 1;
}
