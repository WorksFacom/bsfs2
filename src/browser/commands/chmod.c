/**
 * @file chmod.c
 * @brief Implements chmod command for BSFS.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Change permission bits of a path.
 *
 * Parses the octal mode and calls the high-level API to update permission
 * bits. Prints usage on invalid input and a concise error on failure.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock.
 * @param args Command argv-like vector; expects mode in args[1], path in
 * args[2].
 * @return 1 to continue the browser loop.
 */
int chmod(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1] || !args[2]) {
    printf("Usage: chmod <mode-octal> <path>\n");
    return 1;
  }
  char *e = NULL;
  unsigned long mode = strtoul(args[1], &e, 8);
  if (e == args[1] || *e != '\0' || mode > 07777) {
    printf("chmod: invalid mode '%s'\n", args[1]);
    return 1;
  }
  if (chmod_bsfs(disk, sb, args[2], (uint16_t)mode) != 0) {
    printf("chmod: failed on '%s'\n", args[2]);
  }
  return 1;
}
