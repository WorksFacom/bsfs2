#include <errno.h>
/**
 * @file list.c
 * @brief Implements the list command for the BSFS filesystem.
 *
 * Lists contents of a directory.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Lists the entries within a directory.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, optionally contains the directory path
 * at args[1].
 * @return Returns 1 on success to continue browser loop, 1 if directory cannot
 * be opened.
 */
int list(FILE *disk, superblock_t *sb, char **args) {
  dir_t dir;
  dir_entry_t entry;
  const char *path = args[1] ? args[1] : ".";

    if (opendir_bsfs(disk, sb, path, &dir) != 0) {
        if (errno == EACCES) printf("Permission denied\n");
        else printf("Error opening directory: %s\n", path);
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
