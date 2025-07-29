/**
 * @file pwd.c
 * @brief Implements the pwd command for the BSFS filesystem.
 *
 * Displays the current working directory.
 */
#include <stdio.h>
#include "filesystem_bsfs.h"

extern char current_path[PATH_MAX];

/**
 * @brief Prints the current working directory path.
 *
 * @param disk Unused parameter.
 * @param sb Unused parameter.
 * @param args Unused parameter.
 * @return Always returns 1 to continue the browser loop.
 */
int pwd(FILE *disk, superblock_t *sb, char **args) {
    (void)disk;
    (void)sb;
    (void)args;
    printf("%s\n", current_path);
    return 1;
}