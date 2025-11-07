/**
 * @file cat.c
 * @brief Implements the cat command for the BSFS filesystem.
 *
 * Displays the contents of files.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Display file contents.
 *
 * Usage: cat &lt;path&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args File path in args[1].
 * @return 1 to continue the REPL.
 */
int cat(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1]) {
    printf("Usage: cat <filename>\n");
    return 1;
  }

  file_t file;
  if (open_bsfs(disk, sb, args[1], &file) != 0) {
    printf("Error opening file: %s\n", args[1]);
    return 1;
  }

  unsigned char buffer[4096];
  size_t bytes_read;

  while ((bytes_read = read_bsfs(disk, sb, &file, buffer, sizeof(buffer))) >
         0) {
    if (fwrite(buffer, 1, bytes_read, stdout) != bytes_read) {
      printf("\ncat: write error to stdout\n");
      break;
    }
  }

  close_bsfs(&file);
  return 1;
}
