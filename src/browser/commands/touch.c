/**
 * @file touch.c
 * @brief Implements the touch command for the BSFS filesystem.
 *
 * Creates empty files in the filesystem.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Create an empty regular file.
 *
 * Usage: touch &lt;filename&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Target filename in args[1].
 * @return 1 to continue the REPL.
 */
int touch(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: touch <filename>\n");
    return 1;
  }

  int result = create_file_bsfs(disk, sb, args[1]);
  if (result != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error creating file: %s\n", args[1]);
    return 1;
  }

  printf("File created: %s\n", args[1]);
  return 1;
}
