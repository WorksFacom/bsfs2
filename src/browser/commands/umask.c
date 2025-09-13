/**
 * @file umask.c
 * @brief Implements umask command to show/set umask in octal.
 */
#include <stdio.h>
#include <stdlib.h>
#include "api_bsfs.h"

int umask_cmd(FILE *disk, superblock_t *sb, char **args) {
  (void)disk; (void)sb;
  if (!args[1]) {
    uint16_t um=0; get_umask_bsfs(&um);
    printf("umask %03o\n", (unsigned)um);
    return 1;
  }
  char *e=NULL; unsigned long val = strtoul(args[1], &e, 8);
  if (e==args[1] || *e!='\0' || val>0777) {
    printf("umask: invalid value (octal 000..777)\n");
    return 1;
  }
  set_umask_bsfs((uint16_t)val);
  printf("umask set to %03o\n", (unsigned)val);
  return 1;
}

