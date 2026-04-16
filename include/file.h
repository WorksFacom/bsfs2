/**
 * @file file_operations_bsfs.h
 * @brief Header file for file-level operations in the BSFS file system.
 *
 * This module provides functions for opening, closing, seeking, reading,
 * and writing files using the BSFS file system. File access is performed
 * through a memory-resident `file_t` structure, which maintains a cursor
 * and an associated inode.
 */

#ifndef FILE_OPERATIONS_BSFS_H
#define FILE_OPERATIONS_BSFS_H

#include "../bsfs.h"

extern const struct file_operations bsfs_file_operations;

extern const struct inode_operations bsfs_file_inode_operations;

extern const struct inode_operations bsfs_symlink_inode_operations;

#endif // FILE_OPERATIONS_BSFS_H
