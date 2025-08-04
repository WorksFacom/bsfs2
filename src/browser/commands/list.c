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

    printf("Listing directory: %s\n", path);
    printf("%-20s %s\n", "Name", "Type");
    printf("%-20s %s\n", "----", "----");

    while (readdir_bsfs(disk, sb, &dir, &entry) == 1) {
        // Get inode information to determine type
        inode_t inode;
        btree_handle_t *inode_handle = btree_create_inode_handle();
        if (inode_handle && read_inode(disk, inode_handle, entry.inode_number, &inode) == 0) {
            const char *type = (inode.file_type == 0) ? "DIR" : "FILE";
            printf("%-20s %s\n", entry.name, type);
            free(inode_handle);
        } else {
            printf("%-20s %s\n", entry.name, "?");
            if (inode_handle) free(inode_handle);
        }
    }

    closedir_bsfs(&dir);
    return 1;
}