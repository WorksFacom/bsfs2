/**
 * @file cp.c
 * @brief Implements the cp command for the BSFS filesystem.
 *
 * Copies files within the filesystem.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Copy a file.
 *
 * Usage: cp &lt;src&gt; &lt;dst&gt;
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Source path in args[1], destination path in args[2].
 * @return 1 to continue the REPL.
 */
int cp(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1] || !args[2]) {
    printf("Usage: cp <source> <destination>\n");
    return 1;
  }

  file_t source_file;
  if (open_bsfs(disk, sb, args[1], &source_file) != 0) {
    printf("Error opening source file: %s\n", args[1]);
    return 1;
  }

  if (create_file_bsfs(disk, sb, args[2]) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error creating destination file: %s\n", args[2]);
    close_bsfs(&source_file);
    return 1;
  }

  file_t dest_file;
  if (open_bsfs(disk, sb, args[2], &dest_file) != 0) {
    printf("Error opening destination file: %s\n", args[2]);
    close_bsfs(&source_file);
    return 1;
  }

  // Copy data in chunks
  char buffer[1024];
  size_t bytes_read;

  while ((bytes_read =
              read_bsfs(disk, sb, &source_file, buffer, sizeof(buffer))) > 0) {
    size_t wrote = write_bsfs(disk, sb, &dest_file, buffer, bytes_read);
    if (wrote != bytes_read) {
      if (errno == EACCES)
        printf("Permission denied\n");
      else
        printf("Error writing to destination file: %s\n", args[2]);
      close_bsfs(&source_file);
      close_bsfs(&dest_file);
      return 1;
    }
  }

  close_bsfs(&source_file);
  close_bsfs(&dest_file);

  printf("File copied: %s -> %s\n", args[1], args[2]);
  return 1;
}
