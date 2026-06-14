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
    .read_iter    = generic_file_read_iter,
    .write_iter   = generic_file_write_iter,
    .llseek       = generic_file_llseek,
    .mmap         = generic_file_mmap,
    .splice_read  = filemap_splice_read,
    .splice_write = iter_file_splice_write,
    .fsync        = noop_fsync,
};

const struct inode_operations bsfs_file_inode_operations = {

};

const struct inode_operations bsfs_symlink_inode_operations = {

};
