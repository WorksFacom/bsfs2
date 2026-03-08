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

#include "dentry.h"
#include "fs.h"
#include "inode.h"

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
int resolve_path(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd, dir_t *parent_out, char *final_name);

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
int lookup_file(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd, inode_t *out_inode);

/**
 * @brief Resolve the parent directory and basename from a path.
 *
 * This function takes an absolute or relative path, normalizes it,
 * validates it, splits it into parent directory path and basename,
 * and resolves the parent directory into a @ref dir_t structure.
 *
 * Workflow:
 *  1. Normalize the input path using normalize_path().
 *  2. Reject paths ending with '/' (files must have a basename).
 *  3. Split the normalized path into parent and basename using
 *     split_parent_basename().
 *  4. Resolve the parent path into a directory inode using
 *     lookup_file().
 *  5. Verify that the resolved parent is a directory.
 *  6. Return the parent directory in @p out_parent and the basename
 *     in @p out_basename.
 *
 * Error handling:
 *  - Returns -1 if arguments are invalid.
 *  - Returns -1 if normalization or splitting fails.
 *  - Returns -1 if the parent path cannot be resolved or is not a directory.
 *  - Returns 0 on success.
 *
 * Memory:
 *  - Internally allocates a temporary normalized path; this is freed before
 *    return.
 *
 * @param disk          Pointer to the open filesystem image.
 * @param sb            Pointer to the loaded superblock.
 * @param path          Absolute or relative path of the file.
 * @param cwd           Current working directory for relative path resolution.
 * @param out_parent    Output: resolved parent directory (as @ref dir_t).
 * @param out_basename  Output: buffer that will receive the final component
 *                      (basename) of the path.
 * @param basename_cap  Capacity of @p out_basename buffer (in bytes).
 *
 * @return 0 on success; -1 on error.
 */
int resolve_parent_dir(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd, dir_t *out_parent, char *out_basename,
                       size_t basename_cap);

#endif // PATH_UTILS_BSFS_H
