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
 * @return Always returns 0.
 */
int pwd(FILE *disk, superblock_t *sb, char **args) {
    printf("%s\n", current_path);
    return 0;
}