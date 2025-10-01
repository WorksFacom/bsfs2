/**
 * @file mv.c
 * @brief Implements the mv command for the BSFS filesystem.
 *
 * Moves or renames files and directories within the filesystem.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Move or rename a file or directory.
 *
 * Usage: mv &lt;src&gt; &lt;dst&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Source path in args[1], destination path in args[2].
 * @return 1 to continue the REPL.
 */
int mv(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1] || !args[2]) {
    printf("Usage: mv <source> <destination>\n");
    return 1;
  }

  if (move_bsfs(disk, sb, args[1], args[2]) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error moving/renaming: %s -> %s\n", args[1], args[2]);
    return 1;
  }

  printf("Moved/renamed: %s -> %s\n", args[1], args[2]);
  return 1;
}
