#include <errno.h>
/**
 * @file mv.c
 * @brief Implements the mv command for the BSFS filesystem.
 *
 * Moves or renames files and directories within the filesystem.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Moves or renames a file/directory from source to destination in the
 * BSFS filesystem.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects source at args[1] and
 * destination at args[2].
 * @return Returns 1 to continue browser loop on success or failure.
 */
int mv(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1] || !args[2]) {
    printf("Usage: mv <source> <destination>\n");
    return 1;
  }

    if (move_bsfs(disk, sb, args[1], args[2]) != 0) {
        if (errno == EACCES) printf("Permission denied\n");
        else printf("Error moving/renaming: %s -> %s\n", args[1], args[2]);
        return 1;
    }

  printf("Moved/renamed: %s -> %s\n", args[1], args[2]);
  return 1;
}
