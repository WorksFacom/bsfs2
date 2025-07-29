/**
 * @file mkdir.c
 * @brief Implements the mkdir command for the BSFS filesystem.
 *
 * Provides functionality to create directories within the BSFS filesystem.
 */
#include <stdio.h>
#include <string.h>
#include "filesystem_bsfs.h"
#include "api_bsfs.h"

/**
 * @brief Creates a new directory in the BSFS filesystem.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the directory name at args[1].
 * @return Returns 1 to continue browser loop on success or failure.
 */
int mkdir(FILE *disk, superblock_t *sb, char **args) {
    if (!args[1]) {
        printf("Usage: mkdir <directory>\n");
        return 1;
    }
    if (mkdir_bsfs(disk, sb, args[1]) != 0) {
        printf("Error creating directory: %s\n", args[1]);
        return 1;
    }
    printf("Directory created: %s\n", args[1]);
    return 1;
}