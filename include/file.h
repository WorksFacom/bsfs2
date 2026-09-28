/**
 * @file file.h
 * @brief VFS operations for regular files and symlinks.
 *
 * Reads, writes and mmap all go through the page cache and the generic
 * helpers. The filesystem-specific part is block mapping (bsfs_get_block()) and
 * truncation (bsfs_truncate()), both in inode.c.
 */

#ifndef FILE_OPERATIONS_BSFS_H
#define FILE_OPERATIONS_BSFS_H

#include "../bsfs.h"

/** @brief Generic page-cache file operations, plus bsfs_fsync(). */
extern const struct file_operations bsfs_file_operations;

/**
 * @brief Makes @p file's data and inode durable.
 *
 * Writes back the range, syncs the inode, syncs the block device, then issues a
 * cache flush. @p datasync is ignored, so the inode is always written. Does not
 * commit deferred frees. Blocks released by earlier mutations stay queued
 * until ->sync_fs or a reclaim.
 *
 * @return 0, or the first error.
 */
int bsfs_fsync(struct file *file, loff_t start, loff_t end, int datasync);

/** @brief setattr for regular files. Any size change runs bsfs_truncate(). */
extern const struct inode_operations bsfs_file_inode_operations;

/** @brief Empty for now. Symlinks can be read from disk but not created. */
extern const struct inode_operations bsfs_symlink_inode_operations;

#endif // FILE_OPERATIONS_BSFS_H
