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

#include "../bsfs.h"
#include "dirent.h"

extern const struct file_operations bsfs_dir_operations;

extern const struct inode_operations bsfs_dir_inode_operations;

ino_t bsfs_inode_by_name(struct dentry *dentry);

#endif // DIRECTORY_OPERATIONS_BSFS_H
