/**
 * @file chgrp.c
 * @brief Implements chgrp command for BSFS.
 */
#include <stdio.h>
#include <stdlib.h>
#include "api_bsfs.h"

int chgrp(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1] || !args[2]) {
    printf("Usage: chgrp <gid> <path>\n");
    return 1;
  }
  char *e=NULL; unsigned long gid = strtoul(args[1], &e, 10);
  if (e==args[1] || *e!='\0' || gid>65535) {
    printf("chgrp: invalid gid '%s'\n", args[1]);
    return 1;
  }
  if (chgrp_bsfs(disk, sb, args[2], (uint16_t)gid) != 0) {
    printf("chgrp: failed on '%s'\n", args[2]);
  }
  return 1;
}

