/**
 * @file list.c
 * @brief Implements the list command for the BSFS filesystem.
 *
 * Lists contents of a directory.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief List Recovery snapshots, optionally filtered by original name.
 *
 * Usage: recovery [name]
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Optional name filter in args[1].
 * @return 1 to continue the REPL.
 */
int list(FILE *disk, superblock_t *sb, char **args) {
  dir_t dir;
  dir_entry_t entry;
  const char *path = args[1] ? args[1] : ".";

  if (opendir_bsfs(disk, sb, path, &dir) != 0) {
    if (errno == EACCES)
      printf("Permission denied\n");
    else
      printf("Error opening directory: %s\n", path);
    return 1;
  }

  printf("Listing directory: %s\n", path);
  printf("%-24s %-6s\n", "Name", "Type");
  printf("%-24s %-6s\n", "----", "----");

  while (readdir_bsfs(disk, sb, &dir, &entry) == 1) {
    const char *type = (entry.file_type == FILE_TYPE_DIR) ? "DIR" : "FILE";
    printf("%-24s %-6s\n", entry.filename, type);
  }

  closedir_bsfs(&dir);
  return 1;
}
