/**
 * @file whoami.c
 * @brief Implements whoami command to show uid/gid and umask.
 */
#include "../../include/api_bsfs.h"

/**
 * @brief Print current session uid, gid and umask.
 *
 * Reads the API session identity and umask and prints them in a compact form.
 *
 * @param disk Open filesystem image (unused).
 * @param sb   Loaded superblock (unused).
 * @param args argv-like vector (unused).
 * @return 1 to continue the browser loop.
 */
int whoami_cmd(FILE *disk, superblock_t *sb, char **args) {
  (void)disk;
  (void)sb;
  (void)args;
  uint16_t uid = 0, gid = 0, um = 0;
  get_identity_bsfs(&uid, &gid);
  get_umask_bsfs(&um);
  printf("uid=%u gid=%u umask=%03o\n", (unsigned)uid, (unsigned)gid,
         (unsigned)um);
  return 1;
}
