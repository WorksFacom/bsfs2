/**
 * @file permissions_bsfs.c
 * @brief Implementation of POSIX-like permission checks for BSFS.
 */

#include "../../include/permissions_bsfs.h"

uint16_t bsfs_effective_mode(const inode_t *ino, uint16_t uid, uint16_t gid) {
  if (!ino)
    return 0;
  if (uid == 0)
    return 07; /* root: full */
  /* owner */
  if (uid == ino->uid)
    return (uint16_t)((ino->permissions >> 6) & 07);
  /* group */
  if (gid == ino->gid)
    return (uint16_t)((ino->permissions >> 3) & 07);
  /* others */
  return (uint16_t)(ino->permissions & 07);
}

int bsfs_can_read_file(const inode_t *file, uint16_t uid, uint16_t gid) {
  return (bsfs_effective_mode(file, uid, gid) & 04) != 0;
}

int bsfs_can_write_file(const inode_t *file, uint16_t uid, uint16_t gid) {
  return (bsfs_effective_mode(file, uid, gid) & 02) != 0;
}

int bsfs_can_exec_file(const inode_t *file, uint16_t uid, uint16_t gid) {
  return (bsfs_effective_mode(file, uid, gid) & 01) != 0;
}

int bsfs_can_traverse_dir(const inode_t *dir, uint16_t uid, uint16_t gid) {
  /* 'x' on directory means search/traverse */
  return (bsfs_effective_mode(dir, uid, gid) & 01) != 0;
}

int bsfs_can_list_dir(const inode_t *dir, uint16_t uid, uint16_t gid) {
  /* 'r' on directory means list */
  return (bsfs_effective_mode(dir, uid, gid) & 04) != 0;
}

int bsfs_can_modify_dir(const inode_t *dir, uint16_t uid, uint16_t gid) {
  /* To modify entries within a directory: need w+x on the directory */
  uint16_t mode = bsfs_effective_mode(dir, uid, gid);
  return (mode & 03) == 03;
}

int bsfs_can_unlink_in_sticky(const inode_t *dir, const inode_t *entry,
                              uint16_t uid, uint16_t gid) {
  (void)gid;
  if (!dir || !entry)
    return 0;
  /* If sticky bit is not set, permit based on directory modify perms only. */
  if ((dir->permissions & S_ISVTX) == 0)
    return 1;
  /* Sticky set: only root, directory owner, or entry owner */
  if (uid == 0)
    return 1;
  if (uid == dir->uid)
    return 1;
  if (uid == entry->uid)
    return 1;
  return 0;
}

