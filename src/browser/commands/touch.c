/**
 * @file touch.c
 * @brief Implements the touch command for the BSFS filesystem.
 *
 * Creates empty files in the filesystem.
 */
#include <stdio.h>
#include <string.h>
#include "filesystem_bsfs.h"
#include "api_bsfs.h"

/**
 * @brief Creates a new empty file in the BSFS filesystem.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the filename at args[1].
 * @return Returns 1 to continue browser loop on success or failure.
 */
int touch(FILE *disk, superblock_t *sb, char **args) {
    if (!args[1]) {
        printf("Usage: touch <filename>\n");
        return 1;
    }
    
    if (create_file_bsfs(disk, sb, args[1]) != 0) {
        printf("Error creating file: %s\n", args[1]);
        return 1;
    }
    
    printf("File created: %s\n", args[1]);
    return 1;
}
