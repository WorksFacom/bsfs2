#ifndef COMMANDS_H
#include "filesystem_bsfs.h"
#include <stdio.h>

typedef struct {
  char *name;
  int (*func)(FILE *file, superblock_t *sb, char **args);
} Command;

int bsfs_cmd_exit(FILE *file, superblock_t *sb, char **args);
int bsfs_cmd_info(FILE *file, superblock_t *sb, char **args);

#endif
