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
#include "path_utils_bsfs.h"

extern char current_path[PATH_MAX];

/**
 * @brief Updates the current_path string based on the path argument.
 * 
 * @param new_path The path to change to.
 */
static void update_current_path(const char *new_path) {
    if (new_path[0] == '/') {
        // Absolute path
        strncpy(current_path, new_path, PATH_MAX - 1);
    } else {
        // Relative path - handle special cases
        if (strcmp(new_path, ".") == 0) {
            // Stay in current directory
            return;
        } else if (strcmp(new_path, "..") == 0) {
            // Go to parent directory
            char *last_slash = strrchr(current_path, '/');
            if (last_slash && last_slash != current_path) {
                *last_slash = '\0';
            } else {
                strcpy(current_path, "/");
            }
        } else {
            // Regular relative path
            if (strlen(current_path) > 1) {
                strncat(current_path, "/", PATH_MAX - strlen(current_path) - 1);
            }
            strncat(current_path, new_path, PATH_MAX - strlen(current_path) - 1);
        }
    }
    current_path[PATH_MAX - 1] = '\0';
}

/**
 * @brief Changes the current directory.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock structure.
 * @param args Command arguments array, expects the directory path at args[1].
 * @return Returns 1 on success to continue browser loop.
 */
int cd(FILE *disk, superblock_t *sb, char **args) {
    if (!args[1]) {
        printf("Usage: cd <directory>\n");
        return 1;
    }

    // Try to change directory using the API
    if (chdir_bsfs(disk, sb, args[1]) != 0) {
        printf("Directory not found: %s\n", args[1]);
        return 1;
    }
    
    // Update the global current_path for display purposes
    update_current_path(args[1]);
    
    return 1;
}