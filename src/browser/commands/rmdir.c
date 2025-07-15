/**
 * @file rmdir.c
 * @brief Implements the rmdir command for the BSFS filesystem.
 *
 * Provides functionality to remove directories within the BSFS filesystem.
 */
#include <stdio.h>
#include <string.h>
#include "filesystem_bsfs.h"
#include "api_bsfs.h"

/**
 * @brief Removes an empty directory from the BSFS filesystem.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the directory name at args[1].
 * @return Returns 0 on success, -1 on failure.
 */
int rmdir(FILE *disk, superblock_t *sb, char **args) {
    if (!args[1]) {
        printf("Usage: rmdir <directory>\n");
        return -1;
    }
    if (rmdir_bsfs(disk, sb, args[1]) != 0) {
        printf("Error removing directory (must be empty): %s\n", args[1]);
        return -1;
    }
    printf("Directory removed: %s\n", args[1]);
    return 0;
}