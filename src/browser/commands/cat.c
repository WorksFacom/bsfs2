/**
 * @file cat.c
 * @brief Implements the cat command for the BSFS filesystem.
 *
 * Displays the contents of files.
 */
#include <stdio.h>
#include <string.h>
#include "filesystem_bsfs.h"
#include "api_bsfs.h"

/**
 * @brief Displays the contents of a file in the BSFS filesystem.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the filename at args[1].
 * @return Returns 1 to continue browser loop on success or failure.
 */
int cat(FILE *disk, superblock_t *sb, char **args) {
    if (!args[1]) {
        printf("Usage: cat <filename>\n");
        return 1;
    }
    
    file_t file;
    if (open_bsfs(disk, sb, args[1], &file) != 0) {
        printf("Error opening file: %s\n", args[1]);
        return 1;
    }
    
    char buffer[1024];
    size_t bytes_read;
    
    while ((bytes_read = read_bsfs(disk, sb, &file, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';  // Null-terminate for printing
        printf("%s", buffer);
    }
    
    close_bsfs(&file);
    return 1;
}
