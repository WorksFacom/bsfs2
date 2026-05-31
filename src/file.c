/**
 * @file file_operations_bsfs.c
 * @brief Implementation of file-level operations in the BSFS file system.
 *
 * This file provides functions to manipulate files, including opening,
 * closing, reading, writing, and seeking, by utilizing inodes and
 * B-tree-based address mapping. Files are loaded into memory as `file_t`
 * structures which manage stateful access.
 */
#include "../include/file.h"

const struct file_operations bsfs_file_operations = {

};

const struct inode_operations bsfs_file_inode_operations = {

};

const struct inode_operations bsfs_symlink_inode_operations = {

};
