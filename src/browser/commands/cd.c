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
 * @brief Updates the current_path string based on the target path.
 * @param target_path The path to navigate to.
 */
static void update_current_path(const char *target_path) {
    if (target_path[0] == '/') {
        // Absolute path
        strncpy(current_path, target_path, PATH_MAX - 1);
    } else if (strcmp(target_path, ".") == 0) {
        // Current directory - no change
        return;
    } else if (strcmp(target_path, "..") == 0) {
        // Parent directory
        char *last_slash = strrchr(current_path, '/');
        if (last_slash && last_slash != current_path) {
            *last_slash = '\0';
        } else {
            strcpy(current_path, "/");
        }
    } else {
        // Relative path
        if (strlen(current_path) > 1) {
            strncat(current_path, "/", PATH_MAX - strlen(current_path) - 1);
        }
        strncat(current_path, target_path, PATH_MAX - strlen(current_path) - 1);
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

    dir_t dir;
    if (opendir_bsfs(disk, sb, args[1], &dir) != 0) {
        printf("Directory not found: %s\n", args[1]);
        return 1;
    }
    closedir_bsfs(&dir);

    update_current_path(args[1]);
    return 1;
}