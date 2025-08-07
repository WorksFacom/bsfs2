#ifndef COMMANDS_H
#define COMMANDS_H

#include "filesystem_bsfs.h"
#include <stdio.h>

typedef struct {
  char *name;
  int (*func)(FILE *file, superblock_t *sb, char **args);
} Command;

int bsfs_cmd_exit(FILE *file, superblock_t *sb, char **args);
int bsfs_cmd_info(FILE *file, superblock_t *sb, char **args);
int bsfs_cmd_help(FILE *file, superblock_t *sb, char **args);

int cd(FILE *disk, superblock_t *sb, char **args);
int pwd(FILE *disk, superblock_t *sb, char **args);
int list(FILE *disk, superblock_t *sb, char **args);

int mkdir(FILE *disk, superblock_t *sb, char **args);
int rmdir(FILE *disk, superblock_t *sb, char **args);

int touch(FILE *disk, superblock_t *sb, char **args);
int rm(FILE *disk, superblock_t *sb, char **args);
int cp(FILE *disk, superblock_t *sb, char **args);
int mv(FILE *disk, superblock_t *sb, char **args);
int cat(FILE *disk, superblock_t *sb, char **args);
int echo(FILE *disk, superblock_t *sb, char **args);
int stat_cmd(FILE *disk, superblock_t *sb, char **args);

#endif