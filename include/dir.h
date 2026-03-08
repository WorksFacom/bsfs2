/**
 * @file directory_operations_bsfs.h
 * @brief High-level operations for directory management in the BSFS file
 * system.
 *
 * This header defines functions to create, delete, rename, and move directories
 * and files by manipulating their directory entries and associated inodes.
 */

#ifndef DIRECTORY_OPERATIONS_BSFS_H
#define DIRECTORY_OPERATIONS_BSFS_H

#include "dentry.h"
#include "fs.h"
#include "inode.h"
#include "path_utils.h"

/**
 * @brief Creates a new directory at the given path.
 *
 * This function allocates a new inode of type directory and inserts it as an
 * entry into the parent directory determined by the path.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the loaded superblock.
 * @param path Absolute or relative path for the new directory.
 * @param cwd Current working directory (for resolving relative paths).
 * @return 0 on success, -1 on error.
 */
int create_directory(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd);

/**
 * @brief Deletes a directory at the given path, if it is empty.
 *
 * This function removes the directory entry and deallocates the associated
 * inode.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the loaded superblock.
 * @param path Absolute or relative path of the directory to remove.
 * @param cwd Current working directory (for resolving relative paths).
 * @return 0 on success, -1 if the directory is not empty or cannot be removed.
 */
int remove_directory(FILE *disk, superblock_t *sb, const char *path, dir_t *cwd);

/**
 * @brief Moves or renames a directory entry from one directory to another.
 *
 * This function removes an existing directory entry from the source directory
 * and inserts it into the destination directory. The entry can also be renamed
 * in the process.
 *
 * @param disk Pointer to the disk file.
 * @param sb Pointer to the superblock.
 * @param src_dir Pointer to the source directory.
 * @param src_name Name of the entry to move (existing name).
 * @param dest_dir Pointer to the destination directory.
 * @param dest_name New name for the entry in the destination (can be same as
 * src_name).
 * @return 0 on success, -1 on failure.
 */
int move_dir_entry(FILE *disk, superblock_t *sb, dir_t *src_dir, const char *src_name, dir_t *dest_dir, const char *dest_name);

/**
 * @brief Initialize and persist the root directory ("/") on a fresh image.
 *
 * Creates the root inode with default ownership and permissions, allocates
 * a directory B-tree for entries, inserts the mandatory "." and ".." entries,
 * and ensures the inode tree contains the root inode.
 *
 * @param disk Open filesystem image.
 * @param sb   Loaded superblock (root inode and tree roots may be updated).
 * @return 0 on success; -1 on error.
 */
int create_root_directory(FILE *disk, superblock_t *sb);

#endif // DIRECTORY_OPERATIONS_BSFS_H
