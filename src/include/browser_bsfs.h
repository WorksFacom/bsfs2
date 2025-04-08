#ifndef BROWSER_H
#define BROWSER_H

#include "commands_bsfs.h"
#include <stdio.h>

void read_superblock(FILE *file, superblock_t *sb);
void print_superblock_info(superblock_t *sb);
char *browser_read_line();
char **browser_split_line(char *line);
int browser_execute(FILE *file, superblock_t sb, char **args);
void browser_loop(FILE *file, superblock_t *sb);

#endif // !BROWSER_H
