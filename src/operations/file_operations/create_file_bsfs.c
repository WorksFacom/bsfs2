/**
 * @file create_file_bsfs.c
 * @brief Implementation of file creation routines in the BSFS filesystem.
 *
 * This module provides the implementation of functions to create new
 * empty files in the BSFS filesystem. A new inode is initialized,
 * inserted into the inode B-tree, and linked into its parent directory
 * via a directory entry. No data blocks are allocated at creation time;
 * they are allocated on demand during writes.
 *
 * Responsibilities:
 *  - Resolve parent directory and basename from a path.
 *  - Ensure that duplicate names are not allowed in the parent directory.
 *  - Allocate and initialize a fresh inode with zero size and no blocks.
 *  - Insert the inode into the inode B-tree.
 *  - Add the directory entry in the parent directory.
 *  - Update parent directory metadata (timestamps).
 *
 * This file is part of the higher-level file management interface for BSFS.
 */
#include "../../include/create_file_bsfs.h"

/**
 * @brief Creates an empty regular file at the given path.
 *
 * This function initializes a fresh @ref inode_t (size = 0, no data blocks),
 * inserts it into the inode B-tree, and then adds a directory entry in the
 * parent directory resolved from @p path. It prevents duplicate names in the
 * parent directory and updates the parent's metadata (timestamps).
 *
 * Workflow:
 *  1) Resolve the parent directory and basename from @p path (relative to @p
 * cwd if needed) using resolve_parent_dir(). 2) Allocate a new inode number and
 * initialize a zero-sized regular file inode. 3) Insert the inode into the
 * inode B-tree via insert_inode(). 4) Build a dir_entry_t and insert it into
 * the parent directory using add_dir_entry(). 5) Update the parent directory's
 * timestamps and persist them via update_inode().
 *
 * Notes:
 *  - This routine does NOT allocate any data blocks; writes will allocate on
 * demand.
 *  - On failure to add the directory entry, the newly inserted inode is removed
 *    from the inode B-tree (basic rollback).
 *
 * @param disk         Open filesystem image.
 * @param sb           Loaded superblock.
 * @param inode_tree   Handle for the inode B-tree.
 * @param path         Absolute or relative path of the new file.
 * @param cwd          Current working directory for resolving relative paths.
 * @param uid          Owner user ID for the new inode.
 * @param gid          Owner group ID for the new inode.
 * @param permissions  Mode bits (e.g., 0644).
 *
 * @return 0 on success; -1 on error (invalid args, path resolution failure,
 *         inode insertion failure, duplicate name, or dir entry insertion
 * failure).
 */
int create_file(FILE *disk, superblock_t *sb, btree_handle_t *inode_tree,
                const char *path, dir_t *cwd, uint16_t uid, uint16_t gid,
                uint16_t permissions) {
  if (!disk || !sb || !inode_tree || !path || !cwd)
    return -1;

  /* 1) Resolve parent directory + basename */
  dir_t parent_dir;
  char basename[256] = {0};

  if (resolve_parent_dir(disk, sb, path, cwd, &parent_dir, basename,
                         sizeof(basename)) != 0)
    return -1;

  if (basename[0] == '\0')
    return -1; /* empty filename is invalid */

  /* 2) Prepare a zero-sized inode for a regular file (no data blocks allocated)
   */
  inode_t ino;
  memset(&ino, 0, sizeof(ino));

  uint32_t new_ino = get_next_inode_number(disk, inode_tree);
  if (new_ino == 0)
    return -1;

  ino.inode_number = new_ino;
  ino.file_size = 0;
  ino.file_type =
      FILE_TYPE_FILE; /* adjust to your enum/constant if different */
  ino.block_count = 0;
  ino.uid = uid;
  ino.gid = gid;
  ino.permissions = permissions;
  ino.links_count = 1; /* linked by the parent directory entry */

  uint32_t now = (uint32_t)time(NULL);
  ino.created_at = now;
  ino.modified_at = now;
  ino.accessed_at = now;

  /* No direct spans and no data B-tree root for an empty file */
  for (int i = 0; i < 16; i++) {
    ino.direct_blocks[i].file_blk = 0;
    ino.direct_blocks[i].disk_blk = 0;
    ino.direct_blocks[i].length = 0;
  }
  ino.btree_root = 0;

  /* 3) Insert inode into the inode B-tree */
  if (insert_inode(disk, sb, inode_tree, &ino) != 1)
    return -1;

  /* 4) Build dir_entry_t and insert into parent directory (add_dir_entry
   * prevents duplicates) */
  dir_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.inode_number = ino.inode_number;
  entry.file_type = ino.file_type;
  /* Adjust the name field to match your dir_entry_t definition */
  strncpy(entry.filename, basename, sizeof(entry.filename) - 1);
  entry.filename[sizeof(entry.filename) - 1] = '\0';
  /* If your dir_entry_t carries size or extra metadata, set them here (e.g.,
   * entry.file_size = 0). */

  if (add_dir_entry(disk, sb, &parent_dir, &entry) != 0) {
    /* Roll back the inode insertion to keep metadata consistent */
    (void)remove_inode(disk, sb, inode_tree, ino.inode_number);
    return -1;
  }

  /* 5) Update parent directory metadata and persist it */
  parent_dir.inode.modified_at = now;
  parent_dir.inode.accessed_at = now;
  (void)update_inode(disk, inode_tree, &parent_dir.inode);

  return 0;
}
