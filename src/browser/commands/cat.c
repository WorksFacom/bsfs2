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

  char buffer[1024];
  size_t bytes_read;

  while ((bytes_read = read_bsfs(disk, sb, &file, buffer, sizeof(buffer) - 1)) >
         0) {
    buffer[bytes_read] = '\0'; // Null-terminate for printing
    printf("%s", buffer);
  }

  close_bsfs(&file);
  return 1;
}
