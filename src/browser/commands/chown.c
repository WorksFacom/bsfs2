/**
 * @file chown.c
 * @brief Implements chown command for BSFS.
 */
#include <stdio.h>
#include <stdlib.h>
#include "api_bsfs.h"

int chown(FILE *disk, superblock_t *sb, char **args) {
  if (!args[1] || !args[2]) {
    printf("Usage: chown <uid> <path>\n");
    return 1;
  }
  char *e=NULL; unsigned long uid = strtoul(args[1], &e, 10);
  if (e==args[1] || *e!='\0' || uid>65535) {
    printf("chown: invalid uid '%s'\n", args[1]);
    return 1;
  }
  if (chown_bsfs(disk, sb, args[2], (uint16_t)uid) != 0) {
    printf("chown: failed on '%s'\n", args[2]);
  }
  return 1;
}

