/**
 * @file remove_file_bsfs.c
 * @brief Implementation of logical file removal in BSFS.
 *
 * Removal here is logical: the file is unlinked from its directory and its
 * inode is removed from the inode tree, but blocks are not freed. A snapshot
 * is inserted into the Recovery tree to allow later restoration or purging.
 */

#include "../../include/remove_file_bsfs.h"

int remove_file(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                btree_handle_t *rec_tree, const char *path, dir_t *cwd,
                uint16_t uid) {
  if (!disk || !sb || !inode_tree || !rec_tree || !path || !cwd)
    return -1;

  /* 1) Resolve parent directory and basename */
  dir_t parent_dir;
  char basename[256] = {0};
  if (resolve_parent_dir(disk, sb, path, cwd, &parent_dir, basename,
                         sizeof(basename)) != 0)
    return -1;

  /* 2) Look up the file inode */
  inode_t target_inode;
  if (lookup_file(disk, sb, path, cwd, &target_inode) != 0)
    return -1;

  if (target_inode.file_type == FILE_TYPE_DIR) {
    /* safety: only remove regular files for now */
    return -1;
  }

  /* Optional: permission checks with uid/gid if you enforce them */
  (void)uid;

  /* 3) Build recovery entry snapshot */
  rec_entry_t rec;
  memset(&rec, 0, sizeof(rec));
  rec.recovery_id = 0; /* assigned by rec_tree_add */
  rec.deleted_at = (uint64_t)time(NULL);
  rec.original_parent_ino = parent_dir.inode.inode_number;
  strncpy(rec.original_name, basename, sizeof(rec.original_name) - 1);
  rec.original_name[sizeof(rec.original_name) - 1] = '\0';

  rec.inode_number = target_inode.inode_number;
  rec.file_type = target_inode.file_type;
  rec.uid = target_inode.uid;
  rec.gid = target_inode.gid;
  rec.permissions = target_inode.permissions;
  rec.file_size = target_inode.file_size;
  rec.block_count = target_inode.block_count;
  memcpy(rec.direct_blocks, target_inode.direct_blocks,
         sizeof(rec.direct_blocks));
  rec.btree_root = target_inode.btree_root;
  rec.retention_until = rec.deleted_at + DEFAULT_RECOVERY_TTL;

  /* 4) Insert into recovery tree */
  if (!rec_tree_add(disk, sb, rec_tree, &rec))
    return -1;

  /* 5) Remove directory entry */
  if (remove_dir_entry(disk, sb, &parent_dir, basename) != 0) {
    /* rollback: if dir removal fails, we should also remove recovery entry */
    (void)rec_tree_remove(disk, sb, rec_tree, rec.recovery_id);
    return -1;
  }

  /* 6) Remove inode from inode tree (blocks not freed) */
  if (!remove_inode(disk, sb, inode_tree, target_inode.inode_number)) {
    /* rollback recovery entry if inode removal fails */
    (void)rec_tree_remove(disk, sb, rec_tree, rec.recovery_id);
    return -1;
  }

  /* update parent dir metadata */
  parent_dir.inode.modified_at = (uint32_t)time(NULL);
  (void)update_inode(disk, inode_tree, &parent_dir.inode);

  return 0;
}
