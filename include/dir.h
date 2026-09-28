/**
 * @file dir.h
 * @brief VFS operations for directories.
 *
 * lookup, create, mkdir, mknod, unlink, rmdir, rename and readdir. link and
 * symlink are not wired up yet. Every mutating entry point reclaims first when
 * bsfs_should_reclaim() says so, since each one rewrites at least two trees.
 * mknod, create and mkdir also retry once after bsfs_reclaim_space() on
 * -ENOSPC.
 */

#ifndef DIRECTORY_OPERATIONS_BSFS_H
#define DIRECTORY_OPERATIONS_BSFS_H

#include "../bsfs.h"
#include "dirent.h"

/** @brief readdir, llseek and fsync for directories. */
extern const struct file_operations bsfs_dir_operations;

/** @brief Namespace operations for directories. */
extern const struct inode_operations bsfs_dir_inode_operations;

/**
 * @brief Resolves @p dentry's name in its parent.
 *
 * @return the inode number, or 0 if the name is missing or the lookup failed.
 *         Inode 0 is never allocated, so 0 is safe as "none".
 */
ino_t bsfs_inode_by_name(struct dentry *dentry);

#endif // DIRECTORY_OPERATIONS_BSFS_H
