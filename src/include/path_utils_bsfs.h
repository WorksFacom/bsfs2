/**
 * @file path_utils_bsfs.h
 * @brief Utility functions for resolving paths in the BSFS file system.
 *
 * This module provides helper functions for resolving absolute or relative
 * file system paths, locating parent directories, and retrieving inodes from
 * path strings.
 */

#ifndef PATH_UTILS_BSFS_H
#define PATH_UTILS_BSFS_H

#include "dir_entry_operations_bsfs.h"
#include "filesystem_bsfs.h"
#include "inode_operations_bsfs.h"

/**
 * @brief Resolves a path to its parent directory and the final component name.
 *
 * This function traverses the file system path starting from either the root
 * (for absolute paths) or the given current working directory (cwd), and
 * returns:
 * - The parent directory (dir_t) that contains the final component.
 * - The name of the final component (file or directory name).
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param path Input path string (absolute or relative).
 * @param cwd Pointer to current working directory (used for relative paths).
 * @param parent_out Output parameter to receive the parent directory.
 * @param final_name Output parameter to receive the final name.
 * @return 0 on success, -1 on error (invalid path or not found).
 */
int resolve_path(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd,
                 dir_t *parent_out, char *final_name);

/**
 * @brief Looks up a file or directory by path and loads its inode.
 *
 * This function resolves the given path and attempts to locate the
 * corresponding inode (for either a file or directory). It internally uses
 * `resolve_path` and `lookup_dir_entry`.
 *
 * @param disk Pointer to the filesystem image.
 * @param sb Pointer to the superblock.
 * @param path Input path string (absolute or relative).
 * @param cwd Pointer to current working directory (used for relative paths).
 * @param out_inode Output parameter to receive the found inode.
 * @return 0 on success, -1 if the file or directory is not found.
 */
int lookup_file(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd,
                inode_t *out_inode);

#endif // PATH_UTILS_BSFS_H
