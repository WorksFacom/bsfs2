/**
 * @file whoami.c
 * @brief Implements whoami command to show uid/gid and umask.
 */
#include <stdio.h>
#include "api_bsfs.h"

int whoami_cmd(FILE *disk, superblock_t *sb, char **args) {
  (void)disk; (void)sb; (void)args;
  uint16_t uid=0,gid=0,um=0;
  get_identity_bsfs(&uid,&gid);
  get_umask_bsfs(&um);
  printf("uid=%u gid=%u umask=%03o\n", (unsigned)uid, (unsigned)gid, (unsigned)um);
  return 1;
}

