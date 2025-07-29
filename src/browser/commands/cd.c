/**
 * @file cd.c
 * @brief Implements the cd command for the BSFS filesystem.
 *
 * Changes the current working directory.
 */
#include <stdio.h>
#include <string.h>
#include "filesystem_bsfs.h"
#include "api_bsfs.h"

extern char current_path[PATH_MAX];

/**
 * @brief Changes the current directory.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the directory path at args[1].
 * @return Returns 1 on success to continue browser loop, -1 if directory not found.
 */
int cd(FILE *disk, superblock_t *sb, char **args) {
    if (!args[1]) {
        printf("Usage: cd <directory>\n");
        return 1;
    }

    dir_t dir;
    if (opendir_bsfs(disk, sb, args[1], &dir) != 0) {
        printf("Directory not found: %s\n", args[1]);
        return 1;
    }
    closedir_bsfs(&dir);

    strncpy(current_path, args[1], PATH_MAX-1);
    current_path[PATH_MAX-1] = '\0';

    return 1;
}