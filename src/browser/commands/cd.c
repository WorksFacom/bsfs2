/**
 * @file cd.c
 * @brief Implements the cd command for the BSFS filesystem.
 *
 * Changes the current working directory.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Change current working directory.
 *
 * Usage: cd &lt;directory&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Expects directory path in args[1].
 * @return 1 to continue the REPL.
 */
int cd(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: cd <directory>\n");
    return 1;
  }

  if (chdir_bsfs(disk, sb, args[1]) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Directory not found: %s\n", args[1]);
    return 1;
  }
  return 1;
}
