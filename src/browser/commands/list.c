/**
 * @file list.c
 * @brief Implements the list command for the BSFS filesystem.
 *
 * Lists contents of a directory.
 */
#include <stdio.h>
#include "filesystem_bsfs.h"
#include "api_bsfs.h"

/**
 * @brief Lists the entries within a directory.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, optionally contains the directory path at args[1].
 * @return Returns 1 on success to continue browser loop, 1 if directory cannot be opened.
 */
int list(FILE *disk, superblock_t *sb, char **args) {
    dir_t dir;
    dir_entry_t entry;
    const char *path = args[1] ? args[1] : "."; 

    if (opendir_bsfs(disk, sb, path, &dir) != 0) {
        printf("Error opening directory: %s\n", path);
        return 1;
    }

    while (readdir_bsfs(disk, sb, &dir, &entry) == 1) {
        printf("%s\n", entry.name);
    }

    closedir_bsfs(&dir);
    return 1;
}