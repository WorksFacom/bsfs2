/**
 * @file permissions_bsfs.h
 * @brief Permission helper routines for BSFS (POSIX-like rwx bits).
 *
 * Provides centralized checks to enforce discretionary access control (DAC)
 * using classic rwx bits on inodes. These helpers should be called from
 * path traversal and API entry points (mkdir/rmdir/create/remove/move/open,
 * etc.) rather than from low-level I/O primitives.
 */

#ifndef PERMISSIONS_BSFS_H
#define PERMISSIONS_BSFS_H

#include <stdint.h>

#include "fs.h"

/* POSIX-style permission bits (octal) */
#define S_IXOTH 0001  /**< Execute/search permission for others. */
#define S_IWOTH 0002  /**< Write permission for others. */
#define S_IROTH 0004  /**< Read permission for others. */
#define S_IXGRP 0010  /**< Execute/search permission for group. */
#define S_IWGRP 0020  /**< Write permission for group. */
#define S_IRGRP 0040  /**< Read permission for group. */
#define S_IXUSR 0100  /**< Execute/search permission for owner. */
#define S_IWUSR 0200  /**< Write permission for owner. */
#define S_IRUSR 0400  /**< Read permission for owner. */
#define S_ISVTX 01000 /**< Sticky bit (restricts unlink/rename in dir). */
#define S_ISGID 02000 /**< Set-GID bit. */
#define S_ISUID 04000 /**< Set-UID bit. */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute the effective rwx mode (3 bits) for a caller on an inode.
 * @param ino Target inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 3-bit mask (r=4, w=2, x=1) for the caller class; root returns 7.
 */
uint16_t bsfs_effective_mode(const inode_t *ino, uint16_t uid, uint16_t gid);

/** Root check */
static inline int bsfs_is_root(uint16_t uid) {
  return uid == 0;
}

/**
 * @brief Check if caller can read a regular file.
 * @param file Target inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 1 if allowed; 0 otherwise.
 */
int bsfs_can_read_file(const inode_t *file, uint16_t uid, uint16_t gid);
/**
 * @brief Check if caller can write to a regular file.
 * @param file Target inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 1 if allowed; 0 otherwise.
 */
int bsfs_can_write_file(const inode_t *file, uint16_t uid, uint16_t gid);
/**
 * @brief Check if caller can execute a regular file.
 * @param file Target inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 1 if allowed; 0 otherwise.
 */
int bsfs_can_exec_file(const inode_t *file, uint16_t uid, uint16_t gid);

/**
 * @brief Check if caller can traverse (search) a directory.
 * @param dir Directory inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 1 if allowed; 0 otherwise.
 */
int bsfs_can_traverse_dir(const inode_t *dir, uint16_t uid, uint16_t gid);
/**
 * @brief Check if caller can list entries in a directory.
 * @param dir Directory inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 1 if allowed; 0 otherwise.
 */
int bsfs_can_list_dir(const inode_t *dir, uint16_t uid, uint16_t gid);
/**
 * @brief Check if caller can modify entries within a directory (w+x).
 * @param dir Directory inode.
 * @param uid Caller user id.
 * @param gid Caller group id.
 * @return 1 if allowed; 0 otherwise.
 */
int bsfs_can_modify_dir(const inode_t *dir, uint16_t uid, uint16_t gid);

/**
 * @brief Check unlink/rename permission in a sticky directory.
 *
 * If the directory has sticky bit set (S_ISVTX), only root, the directory
 * owner, or the entry owner may remove/rename entries inside it.
 *
 * @param dir   Parent directory inode.
 * @param entry Entry inode (file or subdirectory) to remove/rename.
 * @param uid   Caller user id.
 * @param gid   Caller group id (unused here).
 * @return 1 if allowed, 0 otherwise.
 */
int bsfs_can_unlink_in_sticky(const inode_t *dir, const inode_t *entry, uint16_t uid, uint16_t gid);

/** Apply umask to a mode when creating inode or directory. */
static inline uint16_t bsfs_apply_umask(uint16_t mode, uint16_t umask) {
  return (uint16_t)(mode & ~umask);
}

#ifdef __cplusplus
}
#endif

#endif /* PERMISSIONS_BSFS_H */
