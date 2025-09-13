/**
 * @file pwd.c
 * @brief Implements the pwd command for the BSFS filesystem.
 *
 * Displays the current working directory.
 */
#include "../../include/api_bsfs.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char current_path[PATH_MAX];

/**
 * @brief Prints the current working directory path.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Unused parameter.
 * @return Always returns 1 to continue the browser loop.
 */
int pwd(FILE *disk, superblock_t *sb, char **args) {
  (void)args;

  // Try to get the path from the API first
  char api_path[PATH_MAX];
  if (getcwd_bsfs(disk, sb, api_path, sizeof(api_path)) == 0) {
    printf("%s\n", api_path);
  } else {
    // Fallback to the global current_path
    printf("%s\n", current_path);
  }
  return 1;
}
